#include "MG-4-BATTERY.h"
#include <soc/soc.h>
#include <cmath>    //For unit test
#include <cstring>  //For unit test
//#include "esp_timer.h"
#include "../battery/BATTERIES.h"
#include "../communication/can/comm_can.h"
#include "../communication/contactorcontrol/comm_contactorcontrol.h"
#include "../datalayer/datalayer.h"
#include "../devboard/utils/common_functions.h"
#include "../devboard/utils/events.h"
#include "../devboard/utils/logging.h"

static const uint16_t MAX_CHARGE_POWER_W = 14000;
static const uint16_t CHARGE_TRICKLE_POWER_W = 100;    // The cell voltage limits will override
static const uint16_t DERATE_CHARGE_ABOVE_SOC = 9500;  // in 0.01% units

static const uint16_t MAX_DISCHARGE_POWER_W = 14000;
static const uint16_t DERATE_DISCHARGE_BELOW_SOC = 500;  // in 0.01% units
static const uint16_t DISCHARGE_MIN_SOC = 0;

// Cell-voltage-based power derating (copied from MG-GEN1-BATTERY)

static constexpr int32_t CHARGE_TAPER_MV = 50;
static constexpr int32_t CHARGE_HYSTERESIS_MV = 10;

static constexpr int32_t DISCHARGE_TAPER_MV = 50;
static constexpr int32_t DISCHARGE_HYSTERESIS_MV = 25;

// Temperature-based power derating (copied from MG-GEN1-BATTERY)

// Temp thresholds for the two chemistries
static constexpr int32_t MIN_TEMP_NMC_DC = -100;
static constexpr int32_t MIN_TEMP_LFP_DC = 0;
static constexpr int32_t MAX_TEMP_DC = 500;
// Taper down to 0W at min temp with a linear gradient (affects charge only)
static constexpr int32_t MIN_WATTS_PER_DC = 280;  // gradient is 14kW per 5dC
// Taper down to 0W at max temp with a linear gradient (both charge and discharge)
static constexpr int32_t MAX_WATTS_PER_DC = 140;  // gradient is 14kW per 10dC

// 101 voltage values from 0% to 100%
static const uint16_t nmc_voltages[] = {
    2500, 3100, 3246, 3336, 3400, 3448, 3468, 3473, 3478, 3483, 3488, 3494, 3501, 3509, 3519, 3529, 3539,
    3549, 3559, 3568, 3576, 3585, 3595, 3601, 3605, 3609, 3612, 3615, 3618, 3621, 3624, 3627, 3629, 3632,
    3635, 3638, 3641, 3643, 3646, 3649, 3652, 3655, 3659, 3662, 3666, 3669, 3673, 3677, 3682, 3686, 3691,
    3697, 3702, 3709, 3717, 3727, 3739, 3750, 3758, 3766, 3773, 3781, 3789, 3797, 3805, 3814, 3823, 3832,
    3841, 3850, 3860, 3869, 3880, 3889, 3900, 3910, 3920, 3930, 3941, 3951, 3962, 3973, 3983, 3994, 4005,
    4017, 4028, 4039, 4051, 4062, 4074, 4086, 4098, 4110, 4122, 4135, 4147, 4160, 4173, 4186, 4200};

static const uint16_t lfp_voltages[] = {
    2700, 2935, 3033, 3097, 3145, 3175, 3191, 3198, 3201, 3202, 3202, 3204, 3206, 3212, 3218, 3224, 3230,
    3235, 3240, 3246, 3250, 3254, 3257, 3260, 3263, 3267, 3270, 3274, 3278, 3282, 3285, 3286, 3286, 3287,
    3288, 3289, 3289, 3289, 3289, 3289, 3289, 3289, 3289, 3289, 3289, 3289, 3289, 3289, 3289, 3289, 3289,
    3290, 3291, 3292, 3292, 3293, 3294, 3296, 3298, 3303, 3312, 3322, 3327, 3327, 3328, 3328, 3328, 3328,
    3328, 3328, 3329, 3329, 3329, 3329, 3329, 3329, 3329, 3329, 3329, 3329, 3329, 3329, 3329, 3329, 3329,
    3329, 3329, 3329, 3329, 3329, 3330, 3330, 3330, 3331, 3331, 3332, 3332, 3333, 3336, 3354, 3571};

// inline static uint32_t get_working_max_cell_voltage_mV() {
//   //return datalayer.battery.info.max_cell_voltage_mV - 150;
// }
// inline static uint32_t get_working_min_cell_voltage_mV() {
//   //return datalayer.battery.info.min_cell_voltage_mV + 300;
// }

/* Clamped linear interpolation: output_min at input_min, output_max at
   input_max, linear between. Inputs outside the range are clamped. */
static int32_t battery_linear_taper(int32_t input, int32_t input_min, int32_t input_max, int32_t output_min,
                                    int32_t output_max) {
  if (input <= input_min) {
    return output_min;
  } else if (input >= input_max) {
    return output_max;
  } else {
    // Linear interpolation
    return output_min + ((output_max - output_min) * (input - input_min)) / (input_max - input_min);
  }
}

/* SoC based charge derating: full max_charge_power up to derate_above_soc (in
   0.01% units), then a linear taper down to trickle_charge_power at 100%. */
static uint32_t battery_charge_power_by_soc(uint32_t soc, uint32_t max_charge_power, uint32_t trickle_charge_power,
                                            uint16_t derate_above_soc) {
  if (soc <= derate_above_soc) {
    return max_charge_power;
  } else if (soc >= 10000) {
    return trickle_charge_power;
  } else {
    // Linear derate
    return max_charge_power -
           ((max_charge_power - trickle_charge_power) * (soc - derate_above_soc)) / (10000 - derate_above_soc);
  }
}

/* SoC based discharge derating: full max_discharge_power down to
   derate_below_soc (in 0.01% units), then a linear taper down to 0 at min_soc. */
static uint32_t battery_discharge_power_by_soc(uint32_t soc, uint32_t max_discharge_power, uint16_t min_soc,
                                               uint16_t derate_below_soc) {
  if (soc >= derate_below_soc) {
    return max_discharge_power;
  } else if (soc <= min_soc) {
    return 0;
  } else {
    // Linear derate
    return max_discharge_power - ((max_discharge_power * (derate_below_soc - soc)) / (derate_below_soc - min_soc));
  }
}

/* Cell voltage based charge derating with hysteresis. Tapers linearly to 0 as
   cell_max_mV rises from (working_max_mV - taper_mV) to working_max_mV. Once
   tripped at working_max_mV the limit stays 0 until cell_max_mV drops back to
   working_max_mV - recover_mV. The tripped flag persists between calls. */
static int32_t battery_charge_power_by_cell_max(uint32_t cell_max_mV, uint32_t working_max_mV, uint32_t taper_mV,
                                                uint32_t recover_mV, int32_t max_charge_power_W, bool* tripped) {
  if (*tripped && cell_max_mV <= working_max_mV - recover_mV) {
    *tripped = false;
  } else if (!*tripped && cell_max_mV >= working_max_mV) {
    *tripped = true;
  }
  int32_t power = battery_linear_taper((int32_t)cell_max_mV, (int32_t)(working_max_mV - taper_mV),
                                       (int32_t)working_max_mV, max_charge_power_W, 0);
  return *tripped ? 0 : power;
}

/* Cell voltage based discharge derating with hysteresis. Tapers linearly to 0
   as cell_min_mV falls from (working_min_mV + taper_mV) to working_min_mV.
   Once tripped at working_min_mV the limit stays 0 until cell_min_mV recovers
   to working_min_mV + recover_mV. The tripped flag persists between calls. */
static int32_t battery_discharge_power_by_cell_min(uint32_t cell_min_mV, uint32_t working_min_mV, uint32_t taper_mV,
                                                   uint32_t recover_mV, int32_t max_discharge_power_W, bool* tripped) {
  if (*tripped && cell_min_mV >= working_min_mV + recover_mV) {
    *tripped = false;
  } else if (!*tripped && cell_min_mV <= working_min_mV) {
    *tripped = true;
  }
  int32_t power = battery_linear_taper((int32_t)cell_min_mV, (int32_t)working_min_mV,
                                       (int32_t)(working_min_mV + taper_mV), 0, max_discharge_power_W);
  return *tripped ? 0 : power;
}

/* Temperature based derating, low side: power scales linearly from 0 at
   min_temp_dC upwards at watts_per_dC per degree C. Below min_temp_dC the value
   goes negative (meaning "no power allowed" once intersected). */
static int32_t battery_power_by_low_temp(int32_t temp_min_dC, int32_t min_temp_dC, int32_t watts_per_dC) {
  return (temp_min_dC - min_temp_dC) * watts_per_dC;
}

/* Temperature based derating, high side: power scales linearly from 0 at
   max_temp_dC downwards at watts_per_dC per degree C. Above max_temp_dC the
   value goes negative (meaning "no power allowed" once intersected). */
static int32_t battery_power_by_high_temp(int32_t temp_max_dC, int32_t max_temp_dC, int32_t watts_per_dC) {
  return (max_temp_dC - temp_max_dC) * watts_per_dC;
}

static uint16_t ocv_to_soc(uint16_t voltage_mV) {
  const uint16_t* voltages;
  if (datalayer.battery.info.chemistry == battery_chemistry_enum::LFP) {
    voltages = lfp_voltages;
  } else {
    voltages = nmc_voltages;
  }

  if (voltage_mV < voltages[0]) {
    return 0;
  }
  for (size_t i = 1; i < sizeof(nmc_voltages) / sizeof(nmc_voltages[0]); i++) {
    if (voltage_mV < voltages[i]) {
      // Linear interpolation between points
      uint16_t soc_low = (i - 1) * 100;
      uint16_t soc_high = i * 100;
      uint16_t voltage_low = voltages[i - 1];
      uint16_t voltage_high = voltages[i];
      return soc_low + ((voltage_mV - voltage_low) * (soc_high - soc_low)) / (voltage_high - voltage_low);
    }
  }
  return 10000;
}

