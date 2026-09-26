#include "MG-4-BATTERY.h"
#include <soc/soc.h>
#include <cmath>    //For unit test
#include <cstdio>   //For sprintf
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

// Run-length encoded field: `value` is held for `count` consecutive frames.
struct Mg4RleRun {
  uint16_t value;
  uint16_t count;
};

static uint8_t mg4_crc8(const uint8_t* d) {
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < 7; i++) {
    crc = crc8_table_SAE_J1850_ZER0[(crc ^ static_cast<uint8_t>(d[i])) % 256];
  }
  return crc;
}

template <size_t N>
static uint8_t mg4_rle_lookup(const Mg4RleRun (&runs)[N], int i) {
  for (size_t r = 0; r < N; r++) {
    if (i < (int)runs[r].count) {
      return (uint8_t)runs[r].value;
    }
    i -= runs[r].count;
  }
  return 0;  // index past end of segment - should not happen
}

// 15-value rolling counter used in various frames
static uint8_t mg4_cnt(uint8_t base, int i, int skew) {
  return (uint8_t)(base + ((i + skew) % 15));
}

// Linear ramp 0..max, rounded to nearest. Used for simulating ramps in the FD
// frames.
static uint16_t mg4_ramp(int t, int start, int end, uint16_t max) {
  if (t <= start) {
    return 0;
  }
  if (t >= end) {
    return max;
  }
  return (uint16_t)(((t - start) * max + (end - start) / 2) / (end - start));
}

// Number of frames in the 0x047/0x08A contactor-close replay.
static const int MG4_CYCLE_LEN_047_08A = 800;

// Index of precharge edge (from low to high).
static const int MG4_PRECHARGE_STEP = 252;  // first high frame

// This is likely the contactor close request itself
static const Mg4RleRun RLE_08A_REQUEST[3] = {
    {0x00, 190},
    {0x01, 119},
    {0x21, 491},
};

// A flag that goes high just before precharge, for some reason.
static const Mg4RleRun RLE_08A_FLAG[3] = {
    {0x00, 188},
    {0x08, 219},
    {0x00, 393},
};

static const Mg4RleRun RLE_08A_LEVEL[3] = {
    {0x00, 307},
    {0x20, 30},
    {0x40, 463},
};

static const Mg4RleRun RLE_313_STAT[3] = {
    {0x01, 10},
    {0x03, 23},
    {0x05, 47},
};

static const Mg4RleRun RLE_314_VAL[5] = {
    {0x02, 27}, {0x2E, 1}, {0x4C, 1}, {0x56, 35}, {0x57, 16},
};

static const Mg4RleRun RLE_314_HI[7] = {
    {0xC8, 27}, {0x08, 1}, {0xC8, 1}, {0x48, 2}, {0x88, 1}, {0xC8, 32}, {0x08, 16},
};

// Linear taper from output_min to output_max over the input range.
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
  // Fail-closed: no fresh cell voltages or temperatures means we cannot
  // prove the pack is safe, so allow no power. This also covers the boot
  // window before the first 0x12C/0x159 frame.
  if (cell_voltage_freshness <= 0 || temp_freshness <= 0) {
    return 0;
  }

  int32_t max_discharge_power_W = MAX_DISCHARGE_POWER_W;

  // Cellvoltage-based power derating. Taper linearly to zero over the last
  // DISCHARGE_TAPER_MV above working_cell_min_mV, then latch at zero (with
  // hysteresis) until the cell voltage recovers past the trip threshold.
  {
    // Freshness already gated above.
    const int32_t cell_power_W = battery_discharge_power_by_cell_min(
        datalayer.battery.status.cell_min_voltage_mV, working_cell_min_mV, DISCHARGE_TAPER_MV, DISCHARGE_HYSTERESIS_MV,
        MAX_DISCHARGE_POWER_W, &voltageAtCellMin);
    if (cell_power_W < max_discharge_power_W) {
      max_discharge_power_W = cell_power_W;
    }
  }

  // Temperature-based power derating: high temperature limits both charge and
  // discharge. Freshness already gated above, so a default/uninitialized
  // value of 0 dC can no longer slip through as an at-limit reading.
  {
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

  if (!batteryIdentified) {
    max_discharge_power_W = 0;
  }

  return max_discharge_power_W > 0 ? max_discharge_power_W : 0;
}

