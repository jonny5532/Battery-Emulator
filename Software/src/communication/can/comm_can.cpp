#include "comm_can.h"
#include "../../lib/pierremolinaro-ACAN2517FD/ACAN2517FD.h"
#include "../../lib/pierremolinaro-acan-esp32/ACAN_ESP32.h"
#include "../../lib/pierremolinaro-acan2515/ACAN2515.h"
#include "CanReceiver.h"
#include "comm_can.h"
#include "src/datalayer/datalayer.h"
#include "src/devboard/safety/safety.h"
#include "src/devboard/sdcard/sdcard.h"
#include "src/devboard/utils/logging.h"

#include <esp_private/periph_ctrl.h>

#include <algorithm>
#include <map>

volatile CAN_Configuration can_config = {.battery = CAN_NATIVE,
                                         .inverter = CAN_NATIVE,
                                         .battery_double = CAN_ADDON_MCP2515,
                                         .charger = CAN_NATIVE,
                                         .shunt = CAN_NATIVE};

struct CanReceiverRegistration {
  CanReceiver* receiver;
  CAN_Speed speed;
};

static std::multimap<CAN_Interface, CanReceiverRegistration> can_receivers;

volatile bool send_ok_native = 0;
volatile bool send_ok_2515 = 0;
volatile bool send_ok_2518 = 0;

void map_can_frame_to_variable(CAN_frame* rx_frame, CAN_Interface interface);

void register_can_receiver(CanReceiver* receiver, CAN_Interface interface, CAN_Speed speed) {
  can_receivers.insert({interface, {receiver, speed}});
  DEBUG_PRINTF("CAN receiver registered, total: %d\n", can_receivers.size());
}

uint32_t init_native_can(CAN_Speed speed, gpio_num_t tx_pin, gpio_num_t rx_pin);

ACAN_ESP32_Settings* settingsespcan = nullptr;

static uint32_t QUARTZ_FREQUENCY;
SPIClass SPI2515;
uint8_t user_selected_can_addon_crystal_frequency_mhz = 0;

ACAN2515* can2515;
ACAN2515Settings* settings2515;

static ACAN2515_Buffer16 gBuffer;

static ACAN2517FDSettings::Oscillator quartz_fd_frequency;
SPIClass SPI2517;
uint8_t user_selected_canfd_addon_crystal_frequency_mhz = 0;
ACAN2517FD* canfd;
ACAN2517FDSettings* settings2517;
// 3LB: additional MCP2517FD instances (CAN2 U5, CAN3 U3)
#ifdef HW_3LB
SPIClass SPI2517_shared(VSPI);  // Both MCP2517FD chips share same SPI bus
ACAN2517FD* canfd_2 = nullptr;
ACAN2517FD* canfd_3 = nullptr;
ACAN2517FDSettings* settings2517_2 = nullptr;
ACAN2517FDSettings* settings2517_3 = nullptr;
volatile bool send_ok_2517_2 = false;
volatile bool send_ok_2517_3 = false;
#endif
bool use_canfd_as_can = false;
bool native_can_initialized = false;
//CAN logging filter settings
uint16_t user_selected_CAN_ID_cutoff_filter = 0;  //Messages below this ID will not be logged in webserver

