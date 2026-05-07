#include "CMFA-EV-BATTERY.h"
#include <cstring>  //unit tests memcpy
#include "../communication/can/comm_can.h"
#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/utils/events.h"
#include "BATTERIES.h"

/* The raw SOC value sits at 90% when the battery is full, so we should report back 100% once this value is reached
Same goes for low point, when 10% is reached we report 0% */

uint16_t CmfaEvBattery::rescale_raw_SOC(uint32_t raw_SOC) {

  uint32_t calc_soc;
  calc_soc = (raw_SOC * 0.25);
  if (calc_soc > MAXSOC) {  //Constrain if needed
    calc_soc = MAXSOC;
  }
  if (calc_soc < MINSOC) {  //Constrain if needed
    calc_soc = MINSOC;
  }
  // Perform scaling between the two points
  calc_soc = 10000 * (calc_soc - MINSOC);
  calc_soc = calc_soc / (MAXSOC - MINSOC);

  return (uint16_t)calc_soc;
}

void CmfaEvBattery::
    update_values() {  //This function maps all the values fetched via CAN to the correct parameters used for modbus
  datalayer_battery->status.soh_pptt = (SOH * 100);

  datalayer_battery->status.real_soc = rescale_raw_SOC(SOC_raw);

  datalayer_battery->status.current_dA = current * 10;

  datalayer_battery->status.voltage_dV = pack_voltage * 5;

  datalayer_battery->info.total_capacity_Wh = 27000;

  //Calculate the remaining Wh amount from SOC% and max Wh value.
  datalayer_battery->status.remaining_capacity_Wh = static_cast<uint32_t>(
      (static_cast<double>(datalayer_battery->status.real_soc) / 10000) * datalayer_battery->info.total_capacity_Wh);

  datalayer_battery->status.max_discharge_power_W = discharge_power_w;

  datalayer_battery->status.max_charge_power_W = charge_power_w;

  datalayer_battery->status.temperature_min_dC = (lowest_cell_temperature * 10);

  datalayer_battery->status.temperature_max_dC = (highest_cell_temperature * 10);

  datalayer_battery->status.cell_min_voltage_mV = lowest_cell_voltage_mv;

  datalayer_battery->status.cell_max_voltage_mV = highest_cell_voltage_mv;

  if (lead_acid_voltage < 11000) {  //11.000V
    set_event(EVENT_12V_LOW, lead_acid_voltage);
  }

  if (!battery2) {  //Avoid pointer crash on double bat, not sure why this wont work
    // Update webserver datalayer
    datalayer_cmfa->soc_u = soc_u;
    datalayer_cmfa->soc_z = soc_z;
    datalayer_cmfa->lead_acid_voltage = lead_acid_voltage;
    datalayer_cmfa->highest_cell_voltage_number = highest_cell_voltage_number;
    datalayer_cmfa->lowest_cell_voltage_number = lowest_cell_voltage_number;
    datalayer_cmfa->max_regen_power = max_regen_power;
    datalayer_cmfa->max_discharge_power = max_discharge_power;
    datalayer_cmfa->average_temperature = average_temperature;
    datalayer_cmfa->minimum_temperature = minimum_temperature;
    datalayer_cmfa->maximum_temperature = maximum_temperature;
    datalayer_cmfa->maximum_charge_power = maximum_charge_power;
    datalayer_cmfa->SOH_available_power = SOH_available_power;
    datalayer_cmfa->SOH_generated_power = SOH_generated_power;
    datalayer_cmfa->cumulative_energy_when_discharging = cumulative_energy_when_discharging;
    datalayer_cmfa->cumulative_energy_when_charging = cumulative_energy_when_charging;
    datalayer_cmfa->cumulative_energy_in_regen = cumulative_energy_in_regen;
    datalayer_cmfa->soh_average = soh_average;
    datalayer_cmfa->average_voltage_of_cells = average_voltage_of_cells;
  }
}

