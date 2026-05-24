#include "../../datalayer/datalayer.h"
#include "../../devboard/utils/logging.h"
#include "../../lib/eModbus-eModbus/ModbusServerRTU.h"

#include <Arduino.h>

#include <cstdint>

// void set_modbus_relays(bool a, bool b, bool c, bool d) {

// }

static uint32_t last_modbus_relay_update = 0;
static bool relay_a = false;
static bool relay_b = false;
static bool relay_c = false;
static bool relay_d = false;

void modbus_relays_tick() {
  uint32_t now = millis();
  // Every 500ms
  if (now - last_modbus_relay_update > 450) {
    if (Serial2.availableForWrite() >= 100) {

      relay_a = !(datalayer.system.status.system_status == FAULT);

      // single write
      // uint8_t message[8];
      // message[0] = 33; // relays modbus address
      // message[1] = 6; // function
      // message[2] = 0; // register address (high byte)
      // message[3] = 1; // register address (low byte) (relay number)
      // message[4] = 6; // value (high byte) - delay mode?
      // message[5] = 3; // value (low byte) - seconds

      // multiple write
      uint8_t message[17];
      message[0] = 33;  // relays modbus address
      message[1] = 16;  // function (multiple write)
      message[2] = 0;   // register address (high byte)
      message[3] = 0;   // register address (low byte) (relay number)
      message[4] = 0;   // number of registers (high byte)
      message[5] = 4;   // number of registers (low byte) (we have 4 relays, so 4 registers)
      message[6] = 8;   // byte count (number of registers * 2)

      // forcibly opening (or as the datasheet calls it, closing) a relay
      // which has a timer delay active results in lots of weird clicking.
      // best to just set to 0 and wait for the 2s to timeout.

      // value for relay A
      message[7] = relay_a ? 6 : 0;  // value (high byte) - delay mode
      message[8] = relay_a ? 2 : 0;  // value (low byte) - seconds
      // value for relay B
      message[9] = relay_b ? 6 : 0;   // value (high byte) - delay mode
      message[10] = relay_b ? 2 : 0;  // value (low byte) - seconds
      // value for relay C
      message[11] = relay_c ? 6 : 0;  // value (high byte) - delay mode
      message[12] = relay_c ? 2 : 0;  // value (low byte) - seconds
      // value for relay D
      message[13] = relay_d ? 6 : 0;  // value (high byte) - delay mode
      message[14] = relay_d ? 2 : 0;  // value (low byte) - seconds

      uint16_t crc = RTUutils::calcCRC(message, sizeof(message) - 2);
      message[sizeof(message) - 2] = crc & 0xFF;         // CRC low byte
      message[sizeof(message) - 1] = (crc >> 8) & 0xFF;  // CRC high byte

      // // Send the current relay states as a single byte (bit 0 = A, bit 1 = B, bit 2 = C, bit 3 = D)
      // uint8_t relay_state = (datalayer.battery.status.relay_a ? 1 : 0) |
      //                       (datalayer.battery.status.relay_b ? 2 : 0) |
      //                       (datalayer.battery.status.relay_c ? 4 : 0) |
      //                       (datalayer.battery.status.relay_d ? 8 : 0);
      Serial2.write(message, sizeof(message));
      last_modbus_relay_update = now;
    }
  }
}