bool init_CAN() {

  if (user_selected_can_addon_crystal_frequency_mhz > 0) {
    QUARTZ_FREQUENCY = user_selected_can_addon_crystal_frequency_mhz * 1000000UL;
  } else {
    QUARTZ_FREQUENCY = CRYSTAL_FREQUENCY_MHZ * 1000000UL;
  }

  if (user_selected_canfd_addon_crystal_frequency_mhz == 20) {
    quartz_fd_frequency = ACAN2517FDSettings::OSC_20MHz;
  } else if (user_selected_canfd_addon_crystal_frequency_mhz == 40) {
    quartz_fd_frequency = ACAN2517FDSettings::OSC_40MHz;
  } else {  // Default to 40MHz incase value invalid/not set
    quartz_fd_frequency = ACAN2517FDSettings::OSC_40MHz;
  }

  auto nativeIt = can_receivers.find(CAN_NATIVE);
  if (nativeIt != can_receivers.end()) {
    auto se_pin = esp32hal->CAN_SE_PIN();
    auto tx_pin = esp32hal->CAN_TX_PIN();
    auto rx_pin = esp32hal->CAN_RX_PIN();

    if (se_pin != GPIO_NUM_NC) {
      if (!esp32hal->alloc_pins("CAN", se_pin)) {
        return false;
      }
      pinMode(se_pin, OUTPUT);
      digitalWrite(se_pin, LOW);
    }

    if (!esp32hal->alloc_pins("CAN", tx_pin, rx_pin)) {
      return false;
    }

    const uint32_t errorCode = init_native_can(nativeIt->second.speed, tx_pin, rx_pin);
    if (errorCode == 0) {
      native_can_initialized = true;
      logging.println("Native Can ok");
      logging.print("Bit Rate prescaler: ");
      logging.println(settingsespcan->mBitRatePrescaler);
      logging.print("Time Segment 1:     ");
      logging.println(settingsespcan->mTimeSegment1);
      logging.print("Time Segment 2:     ");
      logging.println(settingsespcan->mTimeSegment2);
      logging.print("RJW:                ");
      logging.println(settingsespcan->mRJW);
      logging.print("Triple Sampling:    ");
      logging.println(settingsespcan->mTripleSampling ? "yes" : "no");
      logging.print("Actual bit rate:    ");
      logging.print(settingsespcan->actualBitRate());
      logging.println(" bit/s");
      logging.print("Exact bit rate ?    ");
      logging.println(settingsespcan->exactBitRate() ? "yes" : "no");
      logging.print("Sample point:       ");
      logging.print(settingsespcan->samplePointFromBitStart());
      logging.println("%");
    } else {
      logging.print("Error Native Can: 0x");
      logging.println(errorCode, HEX);
      return false;
    }
  }

  auto addonIt = can_receivers.find(CAN_ADDON_MCP2515);
  if (addonIt != can_receivers.end()) {
    auto cs_pin = esp32hal->MCP2515_CS();
    auto int_pin = esp32hal->MCP2515_INT();
    auto sck_pin = esp32hal->MCP2515_SCK();
    auto miso_pin = esp32hal->MCP2515_MISO();
    auto mosi_pin = esp32hal->MCP2515_MOSI();
    auto rst_pin = esp32hal->MCP2515_RST();

    if (!esp32hal->alloc_pins("CAN", cs_pin, int_pin, sck_pin, miso_pin, mosi_pin)) {
      return false;
    }

    logging.println("Dual CAN Bus (ESP32+MCP2515) selected");
    gBuffer.initWithSize(25);

    if (rst_pin != GPIO_NUM_NC) {
      pinMode(rst_pin, OUTPUT);
      digitalWrite(rst_pin, HIGH);
      delay(100);
      digitalWrite(rst_pin, LOW);
      delay(100);
      digitalWrite(rst_pin, HIGH);
      delay(100);
    }

    can2515 = new ACAN2515(cs_pin, SPI2515, int_pin);

    SPI2515.begin(sck_pin, miso_pin, mosi_pin);

    // CAN bit rate 250 or 500 kb/s
    auto bitRate = (int)addonIt->second.speed * 1000UL;

    settings2515 = new ACAN2515Settings(QUARTZ_FREQUENCY, bitRate);
    settings2515->mRequestedMode = ACAN2515Settings::NormalMode;
    const uint16_t errorCode2515 = can2515->begin(*settings2515, [] { can2515->isr(); });
    if (errorCode2515 == 0) {
      logging.println("Can ok");
    } else {
      logging.print("Error Can: 0x");
      logging.println(errorCode2515, HEX);
      set_event(EVENT_CANMCP2515_INIT_FAILURE, (uint8_t)errorCode2515);
      return false;
    }
  }

  auto fdNativeIt = can_receivers.find(CANFD_NATIVE);
  auto fdAddonIt = can_receivers.find(CANFD_ADDON_MCP2518);

  if (fdNativeIt != can_receivers.end() || fdAddonIt != can_receivers.end()) {

    auto speed = (fdNativeIt != can_receivers.end()) ? fdNativeIt->second.speed : fdAddonIt->second.speed;

    auto cs_pin = esp32hal->MCP2517_CS();
    auto int_pin = esp32hal->MCP2517_INT();
    auto sck_pin = esp32hal->MCP2517_SCK();
    auto sdo_pin = esp32hal->MCP2517_SDO();
    auto sdi_pin = esp32hal->MCP2517_SDI();

    if (!esp32hal->alloc_pins("CAN", cs_pin, int_pin, sck_pin, sdo_pin, sdi_pin)) {
      return false;
    }

    canfd = new ACAN2517FD(cs_pin, SPI2517, int_pin);

    logging.println("CAN FD add-on (ESP32+MCP2517) selected");
    SPI2517.begin(sck_pin, sdo_pin, sdi_pin);
    auto bitRate = (int)speed * 1000UL;
    settings2517 = new ACAN2517FDSettings(quartz_fd_frequency, bitRate, DataBitRateFactor::x4);
    // Arbitration bit rate: 250/500 kbit/s, data bit rate: 1/2 Mbit/s

    // ListenOnly / Normal20B / NormalFDs
    settings2517->mRequestedMode = use_canfd_as_can ? ACAN2517FDSettings::Normal20B : ACAN2517FDSettings::NormalFD;

    const uint32_t errorCode2517 = canfd->begin(*settings2517, [] { canfd->isr(); });
    canfd->poll();
    if (errorCode2517 == 0) {
      logging.print("Bit Rate prescaler: ");
      logging.println(settings2517->mBitRatePrescaler);
      logging.print("Arbitration Phase segment 1: ");
      logging.print(settings2517->mArbitrationPhaseSegment1);
      logging.print(" segment 2: ");
      logging.print(settings2517->mArbitrationPhaseSegment2);
      logging.print(" SJW: ");
      logging.println(settings2517->mArbitrationSJW);
      logging.print("Actual Arbitration Bit Rate: ");
      logging.print(settings2517->actualArbitrationBitRate());
      logging.print(" bit/s");
      logging.print(" (Exact:");
      logging.println(settings2517->exactArbitrationBitRate() ? "yes)" : "no)");
      logging.print("Arbitration Sample point: ");
      logging.print(settings2517->arbitrationSamplePointFromBitStart());
      logging.println("%");
    } else {
      logging.print("CAN-FD Configuration error 0x");
      logging.println(errorCode2517, HEX);
      set_event(EVENT_CANMCP2517FD_INIT_FAILURE, (uint8_t)errorCode2517);
      return false;
    }
  }

  // 3LB: initialize CAN2 (MCP2517FD U5) and CAN3 (MCP2517FD U3) if registered
#ifdef HW_3LB
  auto it2 = can_receivers.find(CANFD_ADDON_MCP2517_1);
  auto it3 = can_receivers.find(CANFD_ADDON_MCP2517_2);

  // Initialize shared SPI bus once if either CAN2 or CAN3 is registered
  if (it2 != can_receivers.end() || it3 != can_receivers.end()) {
    logging.println("Initializing shared SPI for 3LB MCP2517FD...");
    SPI2517_shared.begin(esp32hal->MCP2517_2_SCK(), esp32hal->MCP2517_2_SDO(), esp32hal->MCP2517_2_SDI());
    logging.printf("SPI pins: SCK=%d SDO=%d SDI=%d\n", esp32hal->MCP2517_2_SCK(), esp32hal->MCP2517_2_SDO(),
                   esp32hal->MCP2517_2_SDI());
  }

  // Initialize CAN2 (U5) - with delay for GPIO5 strapping pin to stabilize
  if (it2 != can_receivers.end()) {
    delay(100);  // GPIO5 is strapping pin, needs time to stabilize after boot
    pinMode(esp32hal->MCP2517_2_CS(), OUTPUT);
    digitalWrite(esp32hal->MCP2517_2_CS(), HIGH);  // Explicitly set CS high before init
    delay(50);

    // Try manual SPI reset sequence for MCP2517FD
    digitalWrite(esp32hal->MCP2517_2_CS(), LOW);
    SPI2517_shared.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    SPI2517_shared.transfer(0x00);  // RESET instruction
    SPI2517_shared.transfer(0x00);
    SPI2517_shared.endTransaction();
    digitalWrite(esp32hal->MCP2517_2_CS(), HIGH);
    delay(50);

    const uint32_t speed_kbps = static_cast<uint32_t>(it2->second.speed);
    logging.printf("Creating CAN2 MCP2517FD: CS=%d INT=%d\n", esp32hal->MCP2517_2_CS(), esp32hal->MCP2517_2_INT());
    canfd_2 = new ACAN2517FD(esp32hal->MCP2517_2_CS(), SPI2517_shared, esp32hal->MCP2517_2_INT());

    settings2517_2 = new ACAN2517FDSettings(quartz_fd_frequency, speed_kbps * 1000UL, DataBitRateFactor::x4);
    settings2517_2->mRequestedMode = use_canfd_as_can ? ACAN2517FDSettings::Normal20B : ACAN2517FDSettings::NormalFD;

    logging.println("Calling canfd_2->begin()...");
    const uint32_t err2 = canfd_2->begin(*settings2517_2, [] { canfd_2->isr(); });
    canfd_2->poll();
    if (err2)
      logging.printf("CAN2 MCP2517FD error 0x%lX\n", err2);
    else
      logging.println("CAN2 MCP2517FD OK");
  }

  // Initialize CAN3 (U3)
  if (it3 != can_receivers.end()) {
    const uint32_t speed_kbps = static_cast<uint32_t>(it3->second.speed);
    canfd_3 = new ACAN2517FD(esp32hal->MCP2517_3_CS(), SPI2517_shared, esp32hal->MCP2517_3_INT());

    settings2517_3 = new ACAN2517FDSettings(quartz_fd_frequency, speed_kbps * 1000UL, DataBitRateFactor::x4);
    settings2517_3->mRequestedMode = use_canfd_as_can ? ACAN2517FDSettings::Normal20B : ACAN2517FDSettings::NormalFD;

    const uint32_t err3 = canfd_3->begin(*settings2517_3, [] { canfd_3->isr(); });
    canfd_3->poll();
    if (err3)
      logging.printf("CAN3 MCP2517FD error 0x%lX\n", err3);
    else
      logging.println("CAN3 MCP2517FD OK");
  }
#endif

  return true;
}

