#pragma once

#include <stdint.h>
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_intr_alloc.h"
#include <soc/gpio_sig_map.h>  // TWAI_TX_IDX / TWAI_RX_IDX
#include <soc/periph_defs.h>    // PERIPH_TWAI_MODULE
#include <soc/interrupts.h>     // ETS_TWAI_INTR_SOURCE

/* TWAI_Lite: Minimal native CAN (TWAI) driver for the ESP32/ESP32-S3 built-in
 * controller, written in the style of the MCP2515_Lite library.

Features:
 - Non-blocking send/receive using preallocated FreeRTOS queues
 - Interrupt driven: the hardware RX FIFO is drained and queued TX frames are
   loaded in the ISR (no background task is needed, the registers are memory
   mapped unlike SPI on the MCP2515)
 - On-the-fly bit rate changes and pause/resume (stops ACKing)
 - All ESP-IDF ESP32/ESP32-S3 TWAI errata workarounds are included:
     * Bus-off recovery can be blocked by a non-zero REC
                                      (CONFIG_TWAI_ERRATA_FIX_BUS_OFF_REC)
     * Transmit interrupt lost when read on the same APB cycle
                                      (CONFIG_TWAI_ERRATA_FIX_TX_INTR_LOST)
     * Invalid RX frame after a bus error in the data/CRC field
                                      (CONFIG_TWAI_ERRATA_FIX_RX_FRAME_INVALID)
     * RX FIFO corruption once 64 messages are queued
                                      (CONFIG_TWAI_ERRATA_FIX_RX_FIFO_CORRUPT)
     * Dominant error frame sent while in listen only mode
                                      (CONFIG_TWAI_ERRATA_FIX_LISTEN_ONLY_DOM)
 - Direct register access, no dependency on the ESP-IDF TWAI driver

Bit rate notes:
 - The TWAI module clock is the APB clock (80 MHz on most boards).
 - The peripheral prescales BRP by 2, so the BRP register field holds
   (prescaler / 2 - 1) and only even prescalers are valid.

The frame layout matches MCP2515_Lite_Frame/CAN_frame so frames can be
converted with a simple memcpy. The `flags` byte bit 1 marks RTR frames
(bit 0 is the CAN-FD flag, always false on the native TWAI controller).
*/

// Queue depths (in messages)
#define TWAI_LITE_TX_QUEUE_DEPTH 25
#define TWAI_LITE_RX_QUEUE_DEPTH 25

typedef struct {
  union {
    bool fd;        // CAN-FD flag, always false (native TWAI is classic CAN only)
    uint8_t flags;  // Bit 1 set = RTR (remote) frame
  };
  bool ext;         // false -> standard (11 bit) frame, true -> extended (29 bit)
  uint8_t dlc;      // Data length (0...8)
  uint32_t id;      // Frame identifier
  uint8_t data[8];  // Payload
} TWAI_Lite_Frame;

typedef struct {
  uint32_t bitrate;  // Bits per second, e.g. 500000
} TWAI_Lite_Speed;

class TWAI_Lite {
 public:
  TWAI_Lite();
  ~TWAI_Lite();

  // (Re)initialises the controller. Can be called again to change the bit rate.
  bool begin(const TWAI_Lite_Speed& speed, gpio_num_t tx_pin, gpio_num_t rx_pin,
             bool loopback = false, bool listen_only = false);

  // Stops the controller and frees the ISR
  void end();

  // Non-blocking: transmits immediately if the hardware buffer is free and the
  // controller is running, otherwise queues the message
  bool sendFrame(const TWAI_Lite_Frame& msg);

  // Non-blocking: pops a message from the RX queue
  bool receiveFrame(TWAI_Lite_Frame& msg);

  // Non-blocking: reconfigures the controller at the new bit rate
  void changeSpeed(const TWAI_Lite_Speed& new_speed);

  // Non-blocking: pauses all communication (and stops acknowledging messages)
  void pause(bool paused);

  // Bitmask returned by errorFlags(): the causes that latched the error
  // status. Latched read-clear, so a bus-off event is still reported here
  // even though the live busOff() bit is usually already cleared by the time
  // the application polls (the ISR's recovery workaround brings the bus back
  // first).
  enum : uint8_t {
    ERR_EWL     = 1 << 0,  // Hardware error-warning level: TEC or REC >= 96 (ES)
    ERR_BUS_OFF = 1 << 1,  // Entered bus-off (BS)
    ERR_RX_EWL  = 1 << 2,  // Software REC mirror (rxHealth) reached EWL 96
  };

  // Read-clear bitmask of what set the error status since the last call.
  // More granular than hasErrors(), which is equivalent to errorFlags() != 0.
  // The read-modify-clear runs under the spinlock so a set latched by the ISR
  // between the read and the clear cannot be lost.
  inline uint8_t errorFlags() {
    portENTER_CRITICAL(&_spinlock);
    const uint8_t f = _error_flags;
    _error_flags = 0;
    portEXIT_CRITICAL(&_spinlock);
    return f;
  }

  // Latch set when the controller reports a persistent error condition: the
  // error warning level (TEC or REC >= EWL 96), bus-off — or the software REC
  // mirror (rxHealth) reaching EWL — cleared on read (see errorFlags() for
  // the cause). Single transient bus errors do NOT latch it, matching
  // MCP2515_Lite's EFLG (EWARN|TXBO) check and the ESP-IDF ABOVE_ERR_WARN /
  // BUS_OFF alerts. Errata peripheral resets (frame loss) are NOT counted as
  // a bus error; track them with periphResetCount() instead.
  inline bool hasErrors() { return errorFlags() != 0; }

