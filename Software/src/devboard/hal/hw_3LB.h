// CHAdeMO support pin dependencies
#ifndef __HW_3LB_H__
#define __HW_3LB_H__

#include <Arduino.h>
#include "hal.h"

// --- MCP23017 expander pin mapping for 3LB board (as gpio_num_t enums) ---
#define EXT1_GPIO_NUM_A0 ((gpio_num_t)0)
#define EXT1_GPIO_NUM_A1 ((gpio_num_t)1)
#define EXT1_GPIO_NUM_A2 ((gpio_num_t)2)
#define EXT1_GPIO_NUM_A3 ((gpio_num_t)3)
#define EXT1_GPIO_NUM_A4 ((gpio_num_t)4)
#define EXT1_GPIO_NUM_A5 ((gpio_num_t)5)
#define EXT1_GPIO_NUM_A6 ((gpio_num_t)6)
#define EXT1_GPIO_NUM_A7 ((gpio_num_t)7)
#define EXT1_GPIO_NUM_B0 ((gpio_num_t)8)
#define EXT1_GPIO_NUM_B1 ((gpio_num_t)9)
#define EXT1_GPIO_NUM_B2 ((gpio_num_t)10)
#define EXT1_GPIO_NUM_B3 ((gpio_num_t)11)
#define EXT1_GPIO_NUM_B4 ((gpio_num_t)12)
#define EXT1_GPIO_NUM_B5 ((gpio_num_t)13)
#define EXT1_GPIO_NUM_B6 ((gpio_num_t)14)
#define EXT1_GPIO_NUM_B7 ((gpio_num_t)15)

class ThreeLBHal : public Esp32Hal {
 public:
  const char* name() { return "3LB board"; }

  //are these backwards? I don't use RS485 so can't test
  virtual gpio_num_t RS485_TX_PIN() { return GPIO_NUM_33; }
  virtual gpio_num_t RS485_RX_PIN() { return GPIO_NUM_32; }

  // --- CAN1: Native ESP32 CAN (klasyczny CAN) ---
  // For 3LB native CAN pins are different from other boards
  // TX -> IO27, RX -> IO16 (as per 3LB schematic)
  virtual gpio_num_t CAN_TX_PIN() { return GPIO_NUM_27; }
  virtual gpio_num_t CAN_RX_PIN() { return GPIO_NUM_16; }

  // --- CAN2: MCP2517FD U5 (pierwszy dodatkowy interfejs) ---
  virtual gpio_num_t MCP2517_2_SCK() { return GPIO_NUM_18; }
  virtual gpio_num_t MCP2517_2_SDI() { return GPIO_NUM_23; }
  virtual gpio_num_t MCP2517_2_SDO() { return GPIO_NUM_19; }
  virtual gpio_num_t MCP2517_2_CS() { return GPIO_NUM_5; }
  virtual gpio_num_t MCP2517_2_INT() { return GPIO_NUM_34; }

  // --- CAN3: MCP2517FD U3 (drugi dodatkowy interfejs) ---
  virtual gpio_num_t MCP2517_3_SCK() { return GPIO_NUM_18; }
  virtual gpio_num_t MCP2517_3_SDI() { return GPIO_NUM_23; }
  virtual gpio_num_t MCP2517_3_SDO() { return GPIO_NUM_19; }
  virtual gpio_num_t MCP2517_3_CS() { return GPIO_NUM_2; }
  virtual gpio_num_t MCP2517_3_INT() { return GPIO_NUM_35; }

  // OPCJONALNE: MCP2515 add-on (jeśli potrzebne)
  virtual gpio_num_t MCP2515_SCK() { return GPIO_NUM_14; }
  virtual gpio_num_t MCP2515_MOSI() { return GPIO_NUM_13; }
  virtual gpio_num_t MCP2515_MISO() { return GPIO_NUM_12; }
  virtual gpio_num_t MCP2515_CS() { return GPIO_NUM_15; }
  virtual gpio_num_t MCP2515_INT() { return EXT1_GPIO_NUM_B6; }