void transmit_can_frame_to_interface(const CAN_frame* tx_frame, CAN_Interface interface) {
  if (!allowed_to_send_CAN) {
    return;
  }
  print_can_frame(*tx_frame, interface, frameDirection(MSG_TX));

  if (datalayer.system.info.CAN_SD_logging_active) {
    add_can_frame_to_buffer(*tx_frame, frameDirection(MSG_TX));
  }

  // If compiled with USE_CANFD_INTERFACE_AS_CLASSIC_CAN, treat any call that
  // wants to send to CAN_NATIVE as a send to the MCP2517FD U5 (CANFD_ADDON_MCP2517_1).
  // This makes modules that call CAN_NATIVE operate over the selected FD addon.
  // Runtime-controlled mapping: if user enabled mapping in settings, treat CAN_NATIVE as MCP2517 U5
  // Fall back to compile-time define if present.
#if defined(USE_CANFD_INTERFACE_AS_CLASSIC_CAN)
  if (interface == CAN_NATIVE) {
    interface = CANFD_ADDON_MCP2517_1;
  }
#else
  extern bool user_selected_canfd_native_to_can1;  // from comm_nvm.cpp
  if (interface == CAN_NATIVE && user_selected_canfd_native_to_can1) {
    interface = CANFD_ADDON_MCP2517_1;
  }
#endif

  switch (interface) {
    case CAN_NATIVE: {

      CANMessage frame;
      frame.id = tx_frame->ID;
      frame.ext = tx_frame->ext_ID;
      frame.len = tx_frame->DLC;
      for (uint8_t i = 0; i < frame.len; i++) {
        frame.data[i] = tx_frame->data.u8[i];
      }
      send_ok_native = ACAN_ESP32::can.tryToSend(frame);

      if (!send_ok_native) {
        datalayer.system.info.can_native_send_fail = true;
      }
    } break;
    case CAN_ADDON_MCP2515: {
      //Struct with ACAN2515 library format, needed to use the MCP2515 library for CAN2
      CANMessage MCP2515Frame;
      MCP2515Frame.id = tx_frame->ID;
      MCP2515Frame.ext = tx_frame->ext_ID;
      MCP2515Frame.len = tx_frame->DLC;
      MCP2515Frame.rtr = false;
      for (uint8_t i = 0; i < MCP2515Frame.len; i++) {
        MCP2515Frame.data[i] = tx_frame->data.u8[i];
      }

      send_ok_2515 = can2515->tryToSend(MCP2515Frame);
      if (!send_ok_2515) {
        datalayer.system.info.can_2515_send_fail = true;
      }
    } break;
    case CANFD_NATIVE:
    case CANFD_ADDON_MCP2518: {
      CANFDMessage MCP2518Frame;
      if (tx_frame->FD) {
        MCP2518Frame.type = CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH;
      } else {  //Classic CAN message
        MCP2518Frame.type = CANFDMessage::CAN_DATA;
      }
      MCP2518Frame.id = tx_frame->ID;
      MCP2518Frame.ext = tx_frame->ext_ID;
      MCP2518Frame.len = tx_frame->DLC;
      for (uint8_t i = 0; i < MCP2518Frame.len; i++) {
        MCP2518Frame.data[i] = tx_frame->data.u8[i];
      }
      send_ok_2518 = canfd->tryToSend(MCP2518Frame);
      if (!send_ok_2518) {
        datalayer.system.info.can_2518_send_fail = true;
      }
    } break;
#ifdef HW_3LB
    case CANFD_ADDON_MCP2517_1: {
      if (!canfd_2)
        break;
      CANFDMessage frame17;
      frame17.type = tx_frame->FD ? CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH : CANFDMessage::CAN_DATA;
      frame17.id = tx_frame->ID;
      frame17.ext = tx_frame->ext_ID;
      frame17.len = tx_frame->DLC;
      memcpy(frame17.data, tx_frame->data.u8, frame17.len);
      send_ok_2517_2 = canfd_2->tryToSend(frame17);
      if (!send_ok_2517_2)
        datalayer.system.info.can_2518_send_fail = true;
    } break;
    case CANFD_ADDON_MCP2517_2: {
      if (!canfd_3)
        break;
      CANFDMessage frame17b;
      frame17b.type = tx_frame->FD ? CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH : CANFDMessage::CAN_DATA;
      frame17b.id = tx_frame->ID;
      frame17b.ext = tx_frame->ext_ID;
      frame17b.len = tx_frame->DLC;
      memcpy(frame17b.data, tx_frame->data.u8, frame17b.len);
      send_ok_2517_3 = canfd_3->tryToSend(frame17b);
      if (!send_ok_2517_3)
        datalayer.system.info.can_2518_send_fail = true;
    } break;
#endif
    default:
      // Invalid interface sent with function call. TODO: Raise event that coders messed up
      break;
  }
}