static uint16_t soc_to_ocv(uint16_t soc_in_centipercent) {
  const uint16_t* voltages;
  if (datalayer.battery.info.chemistry == battery_chemistry_enum::LFP) {
    voltages = lfp_voltages;
  } else {
    voltages = nmc_voltages;
  }

  if (soc_in_centipercent >= 10000) {
    return voltages[100];
  }

  uint16_t index = soc_in_centipercent / 100;
  uint16_t remainder = soc_in_centipercent % 100;

  if (remainder == 0) {
    return voltages[index];
  }

  // Linear interpolation between points
  uint16_t voltage_low = voltages[index];
  uint16_t voltage_high = voltages[index + 1];
  return voltage_low + ((voltage_high - voltage_low) * remainder) / 100;
}

uint32_t Mg4Battery::calculate_max_discharge_power_W() {
  int32_t max_discharge_power_W = MAX_DISCHARGE_POWER_W;

  // Cellvoltage-based power derating. Taper linearly to zero over the last
  // DISCHARGE_TAPER_MV above working_cell_min_mV, then latch at zero (with
  // hysteresis) until the cell voltage recovers past the trip threshold.
  // Skipped if we have no fresh cell voltages (e.g. non-FD bus).
  if (cell_voltage_freshness > 0) {
    const int32_t cell_power_W = battery_discharge_power_by_cell_min(
        datalayer.battery.status.cell_min_voltage_mV, working_cell_min_mV, DISCHARGE_TAPER_MV, DISCHARGE_HYSTERESIS_MV,
        MAX_DISCHARGE_POWER_W, &voltageAtCellMin);
    if (cell_power_W < max_discharge_power_W) {
      max_discharge_power_W = cell_power_W;
    }
  }

  // Temperature-based power derating: high temperature limits both charge and
  // discharge. Skipped if we have no fresh temperature reading yet - a
  // default/uninitialized value of 0 dC must not be mistaken for an actual
  // at-limit reading.
  if (temp_freshness > 0) {
    const int32_t temp_high_power_W =
        battery_power_by_high_temp(datalayer.battery.status.temperature_max_dC, MAX_TEMP_DC, MAX_WATTS_PER_DC);
    if (temp_high_power_W < max_discharge_power_W) {
      max_discharge_power_W = temp_high_power_W;
    }
  }

  // SoC-based power derating: full power down to DERATE_DISCHARGE_BELOW_SOC,
  // then a linear taper to zero at DISCHARGE_MIN_SOC.
  const int32_t soc_power_W = battery_discharge_power_by_soc(datalayer.battery.status.real_soc, MAX_DISCHARGE_POWER_W,
                                                             DISCHARGE_MIN_SOC, DERATE_DISCHARGE_BELOW_SOC);
  if (soc_power_W < max_discharge_power_W) {
    max_discharge_power_W = soc_power_W;
  }

  return max_discharge_power_W > 0 ? max_discharge_power_W : 0;
}

uint32_t Mg4Battery::calculate_max_charge_power_W() {
  int32_t max_charge_power_W = MAX_CHARGE_POWER_W;

  // Cellvoltage-based power derating. Taper linearly to zero over the last
  // CHARGE_TAPER_MV below working_cell_max_mV, then latch at zero (with
  // hysteresis) until the cell voltage recovers past the trip threshold.
  // Skipped if we have no fresh cell voltages (e.g. non-FD bus).
  if (cell_voltage_freshness > 0) {
    const int32_t cell_power_W =
        battery_charge_power_by_cell_max(datalayer.battery.status.cell_max_voltage_mV, working_cell_max_mV,
                                         CHARGE_TAPER_MV, CHARGE_HYSTERESIS_MV, MAX_CHARGE_POWER_W, &voltageAtCellMax);
    if (cell_power_W < max_charge_power_W) {
      max_charge_power_W = cell_power_W;
    }
  }

  // Temperature-based power derating: high temperature limits both charge and
  // discharge, low temperature limits charge only. Skipped if we have no
  // fresh temperature reading yet - a default/uninitialized value of 0 dC
  // must not be mistaken for an actual at-limit reading (this previously
  // clamped LFP charge power to 0 W any time temperature hadn't been read
  // yet, since MIN_TEMP_LFP_DC is also 0).
  if (temp_freshness > 0) {
    const int32_t MIN_TEMP_DC =
        datalayer.battery.info.chemistry == battery_chemistry_enum::LFP ? MIN_TEMP_LFP_DC : MIN_TEMP_NMC_DC;
    const int32_t temp_high_power_W =
        battery_power_by_high_temp(datalayer.battery.status.temperature_max_dC, MAX_TEMP_DC, MAX_WATTS_PER_DC);
    const int32_t temp_low_power_W =
        battery_power_by_low_temp(datalayer.battery.status.temperature_min_dC, MIN_TEMP_DC, MIN_WATTS_PER_DC);
    if (temp_high_power_W < max_charge_power_W) {
      max_charge_power_W = temp_high_power_W;
    }
    if (temp_low_power_W < max_charge_power_W) {
      max_charge_power_W = temp_low_power_W;
    }
  }

  // SoC-based power derating: full power up to DERATE_CHARGE_ABOVE_SOC, then a
  // linear taper down to CHARGE_TRICKLE_POWER_W at 100%.
  const int32_t soc_power_W = battery_charge_power_by_soc(datalayer.battery.status.real_soc, MAX_CHARGE_POWER_W,
                                                          CHARGE_TRICKLE_POWER_W, DERATE_CHARGE_ABOVE_SOC);
  if (soc_power_W < max_charge_power_W) {
    max_charge_power_W = soc_power_W;
  }

  return max_charge_power_W > 0 ? max_charge_power_W : 0;
}

// uint32_t Mg4Battery::update_pack_max_voltage_limits() {
//   if(cell_voltage_freshness <= 0) {
//     return 0;
//   }

//   uint32_t max_voltage_limit_dV = (get_working_max_cell_voltage_mV() * datalayer.battery.info.number_of_cells) / 100;
//   uint32_t min_voltage_limit_dV = (get_working_min_cell_voltage_mV() * datalayer.battery.info.number_of_cells) / 100;

//   // Calculate the mean cell voltage based on the pack voltage
//   int32_t mean_cell_voltage_mV = (datalayer.battery.status.voltage_dV * 100) / datalayer.battery.info.number_of_cells;

//   // How far is the highest cell above the mean?
//   int32_t deviation_max_mV = datalayer.battery.status.cell_max_voltage_mV - mean_cell_voltage_mV;

//   // Calculate how much to reduce the voltage limit by to account for this
//   // deviation, to avoid overcharging the highest cell.
//   int32_t deviation_reduction_mV = deviation_max_mV * datalayer.battery.info.number_of_cells;
//   if(deviation_reduction_mV > 0) {
//       max_voltage_limit_dV -= deviation_reduction_mV / 100;
//   }
// }

static void update_soc(uint16_t soc_in_centipercent) {
  datalayer.battery.status.real_soc = soc_in_centipercent;

  uint32_t remaining_soc =
      datalayer.battery.status.real_soc > DISCHARGE_MIN_SOC ? datalayer.battery.status.real_soc - DISCHARGE_MIN_SOC : 0;
  uint32_t remaining = (datalayer.battery.info.total_capacity_Wh * remaining_soc) / (10000 - DISCHARGE_MIN_SOC);
  if (remaining > 0) {
    datalayer.battery.status.remaining_capacity_Wh = remaining;
  } else {
    datalayer.battery.status.remaining_capacity_Wh = 0;
  }
}

void Mg4Battery::
    update_values() {  //This function maps all the values fetched via CAN to the correct parameters used for modbus

  // Should be called every second

  if (temp_freshness > 0) {
    temp_freshness--;
  }

  if (coulombCounting) {
    if (cell_voltage_freshness <= 0) {
      // No valid cell voltage received yet, can't update
      datalayer.battery.status.max_charge_power_W = 0;
      datalayer.battery.status.max_discharge_power_W = 0;
      return;
    }

    uint32_t nominal_cell_voltage_mV = (datalayer.battery.info.chemistry == battery_chemistry_enum::LFP) ? 3300 : 3700;
    uint32_t capacity_mAh =
        ((datalayer.battery.info.total_capacity_Wh / datalayer.battery.info.number_of_cells) * 1000000) /
        nominal_cell_voltage_mV;
    uint32_t one_percent_dC = (capacity_mAh * (9 + 1)) / 25;  // 1.11% of capacity in deci-Coulombs

    if (!total_discharge_initialized) {
      if (nonvolatile_cookie != 0 && *nonvolatile_cookie == NONVOLATILE_COOKIE_VALUE) {
        // Non-volatile memory contains a valid previous discharge value, use it
        total_discharge_dC = *nonvolatile_total_discharge_dC;
        logging.printf("[MG4] Restored total discharge from non-volatile memory: %lu dC\n", total_discharge_dC);
      } else {
        // No previous value, Initialize the total discharge counter based on the min cell voltage
        uint16_t initial_soc_centipercent = ocv_to_soc(datalayer.battery.status.cell_min_voltage_mV);
        logging.printf("[MG4] Initial soc: %d\n", initial_soc_centipercent);
        logging.printf("[MG4] Capacity mAh: %lu\n", capacity_mAh);

        total_discharge_dC = ((uint64_t)(10000 - initial_soc_centipercent) * capacity_mAh * 36) / 10000;
      }
      logging.printf("[MG4] Initial total discharge: %lu dC\n", total_discharge_dC);
      total_discharge_initialized = true;
    }

    // 1. Do coulomb count
    if (datalayer.battery.status.current_dA > (int32_t)total_discharge_dC) {
      // We're charging, but discharge is nearly zero - cap at zero
      total_discharge_dC = 0;
    } else {
      // Subtract the (charging) current from the discharge counter
      total_discharge_dC -= datalayer.battery.status.current_dA;
    }

    // 2. State correction to handle drift
    if (datalayer.battery.status.cell_max_voltage_mV >= (working_cell_max_mV - 10)) {
      // We're full
      total_discharge_dC = 0;
    } else if (datalayer.battery.status.cell_max_voltage_mV < working_cell_recharge_threshold_mV &&
               total_discharge_dC < one_percent_dC) {
      // We've drifted - the voltage is low, but the counter is empty.
      // Force charge to 99%-SoC-equivalent to keep the battery charging.
      total_discharge_dC = one_percent_dC;
    }

    // 3. Save to NVRAM
    if (nonvolatile_total_discharge_dC != 0) {
      // Store the total discharged amount in non-volatile memory
      (*nonvolatile_total_discharge_dC) = total_discharge_dC;
      (*nonvolatile_cookie) = NONVOLATILE_COOKIE_VALUE;
    }

    // 4. Calculate SoC Centipercent
    int32_t soc_centipercent = 10000;
    if (capacity_mAh > 0) {
      soc_centipercent = 10000 - ((((uint64_t)total_discharge_dC * 2500) / 9) / capacity_mAh);
    }

    // 5. Cap the SoC to 0-100%
    if (soc_centipercent > 10000) {
      soc_centipercent = 10000;
    } else if (soc_centipercent < 0) {
      soc_centipercent = 0;
    }

    // 6. Handle empty/near-empty case
    if (datalayer.battery.status.cell_min_voltage_mV <= (working_cell_min_mV + 10)) {
      // Battery is empty
      soc_centipercent = 0;
    } else if (soc_centipercent < 100 && datalayer.battery.status.cell_min_voltage_mV > working_cell_min_mV) {
      // Count indicates empty but battery still has charge left, floor at 1%
      soc_centipercent = 100;
    }

    update_soc(soc_centipercent);

    if (cell_voltage_freshness > 0)
      cell_voltage_freshness--;
    datalayer.battery.status.max_charge_power_W = calculate_max_charge_power_W();
    datalayer.battery.status.max_discharge_power_W = calculate_max_discharge_power_W();
  } else {
    if (soc_freshness > 0) {
      soc_freshness--;
    }

    if (soc_freshness <= 0) {
      datalayer.battery.status.max_charge_power_W = 0;
      datalayer.battery.status.max_discharge_power_W = 0;
    } else {
      datalayer.battery.status.max_charge_power_W = calculate_max_charge_power_W();
      datalayer.battery.status.max_discharge_power_W = calculate_max_discharge_power_W();
    }
  }
}

