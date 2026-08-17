#include "twai_lite.h"

#include <string.h>

//--- Header for periph_module_enable/reset/disable
#include <esp_private/periph_ctrl.h>
#include <soc/soc.h>
//--- IRAM-safe FreeRTOS queues (xQueueCreateWithCaps) for the IRAM ISR
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>

//------------------------------------------------------------------------------
//   Register map (extended/PeliCAN layout, offsets from the TWAI base address)
//------------------------------------------------------------------------------

#define REG_MODE        0x000  // reset/listen-only/self-test/filter mode bits
#define REG_CMD         0x004  // transmit/release/clear-overrun command bits
#define REG_STATUS      0x008  // RBS/DOS/TBS/TCS/RS/TS/ES/BS
#define REG_INT_RAW     0x00C  // reading clears the latched interrupts (except RI)
#define REG_INT_ENA     0x010
#define REG_BTR0        0x018  // BRP (divided by 2 in silicon) + SJW
#define REG_BTR1        0x01C  // TSEG1 + TSEG2 + sampling
#define REG_ECC         0x030  // error code capture (type/direction/segment)
#define REG_EWL         0x034
#define REG_REC         0x038
#define REG_TEC         0x03C
#define REG_FRAME_INFO  0x040  // also acceptance code register 0
#define REG_ID_0        0x044  // also ACR1
#define REG_ID_1        0x048  // also ACR2
#define REG_ID_2        0x04C  // also ACR3 / SFF data byte 0
#define REG_ID_3        0x050  // also AMR0 / SFF data byte 1
#define REG_AMR0        0x050
#define REG_RX_MSG_CNT  0x074
#define REG_CDR         0x07C  // clock divider (bit 7 = extended layout on ESP32)

// SFF data bytes live at 0x04C + 4*i, EFF data bytes at 0x054 + 4*i
#define DATA_SFF(i) (0x04C + 4 * (i))
#define DATA_EFF(i) (0x054 + 4 * (i))
// Acceptance code registers 0x040 + 4*i, mask registers 0x050 + 4*i
#define ACR(i) (0x040 + 4 * (i))
#define AMR(i) (0x050 + 4 * (i))

//--- Mode register bits
#define MODE_RM   0x01  // reset mode
#define MODE_LOM  0x02  // listen only mode
#define MODE_STM  0x04  // self test mode (loopback)
#define MODE_AFM  0x08  // single acceptance filter mode

//--- Command register bits (bits are latched, only ever set with |=
#define CMD_TR    0x01  // transmit request
#define CMD_AT    0x02  // abort transmission
#define CMD_RRB   0x04  // release receive buffer
#define CMD_CDO   0x08  // clear data overrun
#define CMD_SRR   0x10  // self reception request (loopback transmission)

//--- Status register bits
#define STATUS_RBS  0x01  // receive buffer status (FIFO non-empty)
#define STATUS_DOS  0x02  // data overrun status
#define STATUS_TBS  0x04  // transmit buffer status (buffer free)
#define STATUS_ES   0x40  // error status (TEC or REC >= EWL)
#define STATUS_BS   0x80  // bus status (bus-off)

//--- Interrupt bits
#define INT_RI    0x01  // receive
#define INT_TI    0x02  // transmit
#define INT_EI    0x04  // error (warning limit crossed / bus-off entered)
#define INT_BEI   0x80  // bus error
#define INT_ENABLED (INT_RI | INT_TI | INT_EI | INT_BEI)

//--- Frame info byte bits
#define FRAME_RTR  0x40
#define FRAME_EFF  0x80

//--- Error code capture fields
#define ECC_TYPE_OTHER  0xC0  // error type 3 (other)
#define ECC_DIR_RX      0x20
#define ECC_SEG_MASK    0x1F
// Segments that mark the invalid-RX-frame errata condition
#define ECC_SEG_CRC_SEQ  8
#define ECC_SEG_DATA     10
#define ECC_SEG_ACK_DELIM 27

//--- Errata: the RX FIFO becomes unrecoverable at 64 messages; reset at 62
#define RX_FIFO_CORRUPT_THRESH 62

//--- The SJW field position differs: ESP32 has it at 7:6, ESP32S3 at 15:14
#ifdef CONFIG_IDF_TARGET_ESP32
  #define SJW_SHIFT 6