  // Latch set when received frames were lost (hardware FIFO data overrun or
  // the RX queue was full), cleared on read. Not a bus error: use for
  // telemetry/drops, not for the bus-error health flag.
  inline bool rxOverflow() {
    portENTER_CRITICAL(&_spinlock);
    const bool ovf = _rx_overflow;
    _rx_overflow = false;
    portEXIT_CRITICAL(&_spinlock);
    return ovf;
  }

  // Cumulative number of errata peripheral resets performed by the ISR (each
  // drops all pending RX frames). Diagnostics: this is the metric for
  // sustained bus trouble, not a bus-error flag.
  inline uint32_t periphResetCount() const { return _periph_reset_count; }

  // Live error counters (TEC = transmit errors, REC = receive errors)
  inline uint16_t tec() const { return *reg(0x03C) & 0xFF; }  // TX error counter
  inline uint16_t rec() const { return *reg(0x038) & 0xFF; }  // RX error counter

  // Software mirror of the hardware REC, which the errata resets keep zeroing:
  // +8 per RX-direction bus error, -1 per successfully received frame
  // (0..255, EWL = 96). Unlike rec(), this one reflects sustained RX
  // corruption even though the resets prevent the hardware counter from
  // climbing. Reset by begin(), preserved across changeSpeed()/pause().
  inline uint16_t rxHealth() const { return _rec_sw; }

  // Programmed bit timing (diagnostics)
  inline uint32_t bitRatePrescaler() const { return _brp; }
  inline uint8_t seg1() const { return _tseg1; }
  inline uint8_t seg2() const { return _tseg2; }
  inline uint8_t syncJumpWidth() const { return _sjw; }

  // Last bus error captured by the ISR from the ECC register (diagnostics).
  // type: 0=bit, 1=form, 2=stuff, 3=other; dir: 0=TX, 1=RX;
  // seg: ECC segment (8=CRC sequence, 10=data, 11=DLC, 25=ACK slot,
  // 27=ACK delimiter, 17=active error flag, ...).
  inline uint8_t lastErrorType() const { return (_last_ecc >> 6) & 0x03; }
  inline uint8_t lastErrorDir() const { return (_last_ecc >> 5) & 0x01; }
  inline uint8_t lastErrorSeg() const { return _last_ecc & 0x1F; }

  // Live bus-off status bit. Usually already false when polled after a
  // bus-off event: the ISR forces recovery (TEC re-trigger workaround) before
  // the application gets to read it. Use errorFlags() & ERR_BUS_OFF for a
  // latched "bus-off happened" signal.
  inline bool busOff() const { return (statusRegister() & 0x80) != 0; }

  // Raw TWAI status register (RBS/DOS/TBS/TCS/RS/TS/ES/BS bits)
  inline uint32_t statusRegister() const { return *reg(0x008); }

  // The bit rate the controller is currently configured for
  inline uint32_t actualBitRate() const { return _bitrate; }

 private:
  // IRAM_ATTR: this accessor and the whole ISR call chain (isr, handleInterrupt,
  // drainRxFifo, startTransfer, errataReset, configure) are placed in IRAM and
  // the ISR is allocated with ESP_INTR_FLAG_IRAM. The RX FIFO must keep draining
  // and the errata resets must stay executable while the flash cache is disabled
  // during OTA/flash writes (ESP-IDF does the same with CONFIG_TWAI_ISR_IN_IRAM).
  IRAM_ATTR inline volatile uint32_t* reg(uint32_t offset) const {
    return (volatile uint32_t*)(_base + offset);
  }

  static IRAM_ATTR void isr(void* arg);
  IRAM_ATTR void handleInterrupt();     // ISR body: RX drain, TX kick, errata handling
  IRAM_ATTR void drainRxFifo();         // Move all hardware FIFO frames into the RX queue
  IRAM_ATTR void startTransfer(const TWAI_Lite_Frame& frame);  // Load HW TX buffer + send
  void kickTx();              // Restart TX after a reconfiguration (task context only)
  IRAM_ATTR void configure();           // Write all config registers, then exit reset mode
  IRAM_ATTR void errataReset();         // Hardware reset of the peripheral + restore config

  uint32_t _base;
  bool _loopback;
  bool _listen_only;
  uint32_t _brp;
  uint8_t _tseg1;
  uint8_t _tseg2;
  uint8_t _sjw;
  uint32_t _bitrate;

  volatile bool _tx_pending;    // A frame is currently in the hardware TX buffer
  volatile bool _running;       // Controller is out of reset mode (TX path may run)
  volatile bool _paused;
  volatile bool _rx_overflow;
  volatile uint8_t _error_flags;  // errorFlags() latch: ERR_EWL|ERR_BUS_OFF|ERR_RX_EWL
  volatile uint32_t _periph_reset_count;  // errata peripheral resets (diagnostics)
  volatile uint8_t _last_ecc;   // Last ECC register capture (diagnostics)
  volatile uint8_t _rec_sw;     // Software REC mirror (EWL source, see rxHealth)
  TWAI_Lite_Frame _tx_current;  // Copy of the in-flight frame (errata TX retry)

  QueueHandle_t _tx_queue;
  QueueHandle_t _rx_queue;
  intr_handle_t _isr_handle;

  static portMUX_TYPE _spinlock;
};