void Mg4Battery::handle_incoming_can_frame(CAN_frame rx_frame) {
  if (handle_incoming_uds_can_frame(rx_frame)) {
    return;
  }

  uint16_t current_raw;
  uint32_t soc_times_ten;

  if (rx_frame.DLC > 8) {
    // FD bus frames
    switch (rx_frame.ID) {
      case 0x12C:
        datalayer.battery.status.CAN_battery_still_alive = CAN_STILL_ALIVE;

        datalayer.battery.status.voltage_dV = (((rx_frame.data.u8[8] << 4) | (rx_frame.data.u8[9] >> 4)) * 5) / 2;
        current_raw = ((rx_frame.data.u8[6] << 8) | rx_frame.data.u8[7]);
        if (current_raw <= 40000) {
          // Only allow plausible values (-1000A to +1000A)
          datalayer.battery.status.current_dA = -((current_raw - 20000) / 2);
        }

        datalayer.battery.status.cell_min_voltage_mV = ((rx_frame.data.u8[30] << 8) | (rx_frame.data.u8[31])) / 8;
        datalayer.battery.status.cell_max_voltage_mV = ((rx_frame.data.u8[32] << 8) | (rx_frame.data.u8[33])) / 8;

        datalayer.battery.status.temperature_max_dC = ((int)rx_frame.data.u8[19] * 5) - 400;
        datalayer.battery.status.temperature_min_dC = ((int)rx_frame.data.u8[22] * 5) - 400;
        temp_freshness = 10;

        cell_voltage_freshness = 10;
        reportsFDVoltages = true;

        break;
      case 0x159:
        // Cellvoltages/temps

        // Loop through the subframes in the message
        for (int i = 0; i < rx_frame.DLC; i += 12) {
          uint8_t length = rx_frame.data.u8[i + 3];
          if (length != 8) {
            // Unexpected length, give up
            break;
          }
          // Get the subframe address and data
          uint32_t addr = (rx_frame.data.u8[i] << 16) | (rx_frame.data.u8[i + 1] << 8) | rx_frame.data.u8[i + 2];
          const uint8_t* sub = &rx_frame.data.u8[i + 4];

          if (addr == 0x509 || addr == 0x510) {
            // Cell voltages

            uint8_t mux = sub[7];
            // 0x509 frames cover cells 1-80, 0x510 frames cover cells 81-104
            int celloffset = (addr == 0x509) ? (mux - 1) : 20 + (mux - 1);

            // Unpack the 4 cell voltages
            uint16_t c0 = (sub[0] << 5) | ((sub[1] & 0xF8) >> 3);
            uint16_t c1 = (sub[2] << 5) | ((sub[3] & 0xF8) >> 3);
            uint16_t c2 = ((sub[3] & 0x07) << 10) | (sub[4] << 2) | ((sub[5] & 0xC0) >> 6);
            uint16_t c3 = ((sub[5] & 0x1F) << 8) | sub[6];

            int idx = celloffset * 4;
            if (idx + 3 < MAX_AMOUNT_CELLS) {
              // This seemingly arbitrary ordering is guessed based on the
              // min/max cell indices given in the 12C messages.
              datalayer.battery.status.cell_voltages_mV[idx + 0] = c2;
              datalayer.battery.status.cell_voltages_mV[idx + 1] = c3;
              datalayer.battery.status.cell_voltages_mV[idx + 2] = c1;
              datalayer.battery.status.cell_voltages_mV[idx + 3] = c0;
            }
          } else if (addr == 0x511) {
            // Temps

            uint8_t mux = sub[7];
            int module_idx = mux - 1;

            for (int j = 0; j < 6; j++) {
              int16_t temp_dC = (sub[j + 1] - 40) * 10;  // Convert from -40..215 range to dC
              int idx = (module_idx * 6) + j;
              if (idx < 12) {
                module_temperatures_dC[idx] = temp_dC;
              }
            }

            if (module_temps_received == module_idx) {
              // Record that we have valid temps for this module. Will saturate
              // at 2 once we have them all.
              module_temps_received++;
            }

            if (mux == 2 && module_temps_received == 2) {
              // Update the overall min/max temps based on the module temps once we have them all
              int16_t temp_min_dC = module_temperatures_dC[0];
              int16_t temp_max_dC = module_temperatures_dC[0];
              for (int j = 0; j < 12; j++) {
                if (module_temperatures_dC[j] < temp_min_dC) {
                  temp_min_dC = module_temperatures_dC[j];
                }
                if (module_temperatures_dC[j] > temp_max_dC) {
                  temp_max_dC = module_temperatures_dC[j];
                }
              }
              datalayer.battery.status.temperature_min_dC = temp_min_dC;
              datalayer.battery.status.temperature_max_dC = temp_max_dC;
              temp_freshness = 10;
            }
          }
          // Cell module temps are in 0x511
        }
        break;
      case 0x15B:
        // SoC

        //00050108001A21 01 80 00000000050508... ~10% SoC
        //00050108007B9A 49 64 00000000050508... ~60% SoC
        //                ^ ^^
        //          bits: 4 42

        soc_times_ten = ((rx_frame.data.u8[7] << 6) | (rx_frame.data.u8[8] >> 2)) & 0x3FF;
        if (!coulombCounting) {
          update_soc(soc_times_ten * 10);
          soc_freshness = 10;
        }

        // Precharge/contactor state, confirmed against a real vehicle
        // capture: 3=idle, 11=precharge active, 7=closed/charging. Single
        // source of truth for the contactor state machine, the
        // contactors_engaged reporting and the UDS info page.
        if ((rx_frame.data.u8[21] & 0x0F) != pack_contactors.state) {
          pack_contactors.state = rx_frame.data.u8[21] & 0x0F;
          logging.printf("[MG4] Precharge/contactor state changed to %d\n", pack_contactors.state);
        }
        pack_contactors.received = true;

        // Reflect the pack's own contactor state on the main BE page.
        datalayer.system.status.contactors_engaged = pack_contactors.contactsEngaged();
        break;
      default:
        break;
    }
  } else {
    // Non-FD bus frames
    switch (rx_frame.ID) {
      case 0x12C:
        datalayer.battery.status.CAN_battery_still_alive = CAN_STILL_ALIVE;

        if (!reportsFDVoltages) {
          datalayer.battery.status.voltage_dV = (((rx_frame.data.u8[4] << 4) | (rx_frame.data.u8[5] >> 4)) * 5) / 2;
          datalayer.battery.status.current_dA = -(((rx_frame.data.u8[2] << 8) | rx_frame.data.u8[3]) - 20000) / 2;
        }
        break;
      case 0x401:
        soc_times_ten = ((rx_frame.data.u8[6] << 8) | rx_frame.data.u8[7]) & 0x3FF;

        if (soc_times_ten <= 1000) {
          if (!coulombCounting) {
            update_soc(soc_times_ten * 10);
            soc_freshness = 10;
          }
          reportsSoC = true;
        }

        break;
      default:
        break;
    }
  }
}

// ===========================================================================
// FD frame generators for the contactor-closing sequence (0x047, 0x08A, 0x313,
// 0x314, 0x315).
//
// These are the runtime form of the standalone generators that used to live
// in mg4_dev/*cycle_opt.cpp. Each FD payload is a sequence of back-to-back
// 12-byte subfields with the layout:
//
//     [3-byte subaddr][len = 0x08][CRC-8][7 payload bytes]
//
// CRC-8: poly 0x1D, init 0x00, MSB-first, no reflection, no xorout, computed
// over the 7 payload bytes that follow the CRC.
//
// Dynamic fields are either modelled with the shared integer curve helpers
// below (precharge exponential, linear ramps) or run-length encoded from the
// original capture. Each build_frame_xxx() call reproduces the same frame
// that the corresponding replay table entry used to hold, but from a
// fraction of the data.
// ===========================================================================

// Run-length encoded field: `value` is held for `count` consecutive frames.
struct RleRun {
  uint16_t value;
  uint16_t count;
};

// CRC-8 (poly 0x1D, init 0x00, MSB-first, no reflection, no xorout) over the
// 7 payload bytes following the CRC slot.
static uint8_t payload_crc8(const uint8_t* d) {
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < 7; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x1D) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