  //unused in 3lb
  // CHAdeMO support pin dependencies
  virtual gpio_num_t CHADEMO_PIN_2() { return EXT1_GPIO_NUM_A0; }
  virtual gpio_num_t CHADEMO_PIN_10() { return EXT1_GPIO_NUM_A1; }
  virtual gpio_num_t CHADEMO_PIN_7() { return EXT1_GPIO_NUM_A2; }
  virtual gpio_num_t CHADEMO_PIN_4() { return EXT1_GPIO_NUM_A3; }
  virtual gpio_num_t CHADEMO_LOCK() { return EXT1_GPIO_NUM_A4; }

  // Contactor handling
  virtual gpio_num_t POSITIVE_CONTACTOR_PIN() { return EXT1_GPIO_NUM_B3; }
  virtual gpio_num_t NEGATIVE_CONTACTOR_PIN() { return EXT1_GPIO_NUM_B2; }
  virtual gpio_num_t PRECHARGE_PIN() { return EXT1_GPIO_NUM_B4; }
  virtual gpio_num_t BMS_POWER() { return EXT1_GPIO_NUM_B5; }
  virtual gpio_num_t SECOND_BATTERY_CONTACTORS_PIN() { return EXT1_GPIO_NUM_B1; }

  // Dodatkowy pin dla trzeciej baterii (specyficzny dla 3LB)
  virtual gpio_num_t THIRD_BATTERY_CONTACTORS_PIN() { return EXT1_GPIO_NUM_B0; }

  // Automatic precharging
  //conflicts with serial, but hopefully that won't be an issue
  //the other option is to use either the ethernet interface pins
  //which are 25,26 but I expect people won't be using serial and pre-charge
  //at the same time, so this allows "full functionality" if you ignore serial.
  virtual gpio_num_t HIA4V1_PIN() { return GPIO_NUM_1; }
  virtual gpio_num_t INVERTER_DISCONNECT_CONTACTOR_PIN() { return GPIO_NUM_3; }

  // SMA CAN contactor pins
  virtual gpio_num_t INVERTER_CONTACTOR_ENABLE_PIN() { return GPIO_NUM_36; }
  virtual gpio_num_t INVERTER_CONTACTOR_ENABLE_LED_PIN() { return GPIO_NUM_NC; }

  //unused in 3lb
  // SD card
  virtual gpio_num_t SD_MISO_PIN() { return GPIO_NUM_2; }
  virtual gpio_num_t SD_MOSI_PIN() { return GPIO_NUM_15; }
  virtual gpio_num_t SD_SCLK_PIN() { return GPIO_NUM_14; }
  virtual gpio_num_t SD_CS_PIN() { return GPIO_NUM_13; }

  // LED
  // Restored to original pin where the LED worked previously
  virtual gpio_num_t LED_PIN() { return GPIO_NUM_4; }
  virtual uint8_t LED_MAX_BRIGHTNESS() { return 40; }

  // Equipment stop pin
  virtual gpio_num_t EQUIPMENT_STOP_PIN() { return EXT1_GPIO_NUM_B7; }

  // Battery wake up pins
  virtual gpio_num_t WUP_PIN1() { return EXT1_GPIO_NUM_A5; }
  virtual gpio_num_t WUP_PIN2() { return EXT1_GPIO_NUM_A6; }

  std::vector<comm_interface> available_interfaces() {
    return {
        comm_interface::Modbus, comm_interface::RS485, comm_interface::CanNative,
        comm_interface::CanFdAddonMcp2517_1,  // CAN2 - MCP2517FD U5
        comm_interface::CanFdAddonMcp2517_2   // CAN3 - MCP2517FD U3
    };
  }

  // Provide human-readable names for the comm interfaces specific to 3LB
  virtual const char* name_for_comm_interface(comm_interface comm) override {
    switch (comm) {
      case comm_interface::CanNative:
        return "CAN Native (CAN0)";
      case comm_interface::CanFdAddonMcp2517_1:
        return "CAN-FD U5 (CAN2)";
      case comm_interface::CanFdAddonMcp2517_2:
        return "CAN-FD U3 (CAN3)";
      case comm_interface::RS485:
        return "RS485";
      case comm_interface::Modbus:
        return "Modbus";
      // Hide all other interfaces (MCP2515, MCP2518, CanFdNative etc.)
      default:
        return nullptr;  // Return nullptr to hide unavailable interfaces
    }
  }
};

#endif