// Receive functions
void receive_can() {
  if (native_can_initialized) {
    receive_frame_can_native();  // Receive CAN messages from native CAN port
  }

  if (can2515) {
    receive_frame_can_addon();  // Receive CAN messages on add-on MCP2515 chip
  }

  if (canfd) {
    receive_frame_canfd_addon();  // Receive CAN-FD messages.
  }

  // 3LB: check additional CAN-FD interfaces (MCP2517 U5 -> CAN2)
#ifdef HW_3LB
  if (canfd_2) {
    CANFDMessage frame;
    while (canfd_2->available()) {
      canfd_2->receive(frame);
      CAN_frame rx_frame;
      rx_frame.ID = frame.id;
      rx_frame.ext_ID = frame.ext;
      rx_frame.DLC = frame.len;
      memcpy(rx_frame.data.u8, frame.data, std::min(rx_frame.DLC, (uint8_t)64));

      // map to MCP2517 U5 interface
      map_can_frame_to_variable(&rx_frame, CANFD_ADDON_MCP2517_1);
      // also provide a copy to CANFD_NATIVE for tooling compatibility
      map_can_frame_to_variable(&rx_frame, CANFD_NATIVE);
      // If compiled to treat CAN-FD as classic CAN, forward the FD frame to CAN_NATIVE
#if defined(USE_CANFD_INTERFACE_AS_CLASSIC_CAN)
      map_can_frame_to_variable(&rx_frame, CAN_NATIVE);
#else
      extern bool user_selected_canfd_native_to_can1;  // from comm_nvm.cpp
      if (user_selected_canfd_native_to_can1) {
        map_can_frame_to_variable(&rx_frame, CAN_NATIVE);
      }
#endif
    }
  }
#endif

  // 3LB: check CAN3 (MCP2517 U3 -> CAN3)
#ifdef HW_3LB
  if (canfd_3) {
    CANFDMessage frame;
    while (canfd_3->available()) {
      canfd_3->receive(frame);
      CAN_frame rx_frame;
      rx_frame.ID = frame.id;
      rx_frame.ext_ID = frame.ext;
      rx_frame.DLC = frame.len;
      memcpy(rx_frame.data.u8, frame.data, std::min(rx_frame.DLC, (uint8_t)64));

      // map to MCP2517 U3 interface
      map_can_frame_to_variable(&rx_frame, CANFD_ADDON_MCP2517_2);
      // also provide a copy to CANFD_NATIVE for tooling compatibility
      map_can_frame_to_variable(&rx_frame, CANFD_NATIVE);
      // If compiled to treat CAN-FD as classic CAN, forward the FD frame to CAN_NATIVE
#if defined(USE_CANFD_INTERFACE_AS_CLASSIC_CAN)
      map_can_frame_to_variable(&rx_frame, CAN_NATIVE);
#else
      extern bool user_selected_canfd_native_to_can1;  // from comm_nvm.cpp
      if (user_selected_canfd_native_to_can1) {
        map_can_frame_to_variable(&rx_frame, CAN_NATIVE);
      }
#endif
    }
  }
#endif
}