template <size_t N>
static uint16_t rle_lookup(const RleRun (&runs)[N], int i) {
  for (size_t r = 0; r < N; r++) {
    if (i < (int)runs[r].count) {
      return runs[r].value;
    }
    i -= runs[r].count;
  }
  return 0;  // index past end of segment - should not happen
}

static const uint32_t CURVE_ONE = 1u << 16;  // 1.0 in Q16

// --- Delayed exponential precharge curve (0x047 VAL12 / 0x313 VAL16) -------
// The plateau this approaches is not fixed: the frame builders scale the live
// pack voltage (datalayer.battery.status.voltage_dV) by a per-frame factor.
static const uint32_t PRECHARGE_DELAY_FRAMES = 250;  // 10 ms frames before the ramp
static const uint32_t PRECHARGE_RATE = 66;           // ~0.258 of the gap per frame
static const uint32_t PRECHARGE_RATE_SHIFT = 8;
static const uint32_t PRECHARGE_RAMP_STEPS = 50;  // tabulated ramp steps

// Normalised (0..CURVE_ONE) precharge shape at time t, in 0x047 10 ms frames.
static uint32_t precharge_shape_q16(uint32_t t) {
  static uint16_t ramp[PRECHARGE_RAMP_STEPS + 1];
  static bool built = false;
  if (!built) {
    uint32_t remaining = CURVE_ONE;
    for (uint32_t n = 0; n <= PRECHARGE_RAMP_STEPS; n++) {
      remaining -= (remaining * PRECHARGE_RATE) >> PRECHARGE_RATE_SHIFT;
      ramp[n] = (uint16_t)(CURVE_ONE - remaining);
    }
    built = true;
  }
  if (t < PRECHARGE_DELAY_FRAMES) {
    return 0;
  }
  uint32_t n = t - PRECHARGE_DELAY_FRAMES;
  if (n > PRECHARGE_RAMP_STEPS) {
    n = PRECHARGE_RAMP_STEPS;
  }
  return ramp[n];
}

// --- Linear ramp -----------------------------------------------------------
// 0 for t <= start, straight line to CURVE_ONE at t == end, held afterwards.
static uint32_t linear_shape_q16(uint32_t t, uint32_t start, uint32_t end) {
  if (t <= start) {
    return 0;
  }
  if (t >= end) {
    return CURVE_ONE;
  }
  return (uint32_t)(((uint64_t)(t - start) * CURVE_ONE) / (end - start));
}

// Map a normalised shape into [dc, scale] (rounded to nearest; scale >= dc).
static uint16_t shape_value(uint32_t shape_q16, uint16_t dc, uint16_t scale) {
  uint32_t range = (uint32_t)(scale - dc);
  return (uint16_t)(dc + ((range * shape_q16 + CURVE_ONE / 2) >> 16));
}

// --- Rolling counter -------------------------------------------------------
// 15 values cycling with wrap, starting at base + offset on frame 0.
static uint8_t rolling_counter(uint8_t base, int i, int offset) {
  return (uint8_t)(base + ((i + offset) % 15));
}

// --- Signal shapes shared between the 0x313, 0x314 and 0x315 generators ----
// 0x313 subfield 2 and 0x315 subfield 1 both carry this VALB companion byte
// (~0x78 -> 0x75 around the precharge ramp).
static const RleRun VALB_RLE[14] = {
    {0x78, 37}, {0x79, 1}, {0x78, 1}, {0x76, 2}, {0x78, 2}, {0x79, 1}, {0x78, 3},
    {0x77, 9},  {0x76, 8}, {0x77, 1}, {0x76, 7}, {0x75, 1}, {0x76, 2}, {0x75, 5},
};

// Linear ramp shared by 0x313's VALA/VALC and 0x315 subfield 1's VALA: idle
// until frame 32, straight line to VALA_MAX at frame 78, then held.
static const int VALA_RAMP_START = 32;
static const int VALA_RAMP_END = 78;
static const uint16_t VALA_MAX = 77;

// Linear ramp timing shared by 0x314 subfield 2's VAL and 0x315 subfield 2's
// bytes 7/9: idle until frame 44, reaching the field maximum at frame 59.
static const int VAL_RAMP_START = 44;
static const int VAL_RAMP_END = 59;

// Frame-segment lengths (number of frames in each generated segment).
static const int CYCLE_LEN_047 = 800;
static const int CYCLE_LEN_08A = 800;
static const int CYCLE_LEN_313 = 80;
static const int CYCLE_LEN_314 = 80;
static const int CYCLE_LEN_315 = 80;

// ===========================================================================
// 0x047 (24-byte FD payload, two 12-byte subfields)
// ===========================================================================

static const uint16_t VAL12_047_A_DC = 45;        // idle / DC offset
static const uint32_t VAL12_047_A_SCALE_NUM = 2;  // frame value = 0.4 x voltage_dV
static const uint32_t VAL12_047_A_SCALE_DEN = 5;
static const uint16_t VAL12_047_A_FIELD_MAX = 0xFFF;  // 12-bit payload field saturation

// Subfield A: MOD value (0x00/0x01/0x02), run-length encoded
static const RleRun VAL12_047_A_MOD_RLE[124] = {
    {0x01, 2}, {0x02, 3}, {0x01, 3}, {0x02, 1},   {0x01, 1}, {0x02, 4}, {0x01, 2}, {0x02, 1}, {0x01, 2}, {0x02, 1},
    {0x01, 1}, {0x02, 2}, {0x01, 1}, {0x02, 3},   {0x01, 2}, {0x02, 3}, {0x01, 1}, {0x02, 1}, {0x01, 2}, {0x02, 2},
    {0x01, 3}, {0x02, 1}, {0x01, 2}, {0x02, 2},   {0x01, 1}, {0x02, 1}, {0x01, 2}, {0x02, 1}, {0x01, 2}, {0x02, 1},
    {0x01, 5}, {0x02, 2}, {0x01, 5}, {0x02, 2},   {0x01, 6}, {0x02, 2}, {0x01, 1}, {0x02, 1}, {0x01, 2}, {0x02, 5},
    {0x01, 1}, {0x02, 4}, {0x01, 1}, {0x02, 1},   {0x01, 2}, {0x02, 5}, {0x01, 2}, {0x02, 6}, {0x01, 1}, {0x02, 4},
    {0x01, 4}, {0x02, 1}, {0x01, 3}, {0x02, 4},   {0x01, 1}, {0x02, 2}, {0x01, 1}, {0x02, 4}, {0x01, 1}, {0x02, 8},
    {0x01, 1}, {0x02, 2}, {0x01, 2}, {0x02, 5},   {0x01, 5}, {0x02, 1}, {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 1},
    {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 1},   {0x01, 3}, {0x02, 2}, {0x01, 1}, {0x02, 1}, {0x01, 2}, {0x02, 2},
    {0x01, 3}, {0x02, 1}, {0x01, 3}, {0x02, 1},   {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 3},
    {0x01, 1}, {0x02, 2}, {0x01, 1}, {0x02, 1},   {0x01, 8}, {0x02, 1}, {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 1},
    {0x01, 1}, {0x02, 3}, {0x01, 3}, {0x02, 2},   {0x01, 3}, {0x02, 3}, {0x01, 1}, {0x02, 1}, {0x01, 1}, {0x02, 1},
    {0x01, 2}, {0x02, 2}, {0x01, 2}, {0x02, 2},   {0x01, 2}, {0x02, 4}, {0x01, 3}, {0x02, 2}, {0x01, 1}, {0x02, 1},
    {0x01, 3}, {0x02, 2}, {0x01, 1}, {0x00, 544},
};

// Subfield B: 0x9F/A0 flag byte, run-length encoded
static const RleRun VAL12_047_B_FLAG_RLE[92] = {
    {0x9F, 260}, {0xA0, 12}, {0x9F, 3}, {0xA0, 16}, {0x9F, 1}, {0xA0, 2},  {0x9F, 3}, {0xA0, 2},  {0x9F, 1}, {0xA0, 7},
    {0x9F, 1},   {0xA0, 16}, {0x9F, 1}, {0xA0, 6},  {0x9F, 1}, {0xA0, 35}, {0x9F, 2}, {0xA0, 22}, {0x9F, 4}, {0xA0, 4},
    {0x9F, 2},   {0xA0, 6},  {0x9F, 7}, {0xA0, 1},  {0x9F, 1}, {0xA0, 5},  {0x9F, 2}, {0xA0, 5},  {0x9F, 1}, {0xA0, 1},
    {0x9F, 3},   {0xA0, 3},  {0x9F, 2}, {0xA0, 1},  {0x9F, 1}, {0xA0, 1},  {0x9F, 3}, {0xA0, 40}, {0x9F, 2}, {0xA0, 3},
    {0x9F, 2},   {0xA0, 1},  {0x9F, 6}, {0xA0, 4},  {0x9F, 3}, {0xA0, 9},  {0x9F, 2}, {0xA0, 1},  {0x9F, 1}, {0xA0, 2},
    {0x9F, 2},   {0xA0, 6},  {0x9F, 3}, {0xA0, 11}, {0x9F, 1}, {0xA0, 11}, {0x9F, 1}, {0xA0, 1},  {0x9F, 1}, {0xA0, 1},
    {0x9F, 4},   {0xA0, 1},  {0x9F, 1}, {0xA0, 28}, {0x9F, 1}, {0xA0, 32}, {0x9F, 2}, {0xA0, 10}, {0x9F, 2}, {0xA0, 21},
    {0x9F, 1},   {0xA0, 43}, {0x9F, 1}, {0xA0, 43}, {0x9F, 1}, {0xA0, 1},  {0x9F, 2}, {0xA0, 4},  {0x9F, 1}, {0xA0, 3},
    {0x9F, 4},   {0xA0, 4},  {0x9F, 1}, {0xA0, 6},  {0x9F, 1}, {0xA0, 1},  {0x9F, 1}, {0xA0, 4},  {0x9F, 2}, {0xA0, 6},
    {0x9F, 1},   {0xA0, 9},
};