void CmfaEvBattery::handle_incoming_can_frame(CAN_frame rx_frame) {
  if (handle_incoming_uds_can_frame(rx_frame)) {
    return;
  }

  switch (rx_frame.ID) {  //These frames are transmitted by the battery
    case 0x127:           //10ms , Same structure as old Zoe 0x155 message!
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      current = (((((rx_frame.data.u8[1] & 0x0F) << 8) | rx_frame.data.u8[2]) * 0.25) - 500);
      SOC_raw = ((rx_frame.data.u8[4] << 8) | rx_frame.data.u8[5]);
      pack_voltage = (((rx_frame.data.u8[6] & 0x03) << 8) | rx_frame.data.u8[7]);
      break;
    case 0x3D6:  //100ms, Same structure as old Zoe 0x424 message!
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      charge_power_w = rx_frame.data.u8[2] * 500;
      discharge_power_w = rx_frame.data.u8[3] * 500;
      lowest_cell_temperature = (rx_frame.data.u8[4] - 40);
      SOH = rx_frame.data.u8[5];
      heartbeat = rx_frame.data.u8[6];
      highest_cell_temperature = (rx_frame.data.u8[7] - 40);
      break;
    case 0x3D7:  //100ms
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      break;
    case 0x3D8:  //100ms
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      //counter_3D8 = rx_frame.data.u8[3]; //?
      //CRC_3D8 = rx_frame.data.u8[4]; //?
      break;
    case 0x43C:  //100ms
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      heartbeat2 = rx_frame.data.u8[2];  //Alternates between 0x55 and 0xAA every 5th frame
      break;
    case 0x431:  //100ms
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      //byte0 9C always
      //byte1 40 always
      break;
    case 0x5A9:
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      break;
    case 0x5AB:
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      break;
    case 0x5C8:
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      break;
    case 0x5E1:
      datalayer_battery->status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      break;
    default:
      break;
  }
}