void receive_frame_can_native() {  // This section checks if we have a complete CAN message incoming on native CAN port
  CANMessage frame;

  if (ACAN_ESP32::can.available()) {
    if (ACAN_ESP32::can.receive(frame)) {

      CAN_frame rx_frame;
      rx_frame.ID = frame.id;
      rx_frame.ext_ID = frame.ext;
      rx_frame.DLC = frame.len;
      for (uint8_t i = 0; i < frame.len && i < 8; i++) {
        rx_frame.data.u8[i] = frame.data[i];
      }

      //message incoming, pass it on to the handler
      map_can_frame_to_variable(&rx_frame, CAN_NATIVE);
    }
  }
}

void receive_frame_can_addon() {  // This section checks if we have a complete CAN message incoming on add-on CAN port
  CAN_frame rx_frame;             // Struct with our CAN format
  CANMessage MCP2515frame;        // Struct with ACAN2515 library format, needed to use the MCP2515 library

  if (can2515->available()) {
    can2515->receive(MCP2515frame);

    rx_frame.ID = MCP2515frame.id;
    rx_frame.ext_ID = MCP2515frame.ext;
    rx_frame.DLC = MCP2515frame.len;
    for (uint8_t i = 0; i < MCP2515frame.len && i < 8; i++) {
      rx_frame.data.u8[i] = MCP2515frame.data[i];
    }

    //message incoming, pass it on to the handler
    map_can_frame_to_variable(&rx_frame, CAN_ADDON_MCP2515);
  }
}