#else
  #define SJW_SHIFT 14
#endif

//--- TWAI module base address and hardware reset register (target specific)
#ifdef CONFIG_IDF_TARGET_ESP32
  #include <soc/dport_reg.h>
  #define TWAI_BASE DR_REG_CAN_BASE
  #define TWAI_HW_RESET()                                                \
    do {                                                                 \
      /* RCC register writes are shared across peripherals/cores: take   */ \
      /* the same spinlock ESP-IDF uses (TWAI_RCC_ATOMIC) so a reset     */ \
      /* from this ISR cannot race a concurrent RCC write                */ \
      PERIPH_RCC_ATOMIC() {                                              \
        DPORT_WRITE_PERI_REG(DPORT_PERIP_RST_EN_REG, DPORT_TWAI_RST);    \
        DPORT_WRITE_PERI_REG(DPORT_PERIP_RST_EN_REG, 0);                 \
      }                                                                  \
    } while (0)
#else
  #include <soc/system_reg.h>
  #define TWAI_BASE DR_REG_TWAI_BASE
  #define TWAI_HW_RESET()                                         \
    do {                                                          \
      PERIPH_RCC_ATOMIC() {                                       \
        SET_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_TWAI_RST); \
        CLEAR_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_TWAI_RST); \
      }                                                           \
    } while (0)
#endif

//------------------------------------------------------------------------------

portMUX_TYPE TWAI_Lite::_spinlock = portMUX_INITIALIZER_UNLOCKED;

//------------------------------------------------------------------------------
//   Bit timing calculation
//------------------------------------------------------------------------------

// The TWAI module clock is the APB clock. The peripheral divides the BRP by 2,
// so the register field is (brp / 2 - 1) with brp even. This searches for an
// even prescaler giving 8..25 time quanta per bit, preferring an exact rate and
// a sample point close to 75%.
static bool calcTiming(uint32_t bitrate, uint32_t& brp, uint8_t& tseg1, uint8_t& tseg2, uint8_t& sjw) {
  const uint32_t clk = getApbFrequency();
  uint32_t best_brp = 0;
  uint32_t best_tq = 0;
  uint32_t best_err = UINT32_MAX;
  uint32_t best_dist = UINT32_MAX;
  for (uint32_t b = 2; b <= 128; b += 2) {
    const uint32_t tq = (clk + (b * bitrate) / 2) / (b * bitrate);  // rounded TQ
    if (tq < 8 || tq > 25) continue;
    const uint32_t w = b * bitrate * tq;
    const uint32_t err = (clk > w) ? (clk - w) : (w - clk);
    const uint32_t dist = (tq > 16) ? (tq - 16) : (16 - tq);
    // Lowest rate error wins; on a tie prefer TQ closest to 16 (8..25 TQ, and
    // 16 TQ maps cleanly onto the 87.5% sample point split below)
    if (err < best_err || (err == best_err && dist < best_dist)) {
      best_err = err;
      best_brp = b;
      best_tq = tq;
      best_dist = dist;
    }
  }
  if (best_brp == 0) return false;
  brp = best_brp;
  // ~87.5% sample point (1/8 of the bit after the sample point), like the
  // ACAN_ESP32 default. Later sampling is more tolerant of propagation delays
  // and bit-sampling margins on marginal buses than the ~75% ESP-IDF default.
  uint32_t t2 = (best_tq + 4) / 8;
  if (t2 < 1) t2 = 1;
  uint32_t t1 = best_tq - 1 - t2;
  if (t1 > 16) { t1 = 16; t2 = best_tq - 1 - t1; }  // TSEG1 register limit
  if (t2 > 8) t2 = 8;                                 // TSEG2 register limit
  tseg1 = t1;
  tseg2 = t2;
  sjw = (t2 > 3) ? 3 : t2;
  return true;
}

//------------------------------------------------------------------------------
//   Constructor / destructor
//------------------------------------------------------------------------------