static const uint8_t BASE_047_A[12] = {0x00, 0x01, 0x27, 0x08, 0x00, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00};
static const uint8_t BASE_047_B[12] = {0x00, 0x01, 0x48, 0x08, 0x00, 0x00, 0x6A, 0x06, 0x00, 0xFF, 0xF0, 0xFF};

// Assemble the 24-byte 0x047 FD payload for frame index i. The 12-bit VAL12
// plateau tracks the live pack voltage (0.4 x voltage_dV at full precharge),
// so `voltage_dV` comes from datalayer.battery.status.voltage_dV.
static void build_frame_047(int i, uint16_t voltage_dV, uint8_t out[24]) {
  uint32_t target = ((uint32_t)voltage_dV * VAL12_047_A_SCALE_NUM) / VAL12_047_A_SCALE_DEN;
  if (target < VAL12_047_A_DC) {
    target = VAL12_047_A_DC;
  } else if (target > VAL12_047_A_FIELD_MAX) {
    target = VAL12_047_A_FIELD_MAX;
  }

  // Rolling counter: 0xF0..0xFE, starting at 0xFB on frame 0 (skips 0xFF)
  uint8_t cnt = rolling_counter(0xF0, i, 11);

  uint16_t val12 = shape_value(precharge_shape_q16((uint32_t)i), VAL12_047_A_DC, (uint16_t)target);
  //uint16_t mod = rle_lookup(VAL12_047_A_MOD_RLE, i);
  uint16_t mod = 0x01;
  //uint16_t flag = rle_lookup(VAL12_047_B_FLAG_RLE, i);
  uint16_t flag = 0x9F;

  // Subfield A
  uint8_t a[12];
  memcpy(a, BASE_047_A, 12);
  a[5] = cnt;
  a[9] = (uint8_t)(val12 >> 4);                    // VAL12 high 8 bits
  a[10] = (uint8_t)(((val12 & 0xF) << 4) | 0x0C);  // VAL12 low 4 bits + static 0xC nibble
  a[11] = (uint8_t)mod;
  a[4] = payload_crc8(&a[5]);

  // Subfield B
  uint8_t b[12];
  memcpy(b, BASE_047_B, 12);
  b[5] = cnt;
  b[8] = (uint8_t)flag;
  b[4] = payload_crc8(&b[5]);

  memcpy(out, a, 12);
  memcpy(out + 12, b, 12);
}

// ===========================================================================
// 0x08A (48-byte FD payload, four 12-byte subfields)
// ===========================================================================

// Subfield 1: VAL payload byte (0x2D-0x30), 102 runs over 800 frames
static const RleRun S118_08A_VAL_RLE[102] = {
    {0x2D, 28}, {0x2E, 1},  {0x2D, 2},  {0x2E, 1},  {0x2D, 12}, {0x2E, 1},  {0x2D, 169}, {0x2E, 1},  {0x2D, 11},
    {0x2E, 2},  {0x2D, 26}, {0x2E, 1},  {0x2F, 5},  {0x30, 12}, {0x2F, 3},  {0x30, 16},  {0x2F, 1},  {0x30, 2},
    {0x2F, 3},  {0x30, 10}, {0x2F, 1},  {0x30, 16}, {0x2F, 1},  {0x30, 6},  {0x2F, 1},   {0x30, 35}, {0x2F, 2},
    {0x30, 22}, {0x2F, 4},  {0x30, 4},  {0x2F, 2},  {0x30, 6},  {0x2F, 7},  {0x30, 1},   {0x2F, 1},  {0x30, 5},
    {0x2F, 2},  {0x30, 5},  {0x2F, 1},  {0x30, 1},  {0x2F, 3},  {0x30, 3},  {0x2F, 2},   {0x30, 1},  {0x2F, 1},
    {0x30, 1},  {0x2F, 3},  {0x30, 40}, {0x2F, 2},  {0x30, 3},  {0x2F, 2},  {0x30, 1},   {0x2F, 6},  {0x30, 4},
    {0x2F, 3},  {0x30, 9},  {0x2F, 2},  {0x30, 1},  {0x2F, 1},  {0x30, 2},  {0x2F, 2},   {0x30, 6},  {0x2F, 3},
    {0x30, 11}, {0x2F, 1},  {0x30, 11}, {0x2F, 1},  {0x30, 1},  {0x2F, 1},  {0x30, 1},   {0x2F, 4},  {0x30, 1},
    {0x2F, 1},  {0x30, 28}, {0x2F, 1},  {0x30, 32}, {0x2F, 2},  {0x30, 10}, {0x2F, 2},   {0x30, 21}, {0x2F, 1},
    {0x30, 43}, {0x2F, 1},  {0x30, 43}, {0x2F, 1},  {0x30, 1},  {0x2F, 2},  {0x30, 4},   {0x2F, 1},  {0x30, 3},
    {0x2F, 4},  {0x30, 4},  {0x2F, 1},  {0x30, 6},  {0x2F, 1},  {0x30, 1},  {0x2F, 1},   {0x30, 4},  {0x2F, 2},
    {0x30, 6},  {0x2F, 1},  {0x30, 9},
};

// Subfield 2: MODE payload byte (0x00/0x01/0x21), 3 runs
static const RleRun S100_08A_MODE_RLE[3] = {
    {0x00, 190},
    {0x01, 119},
    {0x21, 491},
};

// Subfield 2: FLAG payload byte (0x00/0x08), 3 runs
static const RleRun S100_08A_FLAG_RLE[3] = {
    {0x00, 188},
    {0x08, 219},
    {0x00, 393},
};

// Subfield 2: LSB payload byte (0xFE/0xFF), flickers, 351 runs
static const RleRun S100_08A_LSB_RLE[351] = {
    {0xFF, 2}, {0xFE, 1}, {0xFF, 2}, {0xFE, 7}, {0xFF, 3}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 2},
    {0xFF, 1}, {0xFE, 2}, {0xFF, 2}, {0xFE, 2}, {0xFF, 2}, {0xFE, 1},  {0xFF, 2}, {0xFE, 4},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 4}, {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 4}, {0xFF, 2}, {0xFE, 5}, {0xFF, 2}, {0xFE, 4},  {0xFF, 2}, {0xFE, 6},  {0xFF, 2}, {0xFE, 4},
    {0xFF, 4}, {0xFE, 4}, {0xFF, 1}, {0xFE, 8}, {0xFF, 3}, {0xFE, 3},  {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 2}, {0xFE, 1}, {0xFF, 1}, {0xFE, 1},  {0xFF, 3}, {0xFE, 1},  {0xFF, 1}, {0xFE, 2},
    {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 1}, {0xFF, 2}, {0xFE, 4},  {0xFF, 1}, {0xFE, 5},  {0xFF, 2}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 2}, {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 5},  {0xFF, 2}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 2}, {0xFE, 2}, {0xFF, 1}, {0xFE, 6},  {0xFF, 2}, {0xFE, 12}, {0xFF, 2}, {0xFE, 4},
    {0xFF, 1}, {0xFE, 7}, {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 1},  {0xFF, 2}, {0xFE, 8},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 9},  {0xFF, 1}, {0xFE, 6},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 3}, {0xFE, 4}, {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 4},
    {0xFF, 1}, {0xFE, 4}, {0xFF, 1}, {0xFE, 3}, {0xFF, 5}, {0xFE, 2},  {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 2}, {0xFE, 4}, {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 3},
    {0xFF, 5}, {0xFE, 1}, {0xFF, 3}, {0xFE, 6}, {0xFF, 1}, {0xFE, 11}, {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 7}, {0xFF, 2}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 2}, {0xFF, 2}, {0xFE, 3}, {0xFF, 2}, {0xFE, 12}, {0xFF, 2}, {0xFE, 2},  {0xFF, 1}, {0xFE, 2},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 2},  {0xFF, 2}, {0xFE, 4},
    {0xFF, 2}, {0xFE, 2}, {0xFF, 3}, {0xFE, 1}, {0xFF, 2}, {0xFE, 2},  {0xFF, 1}, {0xFE, 3},  {0xFF, 1}, {0xFE, 2},
    {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 5},  {0xFF, 1}, {0xFE, 2},  {0xFF, 2}, {0xFE, 3},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 2}, {0xFF, 3}, {0xFE, 2},  {0xFF, 1}, {0xFE, 3},  {0xFF, 2}, {0xFE, 9},
    {0xFF, 2}, {0xFE, 4}, {0xFF, 2}, {0xFE, 2}, {0xFF, 1}, {0xFE, 1},  {0xFF, 3}, {0xFE, 2},  {0xFF, 1}, {0xFE, 6},
    {0xFF, 2}, {0xFE, 2}, {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 5},  {0xFF, 2}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 4}, {0xFF, 1}, {0xFE, 2}, {0xFF, 1}, {0xFE, 5},  {0xFF, 2}, {0xFE, 2},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 3}, {0xFF, 1}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 2},
    {0xFF, 1}, {0xFE, 5}, {0xFF, 1}, {0xFE, 1}, {0xFF, 3}, {0xFE, 6},  {0xFF, 2}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 4}, {0xFF, 1}, {0xFE, 1}, {0xFF, 2}, {0xFE, 1},  {0xFF, 2}, {0xFE, 1},  {0xFF, 3}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 2}, {0xFF, 2}, {0xFE, 1}, {0xFF, 3}, {0xFE, 1},  {0xFF, 2}, {0xFE, 4},  {0xFF, 2}, {0xFE, 3},
    {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 6}, {0xFF, 1}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 1}, {0xFE, 2}, {0xFF, 3}, {0xFE, 5}, {0xFF, 1}, {0xFE, 2},  {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 3}, {0xFE, 2}, {0xFF, 1}, {0xFE, 3}, {0xFF, 2}, {0xFE, 11}, {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 5},
    {0xFF, 1}, {0xFE, 6}, {0xFF, 1}, {0xFE, 2}, {0xFF, 1}, {0xFE, 3},  {0xFF, 2}, {0xFE, 2},  {0xFF, 3}, {0xFE, 1},
    {0xFF, 2}, {0xFE, 1}, {0xFF, 2}, {0xFE, 3}, {0xFF, 1}, {0xFE, 4},  {0xFF, 1}, {0xFE, 2},  {0xFF, 3}, {0xFE, 4},
    {0xFF, 5}, {0xFE, 6}, {0xFF, 1}, {0xFE, 1}, {0xFF, 1}, {0xFE, 3},  {0xFF, 1}, {0xFE, 1},  {0xFF, 1}, {0xFE, 1},
    {0xFF, 2},
};