void receive_frame_canfd_addon() {  // This section checks if we have a complete CAN-FD message incoming
  CANFDMessage MCP2518frame;
  int count = 0;
  while (canfd->available() && count++ < 16) {
    canfd->receive(MCP2518frame);

    CAN_frame rx_frame;
    rx_frame.ID = MCP2518frame.id;
    rx_frame.ext_ID = MCP2518frame.ext;
    rx_frame.DLC = MCP2518frame.len;
    memcpy(rx_frame.data.u8, MCP2518frame.data, std::min(rx_frame.DLC, (uint8_t)64));
    //message incoming, pass it on to the handler
    map_can_frame_to_variable(&rx_frame, CANFD_ADDON_MCP2518);
    map_can_frame_to_variable(&rx_frame, CANFD_NATIVE);
    // If compiled to treat CAN-FD as classic CAN, forward the FD frame to CAN_NATIVE
#if defined(USE_CANFD_INTERFACE_AS_CLASSIC_CAN)
    map_can_frame_to_variable(&rx_frame, CAN_NATIVE);
#endif
  }
}

// Support functions
void print_can_frame(CAN_frame frame, CAN_Interface interface, frameDirection msgDir) {

  if (datalayer.system.info.CAN_usb_logging_active) {
    uint8_t i = 0;
    Serial.print("(");
    Serial.print(millis() / 1000.0);
    if (msgDir == MSG_RX) {
      Serial.print(") RX");
      Serial.print((int)(interface * 2));
    } else {
      Serial.print(") TX");
      Serial.print((int)(interface * 2) + 1);
    }
    Serial.print(" ");
    Serial.print(frame.ID, HEX);
    Serial.print(" [");
    Serial.print(frame.DLC);
    Serial.print("] ");
    for (i = 0; i < frame.DLC; i++) {
      Serial.print(frame.data.u8[i] < 16 ? "0" : "");
      Serial.print(frame.data.u8[i], HEX);
      if (i < frame.DLC - 1)
        Serial.print(" ");
    }
    Serial.println("");
  }

  if (datalayer.system.info.can_logging_active) {  // If user clicked on CAN Logging page in webserver, start recording
    if (frame.ID > user_selected_CAN_ID_cutoff_filter) {  //Only log the message if CAN ID is higher than user set value
      dump_can_frame(frame, interface, msgDir);
    }
  }
}