TWAI_Lite::TWAI_Lite()
    : _base(TWAI_BASE), _loopback(false), _listen_only(false),
      _brp(0), _tseg1(0), _tseg2(0), _sjw(0), _bitrate(0),
      _tx_pending(false), _running(false), _paused(false), _rx_overflow(false), _error_flags(0),
      _periph_reset_count(0), _last_ecc(0), _rec_sw(0),
      _tx_queue(nullptr), _rx_queue(nullptr), _isr_handle(nullptr) {
  memset(&_tx_current, 0, sizeof(_tx_current));
}

TWAI_Lite::~TWAI_Lite() {
  end();
  if (_tx_queue != nullptr) {
    vQueueDeleteWithCaps(_tx_queue);
    _tx_queue = nullptr;
  }
  if (_rx_queue != nullptr) {
    vQueueDeleteWithCaps(_rx_queue);
    _rx_queue = nullptr;
  }
}

//------------------------------------------------------------------------------
//   Initialisation
//------------------------------------------------------------------------------

bool TWAI_Lite::begin(const TWAI_Lite_Speed& speed, gpio_num_t tx_pin, gpio_num_t rx_pin, bool loopback,
                      bool listen_only) {
  uint32_t brp;
  uint8_t tseg1, tseg2, sjw;
  if (!calcTiming(speed.bitrate, brp, tseg1, tseg2, sjw)) {
    return false;  // Bit rate not achievable with this APB clock, keep old config
  }
  _brp = brp;
  _tseg1 = tseg1;
  _tseg2 = tseg2;
  _sjw = sjw;
  _bitrate = speed.bitrate;
  _loopback = loopback;
  _listen_only = listen_only;

  // Queues must live in internal RAM: the IRAM ISR runs with the flash cache
  // disabled during OTA/flash writes and must never touch PSRAM. Created once,
  // reused on re-initialisation.
  if (_tx_queue == nullptr) _tx_queue = xQueueCreateWithCaps(TWAI_LITE_TX_QUEUE_DEPTH, sizeof(TWAI_Lite_Frame), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (_rx_queue == nullptr) _rx_queue = xQueueCreateWithCaps(TWAI_LITE_RX_QUEUE_DEPTH, sizeof(TWAI_Lite_Frame), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (_tx_queue == nullptr || _rx_queue == nullptr) return false;

  portENTER_CRITICAL(&_spinlock);
  _tx_pending = false;
  _running = false;   // set true again by configure() once the controller exits reset mode
  _paused = false;
  _error_flags = 0;
  _rx_overflow = false;
  _rec_sw = 0;

  // Enable the clock, reset the peripheral and route the GPIO pins
  periph_module_enable(PERIPH_TWAI_MODULE);
  periph_module_reset(PERIPH_TWAI_MODULE);
  pinMode(tx_pin, OUTPUT);
  pinMatrixOutAttach(tx_pin, TWAI_TX_IDX, false, false);
  pinMode(rx_pin, INPUT);
  pinMatrixInAttach(rx_pin, TWAI_RX_IDX, false);

  // Enter reset mode (required to write the configuration registers)
  *reg(REG_MODE) = MODE_RM;
  while (!(*reg(REG_MODE) & MODE_RM)) *reg(REG_MODE) = MODE_RM;
  portEXIT_CRITICAL(&_spinlock);

  // Install the ISR before the interrupts are enabled by configure(). The ISR
  // chain is IRAM-resident and allocated with ESP_INTR_FLAG_IRAM so it keeps
  // draining the RX FIFO (and can execute the errata resets) while the flash
  // cache is disabled during OTA writes — exactly how ESP-IDF deploys its TWAI
  // ISR. Without IRAM, a CAN interrupt during a flash write either stalls until
  // the write ends or starts executing garbage if the cache drops mid-ISR;
  // the latter desynchronises the FIFO drain (releases without reads) so later
  // frames are read from wrong FIFO slots: valid controller CRC, corrupt data.
  if (_isr_handle == nullptr) {
    if (esp_intr_alloc(ETS_TWAI_INTR_SOURCE, ESP_INTR_FLAG_IRAM, isr, this, &_isr_handle) != ESP_OK) {
      // Leave the controller stopped rather than half-running without an ISR
      portENTER_CRITICAL(&_spinlock);
      *reg(REG_INT_ENA) &= 0x10;
      *reg(REG_MODE) = MODE_RM;
      portEXIT_CRITICAL(&_spinlock);
      return false;
    }
  }

  // Write the configuration and start the controller
  portENTER_CRITICAL(&_spinlock);
  configure();
  portEXIT_CRITICAL(&_spinlock);
  return true;
}

void TWAI_Lite::end() {
  _running = false;  // the ISR must not touch the hardware beyond this point
  if (_isr_handle != nullptr) {
    esp_intr_free(_isr_handle);
    _isr_handle = nullptr;
  }
  portENTER_CRITICAL(&_spinlock);
  *reg(REG_INT_ENA) &= 0x10;  // disable all TWAI interrupts (keep ESP32 brp_div bit)
  *reg(REG_CMD) |= CMD_AT;    // abort any transmission
  *reg(REG_MODE) = MODE_RM;   // back to reset mode
  periph_module_disable(PERIPH_TWAI_MODULE);
  portEXIT_CRITICAL(&_spinlock);
}

//------------------------------------------------------------------------------
//   Configuration
//------------------------------------------------------------------------------

// Writes all configuration registers. Must be called in reset mode; exits reset
// mode at the end to start the controller.
void TWAI_Lite::configure() {
  // Extended register layout (only ESP32 has the basic/extended switch; the
  // ESP32S3 is always in the extended layout)
#ifdef CONFIG_IDF_TARGET_ESP32
  *reg(REG_CDR) = 0x80;  // bit 7 = extended layout, CLKOUT disabled
#else
  *reg(REG_CDR) = 0;
#endif

  // Acceptance filter: accept everything (single filter mode, mask = don't care)
  for (uint8_t i = 0; i < 4; i++) {
    *reg(ACR(i)) = 0;
    *reg(AMR(i)) = 0xFF;
  }

  // Bus timing. The BRP field is divided by 2 in the silicon, and TSEG values
  // are stored minus one
  *reg(REG_BTR0) = ((_sjw - 1) << SJW_SHIFT) | ((_brp / 2) - 1);
  *reg(REG_BTR1) = ((_tseg2 - 1) << 4) | (_tseg1 - 1);

  // Error counters and warning limit
  *reg(REG_EWL) = 96;
  *reg(REG_REC) = 0;
  *reg(REG_TEC) = 0;

  // Operating mode: listen only, loopback (self test), single filter
  const uint32_t mode = (_listen_only ? MODE_LOM : 0) | (_loopback ? MODE_STM : 0) | MODE_AFM;
  *reg(REG_MODE) = mode;

  // Interrupts: RX, TX, error, bus error (preserve the ESP32 brp_div bit)
  *reg(REG_INT_ENA) = (*reg(REG_INT_ENA) & 0x10) | INT_ENABLED;
  (void)*reg(REG_INT_RAW);  // clear any latched interrupts

  // Errata: in listen only mode the controller must not send dominant bits.
  // Setting REC to 128 forces it error passive (TWAI_ERRATA_FIX_LISTEN_ONLY_DOM)
  if (_listen_only) {
    *reg(REG_REC) = 128;
  }

  // Start the controller (exit reset mode)
  *reg(REG_MODE) = mode;
  while (*reg(REG_MODE) & MODE_RM) {
    *reg(REG_MODE) = mode;
  }
  _running = true;  // controller is out of reset mode; the ISR TX path may run
}

// Hardware reset of the TWAI peripheral. Needed by the invalid-RX-frame and
// RX-FIFO-corruption errata: the corrupted FIFO state can only be cleared by a
// hardware reset, not by rewriting the registers.
void TWAI_Lite::errataReset() {
  // Decide whether to re-send the in-flight frame from the hardware TX buffer
  // status BEFORE entering reset mode, like ESP-IDF
  // (twai_hal_prepare_for_reset()): if the buffer is already free the frame
  // completed and must not be sent again. Worst case it completes right after
  // this check and is sent twice, exactly as in ESP-IDF.
  const bool retry_tx = _tx_pending && !(*reg(REG_STATUS) & STATUS_TBS);
  *reg(REG_MODE) = MODE_RM;  // enter reset mode (stops the controller)
  while (!(*reg(REG_MODE) & MODE_RM)) *reg(REG_MODE) = MODE_RM;
  TWAI_HW_RESET();           // resets the peripheral into reset mode
  configure();               // rewrite config and restart
  if (retry_tx) {
    // The in-flight frame was cancelled by the reset; send it again
    startTransfer(_tx_current);
    _tx_pending = true;
  } else {
    // Frame already completed (or nothing in flight): the buffer is empty
    _tx_pending = false;
  }
}

//------------------------------------------------------------------------------
//   Transmission
//------------------------------------------------------------------------------

bool TWAI_Lite::sendFrame(const TWAI_Lite_Frame& msg) {
  portENTER_CRITICAL(&_spinlock);
  bool ok;
  if (_running && !_tx_pending) {
    // Hardware TX buffer is free and the controller is running: transmit now
    startTransfer(msg);
    _tx_pending = true;
    ok = true;
  } else {
    // Paused (no traffic wanted) or buffer busy: queue it. The frame is
    // handed to the hardware by the ISR, or by kickTx() once running again.
    ok = xQueueSend(_tx_queue, &msg, 0) == pdTRUE;
  }
  portEXIT_CRITICAL(&_spinlock);
  return ok;
}

void TWAI_Lite::startTransfer(const TWAI_Lite_Frame& f) {
  _tx_current = f;  // keep a copy in case an errata reset aborts the transfer
  const uint8_t dlc = (f.dlc > 8) ? 8 : f.dlc;
  const bool rtr = (f.flags & 0x02) != 0;

  *reg(REG_FRAME_INFO) = (f.ext ? FRAME_EFF : 0) | (rtr ? FRAME_RTR : 0) | dlc;
  if (f.ext) {  // Extended frame: 29 bit ID + data at 0x054
    *reg(REG_ID_0) = f.id >> 21;
    *reg(REG_ID_1) = f.id >> 13;
    *reg(REG_ID_2) = f.id >> 5;
    *reg(REG_ID_3) = f.id << 3;
    if (!rtr) {
      for (uint8_t i = 0; i < dlc; i++) *reg(DATA_EFF(i)) = f.data[i];
    }
  } else {  // Standard frame: 11 bit ID + data at 0x04C
    *reg(REG_ID_0) = f.id >> 3;
    *reg(REG_ID_1) = f.id << 5;
    if (!rtr) {
      for (uint8_t i = 0; i < dlc; i++) *reg(DATA_SFF(i)) = f.data[i];
    }
  }
  // The transmit command must be written last so it is not overwritten
  *reg(REG_CMD) |= _loopback ? CMD_SRR : CMD_TR;
}

// Called after a reconfiguration: resume transmitting the aborted in-flight
// frame, or start the next queued frame
void TWAI_Lite::kickTx() {
  if (_tx_pending) {
    startTransfer(_tx_current);
  } else {
    TWAI_Lite_Frame next;
    if (xQueueReceive(_tx_queue, &next, 0) == pdTRUE) {
      startTransfer(next);
      _tx_pending = true;
    }
  }
}

void TWAI_Lite::changeSpeed(const TWAI_Lite_Speed& new_speed) {
  uint32_t brp;
  uint8_t tseg1, tseg2, sjw;
  if (!calcTiming(new_speed.bitrate, brp, tseg1, tseg2, sjw)) return;  // keep old speed
  portENTER_CRITICAL(&_spinlock);
  _brp = brp;
  _tseg1 = tseg1;
  _tseg2 = tseg2;
  _sjw = sjw;
  _bitrate = new_speed.bitrate;
  *reg(REG_CMD) |= CMD_AT;  // abort the in-flight frame
  *reg(REG_MODE) = MODE_RM;
  while (!(*reg(REG_MODE) & MODE_RM)) *reg(REG_MODE) = MODE_RM;
  (void)*reg(REG_INT_RAW);  // drop TX-complete/bus-error interrupts latched by
                            // the abort so the ISR cannot act on them later
  configure();
  kickTx();
  portEXIT_CRITICAL(&_spinlock);
}

void TWAI_Lite::pause(bool paused) {
  portENTER_CRITICAL(&_spinlock);
  if (paused && !_paused) {
    *reg(REG_CMD) |= CMD_AT;
    *reg(REG_MODE) = MODE_RM;  // reset mode stops RX/TX and stops ACKing
    while (!(*reg(REG_MODE) & MODE_RM)) *reg(REG_MODE) = MODE_RM;
    (void)*reg(REG_INT_RAW);   // drop interrupts latched by the abort/reset so
                               // a stale TI cannot trigger the ISR TX path
    _running = false;          // the ISR TX path must not touch the hardware
    _tx_pending = false;       // the in-flight frame is lost
    _paused = true;
  } else if (!paused && _paused) {
    configure();               // exits reset mode, sets _running = true
    kickTx();
    _paused = false;
  }
  portEXIT_CRITICAL(&_spinlock);
}

//------------------------------------------------------------------------------
//   Reception
//------------------------------------------------------------------------------

bool TWAI_Lite::receiveFrame(TWAI_Lite_Frame& msg) {
  return xQueueReceive(_rx_queue, &msg, 0) == pdTRUE;
}

void TWAI_Lite::drainRxFifo() {
  BaseType_t woken = pdFALSE;
  if (*reg(REG_STATUS) & STATUS_DOS) {
    // Data overrun: a frame was lost because the FIFO was full
    _rx_overflow = true;
    *reg(REG_CMD) |= CMD_CDO;
  }
  while (*reg(REG_RX_MSG_CNT) > 0) {
    TWAI_Lite_Frame f;
    // Valid frames decrement the REC
    _rec_sw = (_rec_sw > 0) ? (uint8_t)(_rec_sw - 1) : 0;
    f.flags = 0;
    const uint32_t info = *reg(REG_FRAME_INFO);
    const bool ext = (info & FRAME_EFF) != 0;
    const bool rtr = (info & FRAME_RTR) != 0;
    f.ext = ext;
    f.dlc = (info & 0x0F) > 8 ? 8 : (info & 0x0F);
    if (ext) {  // Extended frame: 29 bit ID (reads masked to the 8-bit HW field)
      f.id = ((*reg(REG_ID_0) & 0xFF) << 21) | ((*reg(REG_ID_1) & 0xFF) << 13) |
             ((*reg(REG_ID_2) & 0xFF) << 5) | ((*reg(REG_ID_3) & 0xFF) >> 3);
      for (uint8_t i = 0; i < f.dlc; i++) f.data[i] = *reg(DATA_EFF(i));
    } else {  // Standard frame: 11 bit ID
      f.id = ((*reg(REG_ID_0) & 0xFF) << 3) | ((*reg(REG_ID_1) & 0xFF) >> 5);
      for (uint8_t i = 0; i < f.dlc; i++) f.data[i] = *reg(DATA_SFF(i));
    }
    if (rtr) {  // RTR frames carry no data
      f.flags = 0x02;
      for (uint8_t i = 0; i < 8; i++) f.data[i] = 0;  // no memset: it is flash-backed
    }
    *reg(REG_CMD) |= CMD_RRB;  // release the buffer (rotates the FIFO)
    if (xQueueSendFromISR(_rx_queue, &f, &woken) != pdTRUE) {
      _rx_overflow = true;
    }
  }
  if (woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

//------------------------------------------------------------------------------
//   Interrupt handler
//------------------------------------------------------------------------------

void TWAI_Lite::isr(void* arg) {
  static_cast<TWAI_Lite*>(arg)->handleInterrupt();
}

void TWAI_Lite::handleInterrupt() {
  portENTER_CRITICAL_ISR(&_spinlock);
  BaseType_t woken = pdFALSE;

  // Reading the interrupt register clears the latched interrupts (except RI)
  const uint32_t intr = *reg(REG_INT_RAW);

  //--- Errata checks (before the RX drain: the reset invalidates pending RX)
  bool reset_periph = false;

  // Errata: invalid RX frame. If a bus error occurred in the data or CRC field
  // of a received frame, the data of the next received frame can be invalid.
  // Reset the peripheral to clear the corrupted state
  // (TWAI_ERRATA_FIX_RX_FRAME_INVALID)
  if (intr & INT_BEI) {
    const uint32_t ecc = *reg(REG_ECC);  // reading ECC rearms the bus error interrupt
    _last_ecc = ecc & 0xFF;              // diagnostics: type/dir/segment
    const uint32_t seg = ecc & ECC_SEG_MASK;
    if (ecc & ECC_DIR_RX) {
      // RX errors increment the REC by 8.
      _rec_sw = (_rec_sw > 247) ? 255 : (uint8_t)(_rec_sw + 8);
      // Latch errors if we exceed the error warning level
      if (_rec_sw >= 96) _error_flags = _error_flags | ERR_RX_EWL;
    }
    if ((ecc & ECC_DIR_RX) &&
        (seg == ECC_SEG_DATA || seg == ECC_SEG_CRC_SEQ ||
         (seg == ECC_SEG_ACK_DELIM && (ecc & ECC_TYPE_OTHER) == ECC_TYPE_OTHER))) {
      reset_periph = true;
    }
  }

  // Errata: RX FIFO corruption. Once the RX message counter maxes out at 64 the
  // FIFO is unrecoverable; reset at 62 like ESP-IDF
  // (TWAI_ERRATA_FIX_RX_FIFO_CORRUPT)
  if ((intr & INT_RI) && *reg(REG_RX_MSG_CNT) >= RX_FIFO_CORRUPT_THRESH) {
    reset_periph = true;
  }

  if (reset_periph) {
    // An errata peripheral reset is a significant event (pending frames are
    // lost) but it is NOT a persistent bus-error state, so it does not set
    // hasErrors(); count it for diagnostics instead (ESP-IDF reports it as
    // the separate TWAI_ALERT_PERIPH_RESET).
    _periph_reset_count = _periph_reset_count + 1;
    // The reset invalidates all pending RX frames and cancelled any in-flight
    // TX; errataReset() already re-sent a frame that was mid-flight (then
    // _tx_pending is true). If no frame is in flight now, a TX may have
    // completed just before the reset (its latched interrupt was consumed by
    // the INT_RAW read above) or frames are simply queued — advance the TX
    // pipeline (ESP-IDF likewise still processes its TX buffer free event
    // after a peripheral reset).
    errataReset();
    if (!_tx_pending) {
      TWAI_Lite_Frame next;
      if (xQueueReceiveFromISR(_tx_queue, &next, &woken) == pdTRUE) {
        startTransfer(next);
        _tx_pending = true;
      }
    }
  } else {
    //--- Receive: drain the hardware FIFO into the RX queue
    if (intr & INT_RI) {
      drainRxFifo();
    }

    //--- Transmit complete, with the lost-TX-interrupt errata workaround. If
    //--- the TX interrupt was lost (read on the same APB cycle as it was set),
    //--- the TX buffer status bit still reports the buffer is free again
    //--- (TWAI_ERRATA_FIX_TX_INTR_LOST). Gated on _running so a stale TI
    //--- latched while paused cannot touch the hardware buffer.
    if (_running && ((intr & INT_TI) || (_tx_pending && (*reg(REG_STATUS) & STATUS_TBS)))) {
      _tx_pending = false;
      TWAI_Lite_Frame next;
      if (xQueueReceiveFromISR(_tx_queue, &next, &woken) == pdTRUE) {
        startTransfer(next);
        _tx_pending = true;
      }
    }
  }

  //--- Errors / bus-off: latch persistent conditions only — error warning
  //--- level (ES, TEC/REC >= 96) or bus-off (BS) — like MCP2515_Lite's EFLG
  //--- (EWARN|TXBO) check and the ESP-IDF ABOVE_ERR_WARN / BUS_OFF alerts.
  //--- EI also fires on the transition back below EWL, which must not latch.
  if (intr & INT_EI) {
    const uint32_t status = *reg(REG_STATUS);
    if (status & STATUS_ES) _error_flags = _error_flags | ERR_EWL;
    if (status & STATUS_BS) _error_flags = _error_flags | ERR_BUS_OFF;
    if (status & STATUS_BS) {
      // Errata: bus-off recovery requires both TEC and REC to reach 0, but the
      // REC can be left non-zero if errors keep arriving before the ISR
      // responds. Re-triggering the bus-off (TEC 0 then 255) forces REC to 0
      // (TWAI_ERRATA_FIX_BUS_OFF_REC)
      *reg(REG_TEC) = 0;
      *reg(REG_TEC) = 255;
      (void)*reg(REG_INT_RAW);  // clear the re-triggered bus-off interrupt
    }
  }

  portEXIT_CRITICAL_ISR(&_spinlock);
  if (woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}
