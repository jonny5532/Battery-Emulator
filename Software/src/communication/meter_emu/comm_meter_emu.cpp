#include "comm_meter_emu.h"
//#include "../../include.h"
#include "WiFiUdp.h"

#include <semaphore>

/*

RS485 Meter Emulator

Exposes a Modbus RTU server on the RS485 port, which can be connected to an
inverter's meter connection.

When the inverter requests a meter reading, this emulator will respond with the
current power, if a recent reading is available.

Readings can be sent to the emulator via UDP on port 9929, by sending a four
byte packet:

Byte 0: High byte of PGrid (signed integer, positive means we're drawing from
the grid) Byte 1: Low byte of PGrid Byte 2: High byte of valid time in
milliseconds Byte 3: Low byte of valid time in milliseconds

The valid time indicates how long this meter reading will be valid for, before
the emulator starts returning an error to the inverter. If this is too short,
then the inverter may see errors if you don't update the reading often enough
over UDP. If too long, you run the risk of the inverter seeing stale data and
importing/exporting power incorrectly. The maximum is 65535 milliseconds.


*/

ModbusServerRTU meter_emu_modbus_server(2000);  // Modbus RTU server with 2000ms timeout

std::binary_semaphore meter_emu_fresh_data{0};

volatile int16_t meter_holding_registers[] = {
    0,  // RESERVED
    1,  // version
    0,  // zero clearing
    0,  // RESERVED
    0,  // RESERVED

    0,    // ChangeProtocol
    1,    // Device addr
    0,    // RESERVED
    123,  // RESERVED
    0,    // RESERVED

    0,     // RESERVED
    0xa0,  // Meter Type
    3,     // Baud
    0,     // RESERVED
    0,     // -PGrid in watts (positive means we're drawing from the grid)
    0,     // RESERVED
};
const int PGrid_OFFSET = 14;  // Offset for PGrid in holding registers

unsigned long meter_valid_until = 0;
int modbus_query_count = 0;

ModbusMessage handle_meter_hold_register_request(ModbusMessage request) {
  ModbusMessage response;  // The Modbus message we are going to give back
  uint16_t addr = 0;       // Start address
  uint16_t words = 0;      // # of words requested
  request.get(2, addr);    // read address from request
  request.get(4, words);   // read # of words from request

  // TODO - handle millis overflow
  if (meter_valid_until <= millis()) {
    // If the meter value is not valid anymore, return an error
    response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
#ifdef DEBUG_LOG
    logging.printf("modbus returning error.\n");
#endif

    return response;
  }

  // Address overflow?
  if ((addr + words) > sizeof(meter_holding_registers) / sizeof(uint16_t)) {
    // Yes - send respective error response
    response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
    return response;
  }

  // Empty the semaphore to ensure we wait for fresh data
  // meter_emu_fresh_data.try_acquire();

  // // stall for up to 500ms to wait for fresh data
  // if(meter_emu_fresh_data.try_acquire_for(std::chrono::milliseconds(500))) {
  //   #ifdef DEBUG_LOG
  //     logging.printf("acquired!\n");
  //   #endif
  // } else {
  //   #ifdef DEBUG_LOG
  //     logging.printf("didn't acquire!\n");
  //   #endif
  // }

  // Set up response
  response.add(request.getServerID(), request.getFunctionCode(), (uint8_t)(words * 2));
  for (uint8_t i = 0; i < words; ++i) {
    // send data values
    response.add((uint16_t)(meter_holding_registers[addr + i]));
  }

  modbus_query_count++;
#ifdef DEBUG_LOG
  if ((modbus_query_count & (0x8 - 1)) == 0) {  // was 0x20
    logging.printf("modbus returning %d %d.\n", addr, meter_holding_registers[addr]);
  }
#endif

  return response;
}

static void meter_emu_udp_task(void* pData) {
  char buffer[4];
  WiFiUDP MeterUdp;
  MeterUdp.begin(METER_EMU_PORT);

#ifdef DEBUG_LOG
  logging.printf("Meter UDP task starting.");
#endif

  // check if there's a UDP packet
  while (true) {
    int available = MeterUdp.parsePacket();
    if (available == 0) {
      // nothing available, skip
    } else if (available == 4) {
      int read = MeterUdp.read(buffer, 4);
      if (read != 4) {
        // something weird happened, reset
        MeterUdp.begin(METER_EMU_PORT);
      } else {
        int16_t PGrid = (int16_t)((buffer[0] << 8) | buffer[1]);  // Combine the two bytes into a signed integer
        meter_holding_registers[PGrid_OFFSET] = PGrid;            // Update the holding register for PGrid

        uint16_t valid_ms = (buffer[2] << 8) | buffer[3];  // Get the valid time in milliseconds
        if (valid_ms > 0) {
          meter_valid_until = millis() + valid_ms;  // Set the valid time for the meter data
        } else {
          meter_valid_until = millis();  // If valid_ms is 0, set valid_until to 0
        }

        //meter_emu_fresh_data.release(); // Signal that fresh data is available
      }
    } else if (available < 0) {
      // error, reset
      MeterUdp.begin(METER_EMU_PORT);  // Restart the UDP server if an error occurs
    } else {
#ifdef DEBUG_LOG
      logging.printf("bad packet, size: %d\n", available);
#endif
      MeterUdp.clear();  // Clear the buffer if the packet size is not 4
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);  // Delay to avoid busy-waiting
  }
}

void init_meter_emu() {
  xTaskCreate(meter_emu_udp_task, "meter_emu_udp_task", 2400, NULL, TASK_METER_EMU_PRIO, NULL);

  RTUutils::prepareHardwareSerial(Serial2);

#define RS485_RX_PIN GPIO_NUM_21
#define RS485_TX_PIN GPIO_NUM_22
  Serial2.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  meter_emu_modbus_server.registerWorker(1, READ_HOLD_REGISTER, &handle_meter_hold_register_request);

#define MODBUS_CORE 0
  meter_emu_modbus_server.begin(Serial2, MODBUS_CORE);
}