void map_can_frame_to_variable(CAN_frame* rx_frame, CAN_Interface interface) {
  if (interface !=
      CANFD_NATIVE) {  //Avoid printing twice due to receive_frame_canfd_addon sending to both FD interfaces
    //TODO: This check can be removed later when refactored to use inline functions for logging
    print_can_frame(*rx_frame, interface, frameDirection(MSG_RX));
  }

  if (datalayer.system.info.CAN_SD_logging_active) {
    if (interface !=
        CANFD_NATIVE) {  //Avoid printing twice due to receive_frame_canfd_addon sending to both FD interfaces
      //TODO: This check can be removed later when refactored to use inline functions for logging
      add_can_frame_to_buffer(*rx_frame, frameDirection(MSG_RX));
    }
  }

  // Send the frame to all the receivers registered for this interface.
  auto receivers = can_receivers.equal_range(interface);

  for (auto it = receivers.first; it != receivers.second; ++it) {
    auto& receiver = it->second;
    receiver.receiver->receive_can_frame(rx_frame);
  }
}

void dump_can_frame(CAN_frame& frame, CAN_Interface interface, frameDirection msgDir) {
  char* message_string = datalayer.system.info.logged_can_messages;
  int offset = datalayer.system.info.logged_can_messages_offset;  // Keeps track of the current position in the buffer
  size_t message_string_size = sizeof(datalayer.system.info.logged_can_messages);

  if (offset + 128 > sizeof(datalayer.system.info.logged_can_messages)) {
    // Not enough space, reset and start from the beginning
    offset = 0;
  }
  unsigned long currentTime = millis();
  // Add timestamp
  offset += snprintf(message_string + offset, message_string_size - offset, "(%lu.%03lu) ", currentTime / 1000,
                     currentTime % 1000);

  // Add direction. Multiplying the interface by two ensures that SavvyCAN puts TX and RX in a different bus.
  offset += snprintf(message_string + offset, message_string_size - offset, "%s%d ", (msgDir == MSG_RX) ? "RX" : "TX",
                     (int)(interface * 2) + (msgDir == MSG_RX ? 0 : 1));

  // Add ID and DLC
  offset += snprintf(message_string + offset, message_string_size - offset, "%lX [%u] ", frame.ID, frame.DLC);

  // Add data bytes
  for (uint8_t i = 0; i < frame.DLC; i++) {
    if (i < frame.DLC - 1) {
      offset += snprintf(message_string + offset, message_string_size - offset, "%02X ", frame.data.u8[i]);
    } else {
      offset += snprintf(message_string + offset, message_string_size - offset, "%02X", frame.data.u8[i]);
    }
  }
  // Add linebreak
  offset += snprintf(message_string + offset, message_string_size - offset, "\n");

  datalayer.system.info.logged_can_messages_offset = offset;  // Update offset in buffer
}