uint32_t Mg4Battery::calculate_max_charge_power_W() {
  // Fail-closed: see calculate_max_discharge_power_W().
  if (cell_voltage_freshness <= 0 || temp_freshness <= 0) {
    return 0;
  }

  int32_t max_charge_power_W = MAX_CHARGE_POWER_W;

  // Cellvoltage-based power derating. Taper linearly to zero over the last
  // CHARGE_TAPER_MV below working_cell_max_mV, then latch at zero (with
  // hysteresis) until the cell voltage recovers past the trip threshold.
  {
    // Freshness already gated above.
    const int32_t cell_power_W =
        battery_charge_power_by_cell_max(datalayer.battery.status.cell_max_voltage_mV, working_cell_max_mV,
                                         CHARGE_TAPER_MV, CHARGE_HYSTERESIS_MV, MAX_CHARGE_POWER_W, &voltageAtCellMax);
    if (cell_power_W < max_charge_power_W) {
      max_charge_power_W = cell_power_W;
    }
  }

  // Temperature-based power derating: high temperature limits both charge and
  // discharge, low temperature limits charge only. Freshness already gated
  // above, so a default/uninitialized value of 0 dC can no longer slip
  // through as an at-limit reading.
  {
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

  if (!batteryIdentified) {
    max_charge_power_W = 0;
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
    case 0x308:
      // Pack serial (NTSC identifier): subfield 000554 has no CRC slot,
      // it carries 7 ASCII bytes + 1 index byte per frame over 4 frames.
      for (int i = 0; i + 12 <= rx_frame.DLC; i += 12) {
        if (rx_frame.data.u8[i + 3] != 8) {
          break;
        }
        uint32_t addr = (rx_frame.data.u8[i] << 16) | (rx_frame.data.u8[i + 1] << 8) | rx_frame.data.u8[i + 2];
        if (addr == 0x554) {
          const uint8_t* sub = &rx_frame.data.u8[i + 4];
          if (sub[7] < 4) {
            for (int j = 0; j < 7; j++) {
              uint8_t c = sub[j];
              ntsc_serial[sub[7] * 7 + j] = (c == 0xFF) ? 0 : (char)c;
            }
            ntsc_serial[28] = 0;
          }
        }
      }
      break;
    default:
      break;
  }
}

// In contactors-open state, we loop the first 150 frames (1.5s) of the replay.
static constexpr int OPEN_LOOP_LEN_047_08A = 150;
// In contactors-closed state, we start replay from the already-closed tail.
static constexpr int CLOSED_TAIL_START_047_08A = 304;

// How long to wait for the contactor status on boot, before we start sending.
// Allows us to keep contactors closed during BE reboots.
static constexpr unsigned long CONTACTOR_STARTUP_GRACE_MS = 5000;  // max wait for the first 0x15B state

// Patch the 047 and 08A frames in place, for the given replay index.
void Mg4Battery::update_047_08a(int i) {
  uint16_t voltage_dV = datalayer.battery.status.voltage_dV;

  // 0x047 VAL12 plateau tracks the live pack voltage (0.4 x voltage_dV).
  uint32_t target = ((uint32_t)voltage_dV * 2u) / 5u;
  if (target < 45) {
    target = 45;
  } else if (target > 0xFFF) {
    target = 0xFFF;
  }
  uint16_t val12 = (i < MG4_PRECHARGE_STEP) ? 45 : (uint16_t)target;

  uint8_t* f47 = MG4_047_FD.data.u8;
  uint8_t cnt = mg4_cnt(0xF0, i, 11);
  f47[5] = cnt;
  f47[9] = (uint8_t)(val12 >> 4);                    // VAL12 high 8 bits
  f47[10] = (uint8_t)(((val12 & 0xF) << 4) | 0x0C);  // VAL12 low 4 bits + static 0xC nibble
  f47[4] = mg4_crc8(&f47[5]);
  f47[17] = cnt;
  f47[16] = mg4_crc8(&f47[17]);

  uint8_t* f8a = MG4_08A_FD.data.u8;
  f8a[5] = mg4_cnt(0x30, i, 11);
  f8a[4] = mg4_crc8(&f8a[5]);
  f8a[17] = mg4_cnt(0x40, i, 11);
  f8a[19] = mg4_rle_lookup(RLE_08A_REQUEST, i);
  f8a[20] = mg4_rle_lookup(RLE_08A_FLAG, i);
  f8a[16] = mg4_crc8(&f8a[17]);
  f8a[30] = mg4_rle_lookup(RLE_08A_LEVEL, i);
  // Last subfield has no CRC
}

// Patch the 313 and 314 frames in place, for the given replay index.
// This index advances at 1/10 the rate of the 047/08A frames.
void Mg4Battery::update_313_314(int i) {
  uint16_t voltage_dV = datalayer.battery.status.voltage_dV;
  uint8_t* f13 = MG4_313_FD.data.u8;

  f13[5] = mg4_cnt(0xF0, i, 7);
  f13[10] = mg4_rle_lookup(RLE_313_STAT, i);
  f13[4] = mg4_crc8(&f13[5]);

  // VALA and VALC share one ramp shape but different magnitudes.
  uint16_t valc = mg4_ramp(i, 32, 78, 90);
  f13[18] = (uint8_t)mg4_ramp(i, 32, 78, 77);
  f13[20] = (uint8_t)(0x80 | (valc >> 4));          // static hi nibble 8 + VALC hi nibble
  f13[21] = (uint8_t)(((valc & 0xF) << 4) | 0x08);  // VALC lo nibble + static lo nibble 8

  // VAL16 plateau tracks the live pack voltage (5 x voltage_dV, 12.5x the
  // 0x047 VAL12), sampled every 10th 0x047 frame.
  uint32_t target16 = (uint32_t)voltage_dV * 5u;
  if (target16 < 562) {
    target16 = 562;
  } else if (target16 > 0xFFFF) {
    target16 = 0xFFFF;
  }
  uint32_t t16 = (i > 2) ? (uint32_t)(i - 2) * 10u : 0u;
  uint16_t val16 = (t16 < (uint32_t)MG4_PRECHARGE_STEP) ? 562 : (uint16_t)target16;
  f13[29] = mg4_cnt(0x30, i, 7);
  f13[32] = (uint8_t)(val16 >> 8);
  f13[33] = (uint8_t)val16;
  f13[28] = mg4_crc8(&f13[29]);

  uint8_t* f14 = MG4_314_FD.data.u8;
  f14[5] = mg4_cnt(0x40, i, 7);
  f14[6] = mg4_rle_lookup(RLE_314_VAL, i);
  f14[7] = mg4_rle_lookup(RLE_314_HI, i);
  f14[4] = mg4_crc8(&f14[5]);
  f14[17] = mg4_cnt(0x70, i, 7);
  f14[18] = (uint8_t)mg4_ramp(i, 44, 59, 48);
  f14[16] = mg4_crc8(&f14[17]);
}

void Mg4Battery::reset_reclose_tracker() {
  reclose_count = 0;
  reclose_pos = 0;
  if (reclose_blocked) {
    reclose_blocked = false;
    clear_event(EVENT_CONTACTOR_RECLOSE_FAULT, battery_index);
    logging.printf("[MG4] Reclose fault cleared by manual open/close, retry allowed\n");
  }
}

// Have the contactors cycled too many times within the reclose window?
bool Mg4Battery::record_contactor_reclose(unsigned long now_ms) {
  reclose_times[reclose_pos] = now_ms;
  reclose_pos = (reclose_pos + 1) % RECLOSE_TRIP_COUNT;
  if (reclose_count < RECLOSE_TRIP_COUNT) {
    reclose_count++;
  }
  if (reclose_count < RECLOSE_TRIP_COUNT) {
    return false;
  }
  // Return true if the most recent reclose falls within the reclose window (ie,
  // happened too quickly)
  return (now_ms - reclose_times[reclose_pos]) <= RECLOSE_WINDOW_MS;
}

// Tick function for the contactor state machine.
void Mg4Battery::contactor_state_tick(unsigned long currentMillis) {
  const bool open_requested = (datalayer.system.status.system_status == FAULT);

  // An explicit manual equipment stop resets the reclose tracker.
  const bool equipment_stop = datalayer.system.info.equipment_stop_active;
  if (equipment_stop != last_equipment_stop) {
    last_equipment_stop = equipment_stop;
    reset_reclose_tracker();
  }

  // Startup grace for riding through an already-closed pack while the battery
  // is not yet identified. If it expires, the battery will be commanded
  // open/closed regardless of its present state.
  const bool startup_grace_expired = (contactorWaitStartMillis != 0)
                                         ? (currentMillis - contactorWaitStartMillis >= CONTACTOR_STARTUP_GRACE_MS)
                                         : (currentMillis >= CONTACTOR_STARTUP_GRACE_MS);

  switch (contactorState) {
    case ContactorState::WAITING_FOR_PACK:
      // We don't know the current pack state yet.

      if (open_requested) {
        // If an open is requested, we should proceed with that immediately.
        logging.printf("[MG4] Contactor open requested, looping open segment of the message cycle\n");
        reset_reclose_tracker();
        replayFrameIndex047_08A = 0;
        contactorState = ContactorState::OPENING;
      } else if (pack_contactors.received || currentMillis - contactorWaitStartMillis >= CONTACTOR_STARTUP_GRACE_MS) {
        // We now know the pack state, or have given up waiting for it.
        if (pack_contactors.isClosed()) {
          if (batteryIdentified) {
            // Pack contactors were already closed (eg, we rebooted without opening them).
            // Keep them closed.
            contactorWaitStartMillis = 0;
            logging.printf("[MG4] Pack contactors already closed, resuming at closed tail\n");
            clear_event(EVENT_CONTACTOR_OPEN, battery_index);
            replayFrameIndex047_08A = CLOSED_TAIL_START_047_08A;
            contactorState = ContactorState::CLOSED;
          } else if (!startup_grace_expired) {
            // We haven't yet identified the battery, but the contactors were
            // already closed at boot, so keep them closed until the grace
            // period expires.
            if (contactorWaitStartMillis == 0) {
              // Start the wait timer for the grace period if needed.
              contactorWaitStartMillis = currentMillis;
            }
            logging.printf("[MG4] Pack contactors already closed, unidentified pack riding through\n");
            clear_event(EVENT_CONTACTOR_OPEN, battery_index);
            replayFrameIndex047_08A = CLOSED_TAIL_START_047_08A;
            contactorState = ContactorState::CLOSED;
          } else {
            // Grace period expired and we still don't know the battery
            // identity. Open contactors.
            logging.printf("[MG4] Pack closed but unidentified past grace, opening\n");
            replayFrameIndex047_08A = 0;
            contactorState = ContactorState::OPENING;
          }
        } else {
          if (batteryIdentified) {
            // Pack contactors are open, start the closing sequence from the beginning.
            contactorWaitStartMillis = 0;
            logging.printf("[MG4] Pack contactors open (state %d), starting closing sequence from the beginning\n",
                           pack_contactors.received ? (int)pack_contactors.state : -1);
            replayFrameIndex047_08A = 0;
            contactorState = ContactorState::CLOSING;
          } else if (!pack_contactors.received && startup_grace_expired) {
            // We never identified the pack, and the grace period has expired,
            // force contactors open.
            logging.printf("[MG4] Pack state unknown and unidentified past grace, opening\n");
            replayFrameIndex047_08A = 0;
            contactorState = ContactorState::OPENING;
          } else {
            // Stay in this state until we identify the pack.
            if (contactorWaitStartMillis == 0) {
              // Start the grace period timer if necessary.
              contactorWaitStartMillis = currentMillis;
            }
          }
        }
      } else if (contactorWaitStartMillis == 0) {
        // Start the grace period timer if necessary.
        contactorWaitStartMillis = currentMillis;
      }
      break;

    case ContactorState::CLOSING:
      // We're replaying the contactor-close sequence.

      if (open_requested) {
        // Open was requested, abort!
        logging.printf("[MG4] Contactor open requested, looping open segment of the message cycle\n");
        reset_reclose_tracker();
        replayFrameIndex047_08A = 0;
        contactorState = ContactorState::OPENING;
      } else if (!batteryIdentified) {
        // Must not close from open while unidentified.
        logging.printf("[MG4] Aborting close, pack unidentified, looping open segment\n");
        replayFrameIndex047_08A = 0;
        contactorState = ContactorState::OPENING;
      } else if (pack_contactors.isClosed()) {
        // The sequence has worked, the pack has closed. Any earlier surprise
        // open is resolved. We'll now stay in the closed state.
        clear_event(EVENT_CONTACTOR_OPEN, battery_index);
        contactorState = ContactorState::CLOSED;
      }
      break;

    case ContactorState::CLOSED:
      // The contactors are (presumably) currently closed.

      if (open_requested) {
        // Open requested, do that immediately.
        logging.printf("[MG4] Contactor open requested, looping open segment of the message cycle\n");
        reset_reclose_tracker();
        replayFrameIndex047_08A = 0;
        contactorState = ContactorState::OPENING;
      } else if (!batteryIdentified && startup_grace_expired) {
        // We were staying closed over a reboot, but didn't identify the pack in
        // time. Open contactors.
        logging.printf("[MG4] Unidentified pack past grace, opening\n");
        replayFrameIndex047_08A = 0;
        contactorState = ContactorState::OPENING;
      } else if (pack_contactors.received && !pack_contactors.isClosed()) {
        // Contactors opened unexpectedly.
        if (batteryIdentified) {
          set_event(EVENT_CONTACTOR_OPEN, 0, battery_index);
          if (record_contactor_reclose(currentMillis)) {
            // The pack keeps opening by itself (e.g. HV isolation fault):
            // stop wearing out the contactors, hold open and raise a fatal
            // event. Only a manual open/close clears this latch.
            logging.printf("[MG4] Pack opened %d times within %lu ms, latching contactors open\n", RECLOSE_TRIP_COUNT,
                           RECLOSE_WINDOW_MS);
            reclose_blocked = true;
            replayFrameIndex047_08A = 0;
            contactorState = ContactorState::OPENING;
            set_event(EVENT_CONTACTOR_RECLOSE_FAULT, RECLOSE_TRIP_COUNT, battery_index);
          } else {
            // Try to reclose them by restarting the closing sequence.
            logging.printf("[MG4] Pack contactors no longer closed (state %d), replaying closing sequence\n",
                           (int)pack_contactors.state);
            replayFrameIndex047_08A = 0;
            contactorState = ContactorState::CLOSING;
          }
        } else {
          // Must not reclose while unidentified.
          set_event(EVENT_CONTACTOR_OPEN, 0, battery_index);
          logging.printf("[MG4] Pack contactors opened while unidentified (state %d), opening\n",
                         (int)pack_contactors.state);
          replayFrameIndex047_08A = 0;
          contactorState = ContactorState::OPENING;
        }
      }
      break;

    case ContactorState::OPENING:
      // We play the initial segment of the contactor message cycle to open
      // contactors and keep them open.

      // If close was requested, only proceed if we've identified the pack and
      // reclose is not blocked.
      if (!open_requested && batteryIdentified && !reclose_blocked) {
        logging.printf("[MG4] Closing requested, waiting for pack contactor state\n");
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

    // We don't send any contactor messages until we've decided whether we're
    // opening or closing (to allow a closed pack to stay closed during reboot).
    if (contactorState != ContactorState::WAITING_FOR_PACK) {
      update_047_08a(replayFrameIndex047_08A);
      transmit_can_frame(&MG4_047_FD);
      transmit_can_frame(&MG4_08A_FD);

      // Calculate the start/end indices for the replay
      int wrap_start = (contactorState == ContactorState::CLOSED) ? CLOSED_TAIL_START_047_08A : 0;
      int wrap_limit = (contactorState == ContactorState::OPENING) ? OPEN_LOOP_LEN_047_08A : MG4_CYCLE_LEN_047_08A;
      // Wrap if necessary
      if (++replayFrameIndex047_08A >= wrap_limit) {
        replayFrameIndex047_08A = wrap_start;
      }
    }

    if (currentMillis - previousMillis100 >= INTERVAL_100_MS) {
      previousMillis100 = currentMillis;

      // 0x4F3 (FD) wakeup/keep-alive (unsure if this is needed).
      transmit_can_frame(&MG4_4F3_FD);

      if (contactorState != ContactorState::WAITING_FOR_PACK) {
        int replayFrameIndex313_314 = replayFrameIndex047_08A / 10;
        update_313_314(replayFrameIndex313_314);
        transmit_can_frame(&MG4_313_FD);
        transmit_can_frame(&MG4_314_FD);
      }
    }
  }

  transmit_uds_can(currentMillis);
}

uint16_t Mg4Battery::handle_pid(uint16_t pid, uint32_t value, const uint8_t* data, uint16_t length) {
  // Called by the UDS superclass for every successful PID response. `value` is
  // the big-endian PID value (up to 4 bytes), `data` points at the raw value
  // bytes (without the SID/DID header). Return 0 to continue the scan list.
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
      // SoC arrives on the FD 0x15B frame; the PID value is unused.
      break;
    case POLL_MIN_CELL_TEMPERATURE:
      datalayer.battery.status.temperature_min_dC = ((int32_t)value - 20000) / 50;
      temp_freshness = 10;
      break;
    case POLL_MAX_CELL_TEMPERATURE:
      datalayer.battery.status.temperature_max_dC = ((int32_t)value - 20000) / 50;
      temp_freshness = 10;
      break;
    case POLL_ECU_HARDWARE_NUMBER:
      memcpy(pid_ecu_hw_number, data, length > sizeof(pid_ecu_hw_number) ? sizeof(pid_ecu_hw_number) : length);
      if (!batteryIdentified)
        identify_battery();
      break;
    case POLL_ECU_SOFTWARE_NUMBER:
      memcpy(pid_ecu_sw_number, data, length > sizeof(pid_ecu_sw_number) ? sizeof(pid_ecu_sw_number) : length);
      break;  // End of cycle
  }
  return 0;  // Continue normal PID cycling
}

void Mg4Battery::identify_battery() {
  if (pid_ecu_hw_number[8] == 0 || pid_ecu_hw_number[9] == 0) {
    // No valid ECU hardware number received yet
    return;
  }

  if (ntsc_serial[4] == 0) {
    // No valid NTSC serial chemistry received yet
    return;
  }

  battery_chemistry_enum chemistry;
  if (ntsc_serial[4] == 'B') {
    chemistry = LFP;
  } else {
    chemistry = NMC;
  }

  uint8_t capacity = ((pid_ecu_hw_number[8] - '0') * 10) + (pid_ecu_hw_number[9] - '0');

  auto setup_battery = [&](battery_chemistry_enum chem, uint32_t cap, uint8_t num_cells) {
    datalayer.battery.info.chemistry = chem;
    datalayer.battery.info.number_of_cells = num_cells;
    datalayer.battery.info.total_capacity_Wh = cap;

    if (chem == LFP) {
      datalayer.battery.info.max_cell_voltage_mV = MAX_CELL_VOLTAGE_LFP_MV;
      datalayer.battery.info.min_cell_voltage_mV = MIN_CELL_VOLTAGE_LFP_MV;
    } else {
      datalayer.battery.info.max_cell_voltage_mV = MAX_CELL_VOLTAGE_NMC_MV;
      datalayer.battery.info.min_cell_voltage_mV = MIN_CELL_VOLTAGE_NMC_MV;
    }

    working_cell_max_mV = datalayer.battery.info.max_cell_voltage_mV - WORKING_MAX_MARGIN_MV;
    working_cell_min_mV = datalayer.battery.info.min_cell_voltage_mV + WORKING_MIN_MARGIN_MV;
    datalayer.battery.info.max_cell_voltage_deviation_mV =
        (chem == LFP) ? MAX_CELL_DEVIATION_LFP_MV : MAX_CELL_DEVIATION_NMC_MV;

    datalayer.battery.info.max_design_voltage_dV =
        (datalayer.battery.info.max_cell_voltage_mV * (uint32_t)datalayer.battery.info.number_of_cells) / 100;
    datalayer.battery.info.min_design_voltage_dV =
        (datalayer.battery.info.min_cell_voltage_mV * (uint32_t)datalayer.battery.info.number_of_cells) / 100;

    logging.printf("[MG4] Detected %ds %dWh %s battery\n", datalayer.battery.info.number_of_cells,
                   datalayer.battery.info.total_capacity_Wh, chemistry == LFP ? "LFP" : "NMC");

    batteryIdentified = true;
  };

  if (capacity == 49 && chemistry == LFP) {
    setup_battery(chemistry, 49000, 100);
  } else if (capacity == 51 && chemistry == LFP) {
    setup_battery(chemistry, 51000, 104);
  } else if (capacity == 64 && chemistry == NMC) {
    setup_battery(chemistry, 64000, 104);
  } else if (capacity == 77 && chemistry == NMC) {
    setup_battery(chemistry, 77000, 108);
  } else {
    logging.printf("[MG4] Unknown battery: %d %c\n", capacity, ntsc_serial[4]);
  }
}

void Mg4Battery::setup(void) {  // Performs one time setup at startup
  setup_uds(0x7E5, 0);
  fd_uds_requests = true;

  static const uint16_t POLL_LIST[] = {POLL_BATTERY_SOH,          POLL_BATTERY_VOLTAGE,     POLL_MIN_CELL_TEMPERATURE,
                                       POLL_MAX_CELL_TEMPERATURE, POLL_ECU_HARDWARE_NUMBER, POLL_ECU_SOFTWARE_NUMBER};

  set_pid_scan_list(POLL_LIST, sizeof(POLL_LIST) / sizeof(POLL_LIST[0]));
  dtc = &datalayer.battery.dtc;

  strncpy(datalayer.system.info.battery_protocol, Name, 63);
  datalayer.system.info.battery_protocol[63] = '\0';
  datalayer.system.status.battery_allows_contactor_closing = true;
  // Sync the manual open/close edge detector so a persisted equipment stop
  // is not mistaken for a fresh manual action on the first tick.
  last_equipment_stop = datalayer.system.info.equipment_stop_active;

  //
  datalayer.battery.info.chemistry = NMC;
  datalayer.battery.info.number_of_cells = 108;
  datalayer.battery.info.max_cell_voltage_deviation_mV = MAX_CELL_DEVIATION_LFP_MV;

  // Start with wide voltage limits until we identify pack
  datalayer.battery.info.max_cell_voltage_mV = MAX_CELL_VOLTAGE_NMC_MV;
  datalayer.battery.info.min_cell_voltage_mV = MIN_CELL_VOLTAGE_LFP_MV;
  working_cell_max_mV = datalayer.battery.info.max_cell_voltage_mV - WORKING_MAX_MARGIN_MV;
  working_cell_min_mV = datalayer.battery.info.min_cell_voltage_mV + WORKING_MIN_MARGIN_MV;
  // working_cell_recharge_threshold_mV = working_cell_max_mV - 100;
  // coulombCounting = user_selected_use_estimated_SOC;
  // if (coulombCounting) {
  //   static const uint32_t MINIMUM_WORKING_RANGE_MV = 200;
  //   if (user_selected_max_cell_voltage_mV > (datalayer.battery.info.min_cell_voltage_mV + MINIMUM_WORKING_RANGE_MV) &&
  //       user_selected_max_cell_voltage_mV <= datalayer.battery.info.max_cell_voltage_mV) {
  //     working_cell_max_mV = user_selected_max_cell_voltage_mV;
  //     // Calculate threshold as 1% lower SoC than max, using ocv_to_soc
  //     working_cell_recharge_threshold_mV = soc_to_ocv(ocv_to_soc(working_cell_max_mV) - 100);
  //   } else {
  //     logging.printf("[MG4] Invalid user-selected max cell voltage, using default of %d mV\n", working_cell_max_mV);
  //   }
  //   if (user_selected_min_cell_voltage_mV >= datalayer.battery.info.min_cell_voltage_mV &&
  //       user_selected_min_cell_voltage_mV <= (working_cell_max_mV - MINIMUM_WORKING_RANGE_MV)) {
  //     working_cell_min_mV = user_selected_min_cell_voltage_mV;
  //   } else {
  //     logging.printf("[MG4] Invalid user-selected min cell voltage, using default of %d mV\n", working_cell_min_mV);
  //   }
  //   logging.printf("[MG4] Working cell voltage range: %d mV - %d mV, recharge threshold: %d mV\n", working_cell_min_mV,
  //                  working_cell_max_mV, working_cell_recharge_threshold_mV);
  // }

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

// Renders characters if printable, otherwise as [xx] hex.
static void print_chars_or_hex(char* buf, const uint8_t* data, uint16_t length) {
  int ptr = 0;
  for (int i = 0; i < length && ptr < 62; i++) {
    if (data[i] >= 32 && data[i] <= 126) {
      buf[ptr++] = (char)data[i];
    } else {
      int written = sprintf(buf + ptr, "[%02x]", data[i]);
      ptr += written;
    }
  }
  buf[ptr] = '\0';
}

String Mg4Battery::get_uds_info_html() {
  // Pack-reported precharge/contactor state (0x15B byte[21]&0xF)
  String html = "<h3>Precharge/contactor state</h3>";
  html += "<div style='border: 1px solid #ccc; padding: 5px;'>";
  html += "<span style='display: inline-block; width: 14px; height: 14px; background-color: " +
          String(pack_contactors.color()) + "; margin-right: 6px;'></span>";
  html += "State: " + String(pack_contactors.received ? String(pack_contactors.state) : String("n/a")) + " (" +
          pack_contactors.label() + ")";
  html += "<br>Pack serial: " + String(ntsc_serial);
  char buf[64];
  print_chars_or_hex(buf, pid_ecu_hw_number, sizeof(pid_ecu_hw_number));
  html += "<br>ECU hardware: ";
  html += buf;
  print_chars_or_hex(buf, pid_ecu_sw_number, sizeof(pid_ecu_sw_number));
  html += "<br>ECU software: ";
  html += buf;
  if (reclose_blocked) {
    html += "<br><span style='color: #f44336;'>Reclose fault latched: pack opened " + String(RECLOSE_TRIP_COUNT) +
            " times within " + String((unsigned long)(RECLOSE_WINDOW_MS / 1000)) +
            " s. Toggle manual open/close after fixing the fault to retry.</span>";
  }
  html += "</div>";

  return html;
}