// Subfield 3: LEVEL payload byte (0x00/0x20/0x40), 3 runs
static const RleRun S153_08A_LEVEL_RLE[3] = {
    {0x00, 307},
    {0x20, 30},
    {0x40, 463},
};

static const uint8_t BASE_08A_S118[12] = {0x00, 0x01, 0x18, 0x08, 0x00, 0x00, 0x75, 0x00, 0x75, 0x30, 0x75, 0x30};
static const uint8_t BASE_08A_S100[12] = {0x00, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0x00};
static const uint8_t BASE_08A_S153[12] = {0x00, 0x01, 0x53, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Assemble the 48-byte 0x08A FD payload for frame index i.
static void build_frame_08a(int i, uint8_t out[48]) {
  uint8_t s1[12], s2[12], s3[12];

  // Subfield 1 (00 01 18)
  memcpy(s1, BASE_08A_S118, 12);
  s1[5] = rolling_counter(0x30, i, 11);
  //s1[7] = (uint8_t)rle_lookup(S118_08A_VAL_RLE, i);
  s1[7] = 0x2F;
  s1[4] = payload_crc8(&s1[5]);

  // Subfield 2 (00 01 00)
  memcpy(s2, BASE_08A_S100, 12);
  s2[5] = rolling_counter(0x40, i, 11);
  // I suspect S100_08A_MODE_RLE is the contactor close request?
  s2[7] = (uint8_t)rle_lookup(S100_08A_MODE_RLE, i);
  s2[8] = (uint8_t)rle_lookup(S100_08A_FLAG_RLE, i);
  //s2[11] = (uint8_t)rle_lookup(S100_08A_LSB_RLE, i);
  s2[11] = 0xFF;
  s2[4] = payload_crc8(&s2[5]);

  // Subfield 3 (00 01 53) - CRC slot stays 0x00, as captured
  memcpy(s3, BASE_08A_S153, 12);
  s3[6] = (uint8_t)rle_lookup(S153_08A_LEVEL_RLE, i);

  memcpy(out, s1, 12);
  memcpy(out + 12, s2, 12);
  memcpy(out + 24, s3, 12);
  memset(out + 36, 0, 12);  // subfield 4: all-zero padding
}

// ===========================================================================
// 0x313 (48-byte FD payload, four 12-byte subfields)
// ===========================================================================

// Subfield 1 (00 04 02): VAL1 payload byte (0x52-0x54), 3 runs
static const RleRun S1_313_VAL1_RLE[3] = {
    {0x52, 9},
    {0x53, 24},
    {0x54, 47},
};

// Subfield 1 (00 04 02): VAL2 payload byte (0x53-0x56), 4 runs
static const RleRun S1_313_VAL2_RLE[4] = {
    {0x53, 5},
    {0x54, 21},
    {0x55, 37},
    {0x56, 17},
};

// Subfield 1 (00 04 02): STAT payload byte (0x01/0x03/0x05), 3 runs
static const RleRun S1_313_STAT_RLE[3] = {
    {0x01, 10},
    {0x03, 23},
    {0x05, 47},
};

// Subfield 1 (00 04 02): FLAG payload byte (0xE3/0xE7), 2 runs
static const RleRun S1_313_FLAG_RLE[2] = {
    {0xE3, 33},
    {0xE7, 47},
};

// Subfield 2 (00 04 00): VALC shares the shared VALA ramp timing above but
// has its own maximum. VALB is the shared 0x313/0x315 table.
static const uint16_t S2_313_VALC_MAX = 90;

// Subfield 2 (00 04 00): VALD payload byte (0x68/0x69), 2 runs
static const RleRun S2_313_VALD_RLE[2] = {
    {0x68, 1},
    {0x69, 79},
};

// Subfield 3 (00 04 01): VAL16 is the same delayed-exponential precharge
// curve as 0x047's VAL12, sampled every 10th 0x047 frame. Its plateau also
// tracks the live pack voltage, but 12.5x larger: the 0x313 value is
// 5 x voltage_dV at full precharge.
static const uint16_t S3_313_VAL16_DC = 562;   // 12.5 x 45, idle
static const uint32_t S3_313_VAL16_SCALE = 5;  // frame value = 5 x voltage_dV
static const uint16_t S3_313_VAL16_FIELD_MAX = 0xFFFF;
static const int S3_313_VAL16_SKIP = 2;  // 313 frames of capture offset

static const uint8_t BASE_313_S1[12] = {0x00, 0x04, 0x02, 0x08, 0x00, 0x00, 0x3D, 0x00, 0x00, 0xF2, 0x00, 0x00};
static const uint8_t BASE_313_S2[12] = {0x00, 0x04, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x80, 0x08, 0x00, 0x00};
static const uint8_t BASE_313_S3[12] = {0x00, 0x04, 0x01, 0x08, 0x00, 0x00, 0xFF, 0x4C, 0x00, 0x00, 0x00, 0x00};

// Assemble the 48-byte 0x313 FD payload for frame index i. Like 0x047, the
// VAL16 plateau tracks the live pack voltage (5 x voltage_dV).
static void build_frame_313(int i, uint16_t voltage_dV, uint8_t out[48]) {
  uint8_t s1[12], s2[12], s3[12];

  // Subfield 1 (00 04 02)
  memcpy(s1, BASE_313_S1, 12);
  s1[5] = rolling_counter(0xF0, i, 7);
  s1[7] = (uint8_t)rle_lookup(S1_313_VAL1_RLE, i);
  s1[8] = (uint8_t)rle_lookup(S1_313_VAL2_RLE, i);
  s1[10] = (uint8_t)rle_lookup(S1_313_STAT_RLE, i);
  s1[11] = (uint8_t)rle_lookup(S1_313_FLAG_RLE, i);
  s1[4] = payload_crc8(&s1[5]);

  // Subfield 2 (00 04 00) - CRC slot stays 0x00 and byte 5 stays 0x01, as captured
  uint32_t ramp = linear_shape_q16((uint32_t)i, (uint32_t)VALA_RAMP_START, (uint32_t)VALA_RAMP_END);
  uint16_t valc = shape_value(ramp, 0, S2_313_VALC_MAX);
  memcpy(s2, BASE_313_S2, 12);
  s2[6] = (uint8_t)shape_value(ramp, 0, VALA_MAX);
  s2[7] = (uint8_t)rle_lookup(VALB_RLE, i);
  s2[8] = (uint8_t)(0x80 | (valc >> 4));          // static hi nibble 8 + VALC hi nibble
  s2[9] = (uint8_t)(((valc & 0xF) << 4) | 0x08);  // VALC lo nibble + static lo nibble 8
  s2[10] = (uint8_t)rle_lookup(S2_313_VALD_RLE, i);

  // Subfield 3 (00 04 01)
  uint32_t target16 = (uint32_t)voltage_dV * S3_313_VAL16_SCALE;
  if (target16 < S3_313_VAL16_DC) {
    target16 = S3_313_VAL16_DC;
  } else if (target16 > S3_313_VAL16_FIELD_MAX) {
    target16 = S3_313_VAL16_FIELD_MAX;
  }
  uint32_t t16 = (i > S3_313_VAL16_SKIP) ? (uint32_t)(i - S3_313_VAL16_SKIP) * 10u : 0u;
  uint16_t val16 = shape_value(precharge_shape_q16(t16), S3_313_VAL16_DC, (uint16_t)target16);
  memcpy(s3, BASE_313_S3, 12);
  s3[5] = rolling_counter(0x30, i, 7);
  s3[8] = (uint8_t)(val16 >> 8);    // VAL16 high byte
  s3[9] = (uint8_t)(val16 & 0xFF);  // VAL16 low byte
  s3[4] = payload_crc8(&s3[5]);

  memcpy(out, s1, 12);
  memcpy(out + 12, s2, 12);
  memcpy(out + 24, s3, 12);
  memset(out + 36, 0, 12);  // subfield 4: all-zero padding
}

// ===========================================================================
// 0x314 (24-byte FD payload, two 12-byte subfields)
// ===========================================================================

// Subfield 1 (00 04 04): VAL payload byte, 5 runs
static const RleRun S1_314_VAL_RLE[5] = {
    {0x02, 27}, {0x2E, 1}, {0x4C, 1}, {0x56, 35}, {0x57, 16},
};

// Subfield 1 (00 04 04): HI payload byte (top 2 bits vary, low 6 static 0x08)
static const RleRun S1_314_HI_RLE[7] = {
    {0xC8, 27}, {0x08, 1}, {0xC8, 1}, {0x48, 2}, {0x88, 1}, {0xC8, 32}, {0x08, 16},
};

// Subfield 1 (00 04 04): FLAG payload byte (0x7F/0x80), 3 runs
static const RleRun S1_314_FLAG_RLE[3] = {
    {0x7F, 57},
    {0x80, 7},
    {0x7F, 16},
};

// Subfield 1 (00 04 04): STAT payload byte (0x04/0xE0/0xE4), 4 runs
static const RleRun S1_314_STAT_RLE[4] = {
    {0xE0, 32},
    {0xE4, 25},
    {0x04, 7},
    {0xE4, 16},
};

// Subfield 2 (00 04 05): VAL ramps from idle to a peak, then holds. It reuses
// the shared VAL ramp timing defined above.
static const uint16_t S2_314_VAL_MAX = 48;

// Subfield 2 (00 04 05): CNT2 payload byte (0x00-0x04), 5 runs
static const RleRun S2_314_CNT2_RLE[5] = {
    {0x00, 48}, {0x01, 3}, {0x02, 4}, {0x03, 2}, {0x04, 23},
};

static const uint8_t BASE_314_S1[12] = {0x00, 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x56};
static const uint8_t BASE_314_S2[12] = {0x00, 0x04, 0x05, 0x08, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xE0, 0x41};

// Assemble the 24-byte 0x314 FD payload for frame index i.
static void build_frame_314(int i, uint8_t out[24]) {
  uint8_t s1[12], s2[12];

  // Subfield 1 (00 04 04)
  memcpy(s1, BASE_314_S1, 12);
  s1[5] = rolling_counter(0x40, i, 7);
  s1[6] = (uint8_t)rle_lookup(S1_314_VAL_RLE, i);
  s1[7] = (uint8_t)rle_lookup(S1_314_HI_RLE, i);
  //s1[8] = (uint8_t)rle_lookup(S1_314_FLAG_RLE, i);
  s1[8] = 0x7F;
  s1[9] = (uint8_t)rle_lookup(S1_314_STAT_RLE, i);
  s1[4] = payload_crc8(&s1[5]);

  // Subfield 2 (00 04 05)
  memcpy(s2, BASE_314_S2, 12);
  s2[5] = rolling_counter(0x70, i, 7);
  s2[6] = (uint8_t)shape_value(linear_shape_q16((uint32_t)i, (uint32_t)VAL_RAMP_START, (uint32_t)VAL_RAMP_END), 0,
                               S2_314_VAL_MAX);
  s2[9] = (uint8_t)rle_lookup(S2_314_CNT2_RLE, i);
  s2[4] = payload_crc8(&s2[5]);

  memcpy(out, s1, 12);
  memcpy(out + 12, s2, 12);
}

// ===========================================================================
// 0x315 (48-byte FD payload, four 12-byte subfields)
// ===========================================================================

// Subfield 2 (00 04 06): MODE payload byte (0x00/0x64/0x7D), 3 runs
static const RleRun S2_315_MODE_RLE[3] = {
    {0x00, 32},
    {0x64, 2},
    {0x7D, 46},
};

// Subfield 2 (00 04 06): byte 7 ramps to 0x1A, byte 9 ramps from 0x63 to
// 0x72. Both reuse the shared linear ramp timing of 0x314's VAL.
static const uint16_t S2_315_VAL_MAX = 0x1A;   // byte 7 final value
static const uint16_t S2_315_TEMP_DC = 0x63;   // byte 9 idle value
static const uint16_t S2_315_TEMP_MAX = 0x72;  // byte 9 final value

// Subfield 4 (00 04 18): STAT payload byte (0x10/0x30/0x50), 3 runs
static const RleRun S4_315_STAT_RLE[3] = {
    {0x10, 10},
    {0x30, 23},
    {0x50, 47},
};

// Subfield 4 (00 04 18): VAL16 is the 0x313 subfield 3 precharge curve,
// sampled one 100 ms frame earlier (the 0x315 capture leads by one sample),
// and like 0x313 it tracks the live pack voltage (5 x voltage_dV at full
// precharge).
static const uint16_t S4_315_VAL16_DC = 562;   // 12.5 x 45, idle
static const uint32_t S4_315_VAL16_SCALE = 5;  // frame value = 5 x voltage_dV
static const uint16_t S4_315_VAL16_FIELD_MAX = 0xFFFF;
static const int S4_315_VAL16_SKIP = 1;  // 0x315 leads 0x313 by one sample

// Subfield 4 (00 04 18): FLAG payload byte (0x00/0x02), 2 runs
static const RleRun S4_315_FLAG_RLE[2] = {
    {0x00, 1},
    {0x02, 79},
};

static const uint8_t BASE_315_S1[12] = {0x00, 0x04, 0x09, 0x08, 0x00, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t BASE_315_S2[12] = {0x00, 0x04, 0x06, 0x08, 0x00, 0x00, 0x00, 0x00, 0xB9, 0x00, 0x4E, 0x00};
static const uint8_t BASE_315_S3[12] = {0x00, 0x04, 0x17, 0x08, 0x00, 0x00, 0x41, 0x4C, 0x3F, 0xFF, 0xFF, 0xFF};
static const uint8_t BASE_315_S4[12] = {0x00, 0x04, 0x18, 0x08, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00};

// Assemble the 48-byte 0x315 FD payload for frame index i. The VAL16 plateau
// tracks the live pack voltage (5 x voltage_dV), like 0x313.
static void build_frame_315(int i, uint16_t voltage_dV, uint8_t out[48]) {
  uint8_t s1[12], s2[12], s3[12], s4[12];

  // Subfield 1 (00 04 09): VALB and VALA reuse the 0x313 shapes; the CRC
  // slot stays 0x00, as captured.
  memcpy(s1, BASE_315_S1, 12);
  s1[10] = (uint8_t)rle_lookup(VALB_RLE, i);
  s1[11] = (uint8_t)shape_value(linear_shape_q16((uint32_t)i, (uint32_t)VALA_RAMP_START, (uint32_t)VALA_RAMP_END), 0,
                                VALA_MAX);

  // Subfield 2 (00 04 06): bytes 7 and 9 reuse the 0x314 VAL ramp timing; the
  // CRC slot stays 0x00, as captured.
  uint32_t ramp = linear_shape_q16((uint32_t)i, (uint32_t)VAL_RAMP_START, (uint32_t)VAL_RAMP_END);
  memcpy(s2, BASE_315_S2, 12);
  s2[6] = (uint8_t)rle_lookup(S2_315_MODE_RLE, i);
  s2[7] = (uint8_t)shape_value(ramp, 0, S2_315_VAL_MAX);
  s2[9] = (uint8_t)shape_value(ramp, S2_315_TEMP_DC, S2_315_TEMP_MAX);

  // Subfield 3 (00 04 17)
  memcpy(s3, BASE_315_S3, 12);
  s3[5] = rolling_counter(0xF0, i, 7);
  s3[4] = payload_crc8(&s3[5]);

  // Subfield 4 (00 04 18): VAL16 tracks the live pack voltage like 0x313,
  // sampled one frame earlier; the CRC slot stays 0x00, as captured.
  uint32_t target16 = (uint32_t)voltage_dV * S4_315_VAL16_SCALE;
  if (target16 < S4_315_VAL16_DC) {
    target16 = S4_315_VAL16_DC;
  } else if (target16 > S4_315_VAL16_FIELD_MAX) {
    target16 = S4_315_VAL16_FIELD_MAX;
  }
  uint32_t t16 = (i > S4_315_VAL16_SKIP) ? (uint32_t)(i - S4_315_VAL16_SKIP) * 10u : 0u;
  uint16_t val16 = shape_value(precharge_shape_q16(t16), S4_315_VAL16_DC, (uint16_t)target16);
  memcpy(s4, BASE_315_S4, 12);
  s4[5] = (uint8_t)rle_lookup(S4_315_STAT_RLE, i);
  s4[6] = (uint8_t)(val16 >> 8);    // VAL16 high byte
  s4[7] = (uint8_t)(val16 & 0xFF);  // VAL16 low byte
  s4[9] = (uint8_t)rle_lookup(S4_315_FLAG_RLE, i);

  memcpy(out, s1, 12);
  memcpy(out + 12, s2, 12);
  memcpy(out + 24, s3, 12);
  memcpy(out + 36, s4, 12);
}

// --- Contactor state machine constants -------------------------------------
// Geometry of the generated message cycle (frames; see the FD frame
// generators above):
// 0x047/0x08A are generated at 10ms, 0x313/0x314/0x315 at 100ms, all driven
// from one master cursor (replayFrameIndex047_08A).
//
// The cycle opens with an idle period whose 0x08A "open request" bit holds the
// pack's contactors open, so:
//   - looping the first OPEN_LOOP_LEN frames of it keeps the contactors open,
//   - replaying it from index 0 closes them (precharge ramp included), and
//   - once the pack confirms closed (0x15B state == 7) the cycle restarts from
//     the already-closed tail instead, which would otherwise open and reclose
//     the contactors every time the loop wrapped.
static constexpr int OPEN_LOOP_LEN_047_08A = 150;                  // first 1.5s of the cycle
static constexpr int CLOSED_TAIL_START_047_08A = 304;              // already-closed tail of the cycle
static constexpr unsigned long CONTACTOR_STARTUP_GRACE_MS = 5000;  // max wait for the first 0x15B state
// 0x313/0x314/0x315 run at 1/10th the 047/08A rate; their closed-tail start
// is CLOSED_TAIL_START_047_08A / 10 = 30, and their cycle position is derived
// from the master cursor at transmit time.

void Mg4Battery::contactor_state_tick(unsigned long currentMillis) {
  const bool open_requested = (datalayer.system.status.system_status == FAULT);

  switch (contactorState) {
    case ContactorState::WAITING_FOR_PACK:
      // We don't know the current pack state yet.

      if (open_requested) {
        // If an open is requested, we should proceed with that immediately.
        logging.printf("[MG4] Contactor open requested, looping open segment of the message cycle\n");
        replayFrameIndex047_08A = 0;
        contactorState = ContactorState::OPENING;
      } else if (pack_contactors.received || currentMillis - contactorWaitStartMillis >= CONTACTOR_STARTUP_GRACE_MS) {
        // We now know the pack state, or have given up waiting for it.

        contactorWaitStartMillis = 0;
        if (pack_contactors.isClosed()) {
          // Pack contactors were already closed (eg, we rebooted without opening them).
          // Keep them closed.
          logging.printf("[MG4] Pack contactors already closed, resuming at closed tail\n");
          replayFrameIndex047_08A = CLOSED_TAIL_START_047_08A;
          contactorState = ContactorState::CLOSED;
        } else {
          // Pack contactors are open, start the closing sequence from the beginning.
          logging.printf("[MG4] Pack contactors open (state %d), starting closing sequence from the beginning\n",
                         pack_contactors.received ? (int)pack_contactors.state : -1);
          replayFrameIndex047_08A = 0;
          contactorState = ContactorState::CLOSING;
        }
      } else if (contactorWaitStartMillis == 0) {
        // Start the grace period timer
        contactorWaitStartMillis = currentMillis;
      }
      break;

    case ContactorState::CLOSING:
      // We're replaying the contactor-close sequence.

      if (open_requested) {
        // Open was requested, abort!
        logging.printf("[MG4] Contactor open requested, looping open segment of the message cycle\n");
        replayFrameIndex047_08A = 0;
        contactorState = ContactorState::OPENING;
      } else if (pack_contactors.isClosed()) {
        // The sequence has worked, the pack has closed.
        // We'll now stay in the closed state.
        contactorState = ContactorState::CLOSED;
      }
      break;

    case ContactorState::CLOSED:
      // The contactors are (presumably) currently closed.

      if (open_requested) {
        // Open requested, do that immediately.
        logging.printf("[MG4] Contactor open requested, looping open segment of the message cycle\n");
        replayFrameIndex047_08A = 0;
        contactorState = ContactorState::OPENING;
      } else if (pack_contactors.received && !pack_contactors.isClosed()) {
        // The contactors opened by themselves. Try to reclose them by
        // restarting the closing sequence.
        logging.printf("[MG4] Pack contactors no longer closed (state %d), replaying closing sequence\n",
                       (int)pack_contactors.state);
        replayFrameIndex047_08A = 0;
        contactorState = ContactorState::CLOSING;
      }
      break;

    case ContactorState::OPENING:
      // We're waiting for contactors to open.

      if (!open_requested) {
        // Close was requested during opening. Go to the waiting state until
        // we've figured out what the pack is doing (we don't know how far the
        // opening got).

        logging.printf("[MG4] Closing re-enabled, waiting for pack contactor state\n");
        contactorWaitStartMillis = 0;
        contactorState = ContactorState::WAITING_FOR_PACK;
      }
      break;
  }
}

void Mg4Battery::transmit_can(unsigned long currentMillis) {
  if (datalayer.system.status.bms_reset_status != BMS_RESET_IDLE) {
    // Transmitting towards battery is halted while BMS is being reset
    previousMillis10 = currentMillis;
    previousMillis200 = currentMillis;
    return;
  }

  if (currentMillis - previousMillis10 >= INTERVAL_10_MS) {
    previousMillis10 = currentMillis;

    contactor_state_tick(currentMillis);

    if (contactorState != ContactorState::WAITING_FOR_PACK) {
      build_frame_047(replayFrameIndex047_08A, datalayer.battery.status.voltage_dV, MG4_047_FD.data.u8);
      build_frame_08a(replayFrameIndex047_08A, MG4_08A_FD.data.u8);
      transmit_can_frame(&MG4_047_FD);
      transmit_can_frame(&MG4_08A_FD);

      // Calculate the start/end indices for the replay
      int wrap_start = (contactorState == ContactorState::CLOSED) ? CLOSED_TAIL_START_047_08A : 0;
      int wrap_limit = (contactorState == ContactorState::OPENING) ? OPEN_LOOP_LEN_047_08A : CYCLE_LEN_047;
      // Wrap if necessary
      if (++replayFrameIndex047_08A >= wrap_limit) {
        replayFrameIndex047_08A = wrap_start;
      }
    }

    if (currentMillis - previousMillis100 >= INTERVAL_100_MS) {
      previousMillis100 = currentMillis;

      // if (contactorState != ContactorState::WAITING_FOR_PACK) {
      //   int replayFrameIndex313_314_315 = replayFrameIndex047_08A / 10;
      //   build_frame_313(replayFrameIndex313_314_315, datalayer.battery.status.voltage_dV, MG4_313_FD.data.u8);
      //   build_frame_314(replayFrameIndex313_314_315, MG4_314_FD.data.u8);
      //   build_frame_315(replayFrameIndex313_314_315, datalayer.battery.status.voltage_dV, MG4_315_FD.data.u8);
      //   transmit_can_frame(&MG4_313_FD);
      //   transmit_can_frame(&MG4_314_FD);
      //   transmit_can_frame(&MG4_315_FD);
      // }
    }

    // 0x4F3 (FD) wakeup keep-alive, every 100ms. This was the only live part
    // of the old non-FD 047/sendPhase PTEXT cycle - closing works over FD
    // alone, so the non-FD frames are gone.
    if (++wakeupCounter >= 10) {
      wakeupCounter = 0;
      transmit_can_frame(&MG4_4F3_FD);
    }
  }

  transmit_uds_can(currentMillis);
}

uint16_t Mg4Battery::handle_pid(uint16_t pid, uint32_t value, const uint8_t* data, uint16_t length) {
  // Currently unused

  switch (pid) {
    case POLL_BATTERY_SOH:
      datalayer.battery.status.soh_pptt = value;
      break;
    case POLL_BATTERY_VOLTAGE:
      //datalayer.battery.status.voltage_dV = (value * 5) / 2;
      break;
    case POLL_BATTERY_CURRENT:
      //datalayer.battery.status.current_dA = (value - 40000) / -4;
      break;
    case POLL_BATTERY_SOC:
      // Only use SoC from PIDs if we don't get it from 401 messages.
      if (!reportsSoC) {
        //update_soc(value * 10);
      }
      break;
    case POLL_MIN_CELL_TEMPERATURE:
      datalayer.battery.status.temperature_min_dC = ((int32_t)value - 20000) / 50;
      temp_freshness = 10;
      break;
    case POLL_MAX_CELL_TEMPERATURE:
      datalayer.battery.status.temperature_max_dC = ((int32_t)value - 20000) / 50;
      temp_freshness = 10;
      break;  // End of cycle
  }
  return 0;  // Continue normal PID cycling
}

void Mg4Battery::setup(void) {  // Performs one time setup at startup
  setup_uds(0x7E5, 0);
  fd_uds_requests = true;

  static const uint16_t POLL_LIST[] = {POLL_BATTERY_SOH, POLL_BATTERY_VOLTAGE, POLL_MIN_CELL_TEMPERATURE,
                                       POLL_MAX_CELL_TEMPERATURE};

  set_pid_scan_list(POLL_LIST, sizeof(POLL_LIST) / sizeof(POLL_LIST[0]));
  dtc = &datalayer.battery.dtc;

  strncpy(datalayer.system.info.battery_protocol, Name, 63);
  datalayer.system.info.battery_protocol[63] = '\0';
  datalayer.system.status.battery_allows_contactor_closing = true;

  datalayer.battery.info.chemistry = user_selected_battery_chemistry;
  datalayer.battery.info.number_of_cells = 104;
  datalayer.battery.info.max_cell_voltage_deviation_mV = MAX_CELL_DEVIATION_MV;

  // Danger limits
  if (datalayer.battery.info.chemistry == battery_chemistry_enum::LFP) {
    datalayer.battery.info.max_cell_voltage_mV = 3760;
    datalayer.battery.info.min_cell_voltage_mV = 2500;
  } else {
    datalayer.battery.info.max_cell_voltage_mV = 4250;
    datalayer.battery.info.min_cell_voltage_mV = 2700;
  }

  working_cell_max_mV = datalayer.battery.info.max_cell_voltage_mV - 10;
  working_cell_min_mV = datalayer.battery.info.min_cell_voltage_mV + 300;
  working_cell_recharge_threshold_mV = working_cell_max_mV - 100;
  coulombCounting = user_selected_use_estimated_SOC;
  if (coulombCounting) {
    static const uint32_t MINIMUM_WORKING_RANGE_MV = 200;
    if (user_selected_max_cell_voltage_mV > (datalayer.battery.info.min_cell_voltage_mV + MINIMUM_WORKING_RANGE_MV) &&
        user_selected_max_cell_voltage_mV <= datalayer.battery.info.max_cell_voltage_mV) {
      working_cell_max_mV = user_selected_max_cell_voltage_mV;
      // Calculate threshold as 1% lower SoC than max, using ocv_to_soc
      working_cell_recharge_threshold_mV = soc_to_ocv(ocv_to_soc(working_cell_max_mV) - 100);
    } else {
      logging.printf("[MG4] Invalid user-selected max cell voltage, using default of %d mV\n", working_cell_max_mV);
    }
    if (user_selected_min_cell_voltage_mV >= datalayer.battery.info.min_cell_voltage_mV &&
        user_selected_min_cell_voltage_mV <= (working_cell_max_mV - MINIMUM_WORKING_RANGE_MV)) {
      working_cell_min_mV = user_selected_min_cell_voltage_mV;
    } else {
      logging.printf("[MG4] Invalid user-selected min cell voltage, using default of %d mV\n", working_cell_min_mV);
    }
    logging.printf("[MG4] Working cell voltage range: %d mV - %d mV, recharge threshold: %d mV\n", working_cell_min_mV,
                   working_cell_max_mV, working_cell_recharge_threshold_mV);
  }

  datalayer.battery.info.max_design_voltage_dV =
      (datalayer.battery.info.number_of_cells * datalayer.battery.info.max_cell_voltage_mV) / 100;
  datalayer.battery.info.min_design_voltage_dV =
      (datalayer.battery.info.number_of_cells * datalayer.battery.info.min_cell_voltage_mV) / 100;

  // Manually allocate addresses in the 512 bytes of ULP-reserved RTC slow
  // memory for storing the total discharge counter and a cookie to verify its
  // validity. This data survives OTA and software resets (but not hardware
  // resets).

  int nvram_base = (int)SOC_RTC_DATA_LOW + 400;
  if (this == battery2) {
    nvram_base += 16;
  } else if (this == battery3) {
    nvram_base += 32;
  }

  nonvolatile_cookie = (uint32_t*)(nvram_base);
  nonvolatile_total_discharge_dC = (uint32_t*)(nvram_base + 4);
  // logging.printf("Nonvolatile cookie value is %lu\n", *nonvolatile_cookie);
  // // set to a random value
  // *nonvolatile_cookie = esp_timer_get_time();
  // logging.printf("Nonvolatile cookie set to %lu\n", *nonvolatile_cookie);
}

String Mg4Battery::get_uds_info_html() {
  // Pack-reported precharge/contactor state (0x15B byte[21]&0xF)
  String html = "<h3>Precharge/contactor state</h3>";
  html += "<div style='border: 1px solid #ccc; padding: 5px;'>";
  html += "<span style='display: inline-block; width: 14px; height: 14px; background-color: " +
          String(pack_contactors.color()) + "; margin-right: 6px;'></span>";
  html += "State: " + String(pack_contactors.received ? String(pack_contactors.state) : String("n/a")) + " (" +
          pack_contactors.label() + ")";
  html += "</div>";

  return html;
}