void stop_can() {
  if (can_receivers.find(CAN_NATIVE) != can_receivers.end()) {
    ACAN_ESP32::can.end();
  }

  if (can2515) {
    can2515->end();
    SPI2515.end();
  }

  if (canfd) {
    canfd->end();
    SPI2517.end();
  }
#ifdef HW_3LB
  if (canfd_2) {
    canfd_2->end();
  }
  if (canfd_3) {
    canfd_3->end();
  }
  // End shared SPI bus once after both controllers stopped
  if (canfd_2 || canfd_3) {
    SPI2517_shared.end();
  }
#endif
}

void restart_can() {
  if (can_receivers.find(CAN_NATIVE) != can_receivers.end()) {
    ACAN_ESP32::can.begin(*settingsespcan);
  }

  if (can2515) {
    SPI2515.begin();
    can2515->begin(*settings2515, [] { can2515->isr(); });
  }

  if (canfd) {
    SPI2517.begin();
    canfd->begin(*settings2517, [] { can2515->isr(); });
  }
#ifdef HW_3LB
  // Restart shared SPI bus once before both controllers
  if (canfd_2 || canfd_3) {
    SPI2517_shared.begin();
  }
  if (canfd_2) {
    canfd_2->begin(*settings2517_2, [] { canfd_2->isr(); });
  }
  if (canfd_3) {
    canfd_3->begin(*settings2517_3, [] { canfd_3->isr(); });
  }
#endif
}

// Initialize the native CAN interface with the given speed and pins.
// This can be called repeatedly to change the interface speed (as some
// batteries require).
uint32_t init_native_can(CAN_Speed speed, gpio_num_t tx_pin, gpio_num_t rx_pin) {

  // TODO: check whether this is necessary? It seems to help with
  // reinitialization.
  periph_module_reset(PERIPH_TWAI_MODULE);

  if (settingsespcan != nullptr) {
    delete settingsespcan;
  }

  // Create a new settings object (as it does the bitrate calcs in the constructor)
  settingsespcan = new ACAN_ESP32_Settings((int)speed * 1000UL);
  settingsespcan->mRequestedCANMode = ACAN_ESP32_Settings::NormalMode;
  settingsespcan->mTxPin = tx_pin;
  settingsespcan->mRxPin = rx_pin;

  // (Re)start the CAN interface
  return ACAN_ESP32::can.begin(*settingsespcan);
}

// Change the speed of the given CAN interface. Returns true if successful.
bool change_can_speed(CAN_Interface interface, CAN_Speed speed) {
  if (interface == CAN_Interface::CAN_NATIVE && settingsespcan != nullptr) {
    // Reinitialize the native CAN interface with the new speed
    const uint32_t errorCode = init_native_can(speed, settingsespcan->mTxPin, settingsespcan->mRxPin);
    if (errorCode != 0) {
      logging.print("Error Native Can: 0x");
      logging.println(errorCode, HEX);
      return false;
    }
    return true;
  }

  return false;
}