uint32_t CmfaEvBattery::handle_pid(uint16_t pid, uint32_t value, const uint8_t* data, uint16_t length,
                                   UdsStatus status) {
  switch (pid) {
    case PID_POLL_SOCZ:
      soc_z = (uint16_t)((data[0] << 8) | data[1]);
      return PID_POLL_USOC;
    case PID_POLL_USOC:
      soc_u = (uint16_t)((data[0] << 8) | data[1]);
      return PID_POLL_CURRENT_OFFSET;
    case PID_POLL_CURRENT_OFFSET:
      return PID_POLL_INSTANT_CURRENT;
    case PID_POLL_INSTANT_CURRENT:
      return PID_POLL_MAX_REGEN;
    case PID_POLL_MAX_REGEN:
      max_regen_power = (uint16_t)((data[0] << 8) | data[1]);
      return PID_POLL_MAX_DISCHARGE_POWER;
    case PID_POLL_MAX_DISCHARGE_POWER:
      max_discharge_power = (uint16_t)((data[1] << 8) | data[1]);
      return PID_POLL_MAX_CHARGE_POWER;
    case PID_POLL_MAX_CHARGE_POWER:
      maximum_charge_power = (uint16_t)((data[0] << 8) | data[1]);
      return PID_POLL_AVERAGE_TEMPERATURE;
    case PID_POLL_AVERAGE_TEMPERATURE:
      average_temperature = ((((data[0] << 8) | data[1]) - 400) / 2);
      return PID_POLL_MIN_TEMPERATURE;
    case PID_POLL_MIN_TEMPERATURE:
      minimum_temperature = ((((data[0] << 8) | data[1]) - 400) / 2);
      return PID_POLL_MAX_TEMPERATURE;
    case PID_POLL_MAX_TEMPERATURE:
      maximum_temperature = ((((data[0] << 8) | data[1]) - 400) / 2);
      return PID_POLL_END_OF_CHARGE_FLAG;
    case PID_POLL_END_OF_CHARGE_FLAG:
      end_of_charge = data[0];
      return PID_POLL_INTERLOCK_FLAG;
    case PID_POLL_INTERLOCK_FLAG:
      interlock_flag = data[0];
      return PID_POLL_CELL_1;
    // PID_POLL_CELL_1 - PID_POLL_CELL_72 handled below
    case PID_POLL_SOH_AVERAGE:
      soh_average = (uint16_t)((data[0] << 8) | data[1]);
      return PID_POLL_AVERAGE_VOLTAGE_OF_CELLS;
    case PID_POLL_AVERAGE_VOLTAGE_OF_CELLS:
      average_voltage_of_cells = (uint32_t)((data[1] << 16) | (data[2] << 8) | (data[3]));
      return PID_POLL_HIGHEST_CELL_VOLTAGE;
    case PID_POLL_HIGHEST_CELL_VOLTAGE:
      highest_cell_voltage_mv = (uint16_t)(((data[0] << 8) | data[1]) * 0.976563);
      return PID_POLL_LOWEST_CELL_VOLTAGE;
    case PID_POLL_LOWEST_CELL_VOLTAGE:
      lowest_cell_voltage_mv = (uint16_t)(((data[0] << 8) | data[1]) * 0.976563);
      return PID_POLL_CELL_NUMBER_HIGHEST_VOLTAGE;
    case PID_POLL_CELL_NUMBER_HIGHEST_VOLTAGE:
      highest_cell_voltage_number = data[0];
      return PID_POLL_CELL_NUMBER_LOWEST_VOLTAGE;
    case PID_POLL_CELL_NUMBER_LOWEST_VOLTAGE:
      lowest_cell_voltage_number = data[0];
      return PID_POLL_12V_BATTERY;
    case PID_POLL_12V_BATTERY:
      lead_acid_voltage = (uint16_t)((data[0] << 8) | data[1]);
      return PID_POLL_CUMULATIVE_ENERGY_WHEN_CHARGING;
    case PID_POLL_CUMULATIVE_ENERGY_WHEN_CHARGING:
      cumulative_energy_when_charging = (uint64_t)((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | (data[3]));
      return PID_POLL_CUMULATIVE_ENERGY_WHEN_DISCHARGING;
    case PID_POLL_CUMULATIVE_ENERGY_WHEN_DISCHARGING:
      cumulative_energy_when_discharging = (uint64_t)((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | (data[3]));
      return PID_POLL_CUMULATIVE_ENERGY_IN_REGEN;
    case PID_POLL_CUMULATIVE_ENERGY_IN_REGEN:
      cumulative_energy_in_regen = (uint64_t)((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | (data[3]));
      // Back to the first PID
      return PID_POLL_SOCZ;
    default:
      uint8_t cellnumber = 0;
      if (pid >= PID_POLL_CELL_1 && pid <= PID_POLL_CELL_31) {
        cellnumber = (pid - PID_POLL_CELL_1);
      } else if (pid >= PID_POLL_CELL_32 && pid <= PID_POLL_CELL_62) {
        cellnumber = (pid - PID_POLL_CELL_1) - 1;
      } else if (pid >= PID_POLL_CELL_63 && pid <= PID_POLL_CELL_72) {
        cellnumber = (pid - PID_POLL_CELL_1) - 2;
      } else {
        // Non-cell PID, reset
        return 0;
      }

      if (cellnumber < MAX_AMOUNT_CELLS) {
        uint16_t cellvoltage_reading = (uint16_t)((data[0] << 8) | data[1]);
        if (cellvoltage_reading == 0) {
          cellvoltage_reading = 10;
          set_event(EVENT_BATTERY_FUSE, cellnumber);
        }
        datalayer_battery->status.cell_voltages_mV[cellnumber] = cellvoltage_reading * 0.976563;
      }

      // Final cell
      if (pid == PID_POLL_CELL_72)
        return PID_POLL_SOH_AVERAGE;
      // Cells where PID jumps
      else if (pid == PID_POLL_CELL_31)
        return PID_POLL_CELL_32;
      else if (pid == PID_POLL_CELL_62)
        return PID_POLL_CELL_63;
      // Normal case, next cell
      return pid + 1;
  }
  return 0;
}

void CmfaEvBattery::transmit_can(unsigned long currentMillis) {
  // Send 10ms CAN Message
  if (currentMillis - previousMillis10ms >= INTERVAL_10_MS) {
    previousMillis10ms = currentMillis;
    transmit_can_frame(&CMFA_1EA);
    transmit_can_frame(&CMFA_135);
    transmit_can_frame(&CMFA_134);
    transmit_can_frame(&CMFA_125);

    CMFA_135.data.u8[1] = content_135[counter_10ms];
    CMFA_125.data.u8[3] = content_125[counter_10ms];
    counter_10ms = (counter_10ms + 1) % 16;  // counter_10ms cycles between 0-1-2-3..15-0-1...
  }
  // Send 100ms CAN Message
  if (currentMillis - previousMillis100ms >= INTERVAL_100_MS) {
    previousMillis100ms = currentMillis;

    transmit_can_frame(&CMFA_59B);
    transmit_can_frame(&CMFA_3D3);
  }

  transmit_uds_can(currentMillis);
}

void CmfaEvBattery::setup(void) {  // Performs one time setup at startup
  setup_uds(0x79B, PID_POLL_SOH_AVERAGE);

  strncpy(datalayer.system.info.battery_protocol, Name, 63);
  datalayer.system.info.battery_protocol[63] = '\0';
  datalayer.system.status.battery_allows_contactor_closing = true;
  datalayer_battery->info.number_of_cells = 72;
  datalayer_battery->info.max_design_voltage_dV = MAX_PACK_VOLTAGE_DV;
  datalayer_battery->info.min_design_voltage_dV = MIN_PACK_VOLTAGE_DV;
  datalayer_battery->info.max_cell_voltage_mV = MAX_CELL_VOLTAGE_MV;
  datalayer_battery->info.min_cell_voltage_mV = MIN_CELL_VOLTAGE_MV;
  datalayer_battery->info.max_cell_voltage_deviation_mV = MAX_CELL_DEVIATION_MV;
}
