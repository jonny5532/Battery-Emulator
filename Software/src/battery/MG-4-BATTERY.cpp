#include "MG-4-BATTERY.h"
#include <cmath>    //For unit test
#include <cstdio>   //For sprintf
#include <cstdlib>  //For realloc
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

static uint8_t mg4_crc8(const uint8_t* d) {
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < 7; i++) {
    crc = crc8_table_SAE_J1850_ZER0[(crc ^ static_cast<uint8_t>(d[i])) % 256];
  }
  return crc;
}

// Linear ramp 0..max, rounded to nearest. Used for simulating ramps in the FD
// frames. Saturates at max past `end` (no looping), which is what lets the
// free-running sequencer hold its final levels instead of replaying a loop.
static uint16_t mg4_ramp(int t, int start, int end, uint16_t max) {
  if (t <= start) {
    return 0;
  }
  if (t >= end) {
    return max;
  }
  return (uint16_t)(((t - start) * max + (end - start) / 2) / (end - start));
}

// Per-signal sequencers. Each threshold below is that signal's own timing in
// sequence ticks (10ms ticks for fast signals, 100ms ticks for slow ones),
// preserving the legacy 800-frame capture timing exactly. Unlike the old
// run-length tables they saturate at the final level past the end.
uint8_t Mg4Battery::seq_08a_request(uint32_t t) {
  if (t < SEQ_08A_REQ_T1) {
    return 0x00;
  }
  if (t < SEQ_08A_REQ_T2) {
    return 0x01;
  }
  return 0x21;
}

uint8_t Mg4Battery::seq_08a_flag(uint32_t t) {
  if (t < SEQ_08A_FLAG_T1) {
    return 0x00;
  }
  if (t < SEQ_08A_FLAG_T2) {
    return 0x08;
  }
  return 0x00;
}

uint8_t Mg4Battery::seq_08a_level(uint32_t t) {
  if (t < SEQ_08A_LVL_T1) {
    return 0x00;
  }
  if (t < SEQ_08A_LVL_T2) {
    return 0x20;
  }
  return 0x40;
}

uint8_t Mg4Battery::seq_313_stat(uint32_t t) {
  if (t < SEQ_313_STAT_T1) {
    return 0x01;
  }
  if (t < SEQ_313_STAT_T2) {
    return 0x03;
  }
  return 0x05;
}

uint8_t Mg4Battery::seq_314_val(uint32_t t) {
  if (t < SEQ_314_VAL_T1) {
    return 0x02;
  }
  if (t < SEQ_314_VAL_T2) {
    return 0x2E;
  }
  if (t < SEQ_314_VAL_T3) {
    return 0x4C;
  }
  if (t < SEQ_314_VAL_T4) {
    return 0x56;
  }
  return 0x57;
}

uint8_t Mg4Battery::seq_314_hi(uint32_t t) {
  if (t < SEQ_314_HI_T1) {
    return 0xC8;
  }
  if (t < SEQ_314_HI_T2) {
    return 0x08;
  }
  if (t < SEQ_314_HI_T3) {
    return 0xC8;
  }
  if (t < SEQ_314_HI_T4) {
    return 0x48;
  }
  if (t < SEQ_314_HI_T5) {
    return 0x88;
  }
  if (t < SEQ_314_HI_T6) {
    return 0xC8;
  }
  return 0x08;
}

// Free-running 15-value rolling counter. `tick` is the absolute transmit
// count (never reset), so the counter never jumps on state changes.
uint8_t Mg4Battery::seq_cnt15(uint32_t tick, uint8_t base, uint32_t phase) {
  return (uint8_t)(base + ((tick + phase) % 15));
}

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

void Mg4Battery::snap_tick() {
  if (datalayer.battery.info.chemistry != battery_chemistry_enum::LFP) {
    return;
  }
  if (cell_voltage_freshness <= 0) {
    return;
  }
  uint16_t cell_max_mV = datalayer.battery.status.cell_max_voltage_mV;
  snap_tick_attempt(cell_max_mV);
  // Hold the forced 100% from SNAP_ABSORB_MV down to SNAP_HOLD_MV, so the
  // reported SoC doesn't flip as the cells cycle around the post-snap taper.
  if (!snapEnded || cell_max_mV < SNAP_HOLD_MV) {
    snapHoldFull = false;
  } else if (cell_max_mV >= SNAP_ABSORB_MV) {
    snapHoldFull = true;
  }
}

void Mg4Battery::snap_tick_attempt(uint16_t cell_max_mV) {
  if (cell_max_mV < SNAP_RESET_MV) {
    snapEnded = false;
    snap_over_s = 0;
    snap_ema_dA = 0.0f;
    snapAbsorbing = false;
    return;
  }
  if (snapEnded) {
    return;
  }
  if (cell_max_mV >= SNAP_ABORT_MV) {
    snapEnded = true;
    return;
  }
  // Skip snapping if SoC is already high enough - no drift worth correcting.
  if (cell_max_mV >= SNAP_SKIP_MV && datalayer.battery.status.real_soc >= SNAP_SKIP_SOC) {
    snapEnded = true;
    return;
  }
  if (cell_max_mV >= SNAP_ABSORB_MV) {
    snapAbsorbing = true;
  }
  snap_over_s = (cell_max_mV >= SNAP_OVER_MV) ? snap_over_s + 1 : 0;
  // ~30s exponential moving average of charge current (+ = charging, clamp discharge to 0).
  float chg_dA = (float)datalayer.battery.status.current_dA;
  if (chg_dA < 0.0f) {
    chg_dA = 0.0f;
  }
  snap_ema_dA += SNAP_EMA_ALPHA * (chg_dA - snap_ema_dA);
  bool current_done = (cell_max_mV >= SNAP_ABSORB_MV) && (snap_ema_dA < (float)SNAP_MIN_DA);
  if (snap_over_s >= SNAP_OVER_S || current_done) {
    snapEnded = true;
  }
}

bool Mg4Battery::snap_should_force_soc() {
  return datalayer.battery.info.chemistry == battery_chemistry_enum::LFP && snapHoldFull && cell_voltage_freshness > 0;
}

int32_t Mg4Battery::snap_clamp_power_W() {
  uint16_t v_dV = datalayer.battery.status.voltage_dV;
  if (v_dV == 0) {
    v_dV = (uint16_t)(datalayer.battery.info.number_of_cells * 33u);  // ~3.3V/cell fallback
  }
  int32_t w = ((int32_t)SNAP_MAX_DA * (int32_t)v_dV) / 100;
  return w > 0 ? w : 0;
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
  // LFP snap special-cases:
  // - While a snap is in (latched) absorption, the taper is replaced by a 5A
  //   clamp. SNAP_ABORT_MV in snap_tick() and the safety.cpp cell overvoltage
  //   limit bound the voltage instead.
  // - Once the snap has ended (succeeded, skipped or given up), taper to
  //   SNAP_ABSORB_MV instead, so the pack keeps working normally below the
  //   knee until it re-arms at SNAP_RESET_MV.
  {
    // Freshness already gated above.
    const bool lfp = datalayer.battery.info.chemistry == battery_chemistry_enum::LFP;
    int32_t cell_power_W;
    if (lfp && !snapEnded && snapAbsorbing) {
      cell_power_W = snap_clamp_power_W();
    } else {
      const int32_t cell_max_limit_mV = (lfp && snapEnded) ? SNAP_ABSORB_MV : working_cell_max_mV;
      cell_power_W = battery_charge_power_by_cell_max(datalayer.battery.status.cell_max_voltage_mV, cell_max_limit_mV,
                                                      CHARGE_TAPER_MV, CHARGE_HYSTERESIS_MV, MAX_CHARGE_POWER_W,
                                                      &voltageAtCellMax);
    }
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
  // linear taper down to CHARGE_TRICKLE_POWER_W at 100%. NMC only: LFP SoC
  // can't be judged from voltage below the knee, so a BMS reading high would
  // throttle the pack to a trickle long before it is full. LFP charge is left
  // to the cell voltage limits (and snap) above.
  if (datalayer.battery.info.chemistry != battery_chemistry_enum::LFP) {
    const int32_t soc_power_W = battery_charge_power_by_soc(datalayer.battery.status.real_soc, MAX_CHARGE_POWER_W,
                                                            CHARGE_TRICKLE_POWER_W, DERATE_CHARGE_ABOVE_SOC);
    if (soc_power_W < max_charge_power_W) {
      max_charge_power_W = soc_power_W;
    }
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

  // Sample the latest isolation resistance once per second into the ring the
  // info page dumps as a comma-separated series.
  if (iso_received) {
    iso_history[iso_history_head] = iso_raw;
    iso_history_head = (iso_history_head + 1) % ISO_HISTORY_SAMPLES;
    if (iso_history_count < ISO_HISTORY_SAMPLES) {
      iso_history_count++;
    }
  }

  if (temp_freshness > 0) {
    temp_freshness--;
  }

  snap_tick();
  if (soc_freshness > 0) {
    update_soc(snap_should_force_soc() ? 10000 : bms_soc_centipercent);
  }
  if (cell_voltage_freshness > 0) {
    cell_voltage_freshness--;
  }
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

      // [20] contains the max temperature sensor ID
      // [21] contains the min temperature sensor ID
      datalayer.battery.status.temperature_max_dC = ((int)rx_frame.data.u8[22] * 5) - 400;
      datalayer.battery.status.temperature_min_dC = ((int)rx_frame.data.u8[23] * 5) - 400;
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
            // The 51kWh LFP pack has the first two cells of each group of four
            // swapped in the 0x510 rows (cells 81-104), so swap them back.
            datalayer.battery.status.cell_voltages_mV[idx + 0] = (addr == 0x510) ? c3 : c2;
            datalayer.battery.status.cell_voltages_mV[idx + 1] = (addr == 0x510) ? c2 : c3;
            datalayer.battery.status.cell_voltages_mV[idx + 2] = c1;
            datalayer.battery.status.cell_voltages_mV[idx + 3] = c0;
          }
          /* We don't need temps here, we get them from 12C
        } else if (addr == 0x511) {  
          // Cell module temps are in 0x511

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
        */
        }
      }
      break;
    case 0x15B:
      // SoC

      //00050108001A21 01 80 00000000050508... ~10% SoC
      //00050108007B9A 49 64 00000000050508... ~60% SoC
      //                ^ ^^
      //          bits: 4 42

      soc_times_ten = ((rx_frame.data.u8[7] << 6) | (rx_frame.data.u8[8] >> 2)) & 0x3FF;
      bms_soc_centipercent = soc_times_ten * 10;
      soc_freshness = 10;
      update_soc(snap_should_force_soc() ? 10000 : bms_soc_centipercent);

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

// How long to wait for the contactor status on boot, before we start sending.
// Allows us to keep contactors closed during BE reboots.
static constexpr unsigned long CONTACTOR_STARTUP_GRACE_MS = 5000;  // max wait for the first 0x15B state

// Patch the 047 and 08A frames in place from the free-running sequencer.
// Rolling counters come from the absolute tick (never reset, so they never
// jump on state changes). Every other signal comes from time-in-sequence
// (tick - mg4_seq_start) with its own thresholds when CLOSING, or from a
// steady held level when CLOSED/OPENING.
void Mg4Battery::update_047_08a() {
  uint16_t voltage_dV = datalayer.battery.status.voltage_dV;

  // 0x047 VAL12 plateau tracks the live pack voltage (0.4 x voltage_dV).
  uint32_t target = ((uint32_t)voltage_dV * 2u) / 5u;
  if (target < 45) {
    target = 45;
  } else if (target > 0xFFF) {
    target = 0xFFF;
  }

  uint32_t tick = mg4_seq_tick;
  uint32_t elapsed = tick - mg4_seq_start;

  uint16_t val12;
  uint8_t request;
  uint8_t flag;
  uint8_t level;
  if (contactorState == ContactorState::CLOSED) {
    val12 = (uint16_t)target;
    request = 0x21;
    flag = 0x00;
    level = 0x40;
  } else if (contactorState == ContactorState::OPENING) {
    val12 = 45;
    request = 0x00;
    flag = 0x00;
    level = 0x00;
  } else {
    // CLOSING (WAITING never reaches here): each signal follows its own
    // schedule from sequence start, saturating at its final level.
    val12 = (elapsed < SEQ_VAL12_STEP) ? 45 : (uint16_t)target;
    request = seq_08a_request(elapsed);
    flag = seq_08a_flag(elapsed);
    level = seq_08a_level(elapsed);
  }

  uint8_t* f47 = MG4_047_FD.data.u8;
  uint8_t cnt = seq_cnt15(tick, 0xF0, SEQ_FAST_CNT_PHASE);
  f47[5] = cnt;
  f47[9] = (uint8_t)(val12 >> 4);                    // VAL12 high 8 bits
  f47[10] = (uint8_t)(((val12 & 0xF) << 4) | 0x0C);  // VAL12 low 4 bits + static 0xC nibble
  f47[4] = mg4_crc8(&f47[5]);
  f47[17] = cnt;
  f47[16] = mg4_crc8(&f47[17]);

  uint8_t* f8a = MG4_08A_FD.data.u8;
  f8a[5] = seq_cnt15(tick, 0x30, SEQ_FAST_CNT_PHASE);
  f8a[4] = mg4_crc8(&f8a[5]);
  f8a[17] = seq_cnt15(tick, 0x40, SEQ_FAST_CNT_PHASE);
  f8a[19] = request;
  f8a[20] = flag;
  f8a[16] = mg4_crc8(&f8a[17]);
  f8a[30] = level;
  // Last subfield has no CRC
}

// Patch the 313 and 314 frames in place from the free-running sequencer.
// Must be called after mg4_seq_tick was incremented for the paired fast
// frame, so slow_abs/slow_elapsed derive from the post-increment tick exactly
// like the legacy (index+1)/10 derivation.
void Mg4Battery::update_313_314() {
  uint16_t voltage_dV = datalayer.battery.status.voltage_dV;
  uint8_t* f13 = MG4_313_FD.data.u8;

  uint32_t tick_after = mg4_seq_tick;
  uint32_t slow_abs = tick_after / 10u;
  uint32_t slow_elapsed = (tick_after - mg4_seq_start) / 10u;

  uint8_t stat;
  uint16_t vala;
  uint16_t valc;
  uint16_t val16;
  uint8_t val314;
  uint8_t hi314;
  uint8_t ramp314;
  if (contactorState == ContactorState::CLOSED) {
    stat = 0x05;
    vala = SEQ_313_VALA_MAX;
    valc = SEQ_313_VALC_MAX;
    ramp314 = SEQ_314_RAMP_MAX;
    val314 = 0x57;
    hi314 = 0x08;
  } else if (contactorState == ContactorState::OPENING) {
    stat = 0x01;
    vala = 0;
    valc = 0;
    ramp314 = 0;
    val314 = 0x02;
    hi314 = 0xC8;
  } else {
    // CLOSING: each slow signal follows its own schedule, saturating.
    stat = seq_313_stat(slow_elapsed);
    vala = mg4_ramp((int)slow_elapsed, SEQ_313_RAMP_START, SEQ_313_RAMP_END, SEQ_313_VALA_MAX);
    valc = mg4_ramp((int)slow_elapsed, SEQ_313_RAMP_START, SEQ_313_RAMP_END, SEQ_313_VALC_MAX);
    ramp314 = (uint8_t)mg4_ramp((int)slow_elapsed, SEQ_314_RAMP_START, SEQ_314_RAMP_END, SEQ_314_RAMP_MAX);
    val314 = seq_314_val(slow_elapsed);
    hi314 = seq_314_hi(slow_elapsed);
  }

  f13[5] = seq_cnt15(slow_abs, 0xF0, SEQ_SLOW_CNT_PHASE);
  f13[10] = stat;
  f13[4] = mg4_crc8(&f13[5]);

  // VALA and VALC share one ramp shape but different magnitudes.
  f13[18] = (uint8_t)vala;
  f13[20] = (uint8_t)(0x80 | (valc >> 4));          // static hi nibble 8 + VALC hi nibble
  f13[21] = (uint8_t)(((valc & 0xF) << 4) | 0x08);  // VALC lo nibble + static lo nibble 8

  // VAL16 plateau tracks the live pack voltage (5 x voltage_dV, 12.5x the
  // 0x047 VAL12). In CLOSING it follows the legacy sampled step
  // ((slow-2)*10 < 252 ? idle : live); otherwise it holds its steady level.
  uint32_t target16 = (uint32_t)voltage_dV * 5u;
  if (target16 < 562) {
    target16 = 562;
  } else if (target16 > 0xFFFF) {
    target16 = 0xFFFF;
  }
  if (contactorState == ContactorState::CLOSED) {
    val16 = (uint16_t)target16;
  } else if (contactorState == ContactorState::OPENING) {
    val16 = 562;
  } else {
    uint32_t t16 = (slow_elapsed > 2) ? (slow_elapsed - 2u) * 10u : 0u;
    val16 = (t16 < SEQ_VAL12_STEP) ? 562 : (uint16_t)target16;
  }
  f13[29] = seq_cnt15(slow_abs, 0x30, SEQ_SLOW_CNT_PHASE);
  f13[32] = (uint8_t)(val16 >> 8);
  f13[33] = (uint8_t)val16;
  f13[28] = mg4_crc8(&f13[29]);

  uint8_t* f14 = MG4_314_FD.data.u8;
  f14[5] = seq_cnt15(slow_abs, 0x40, SEQ_SLOW_CNT_PHASE);
  f14[6] = val314;
  f14[7] = hi314;
  f14[4] = mg4_crc8(&f14[5]);
  f14[17] = seq_cnt15(slow_abs, 0x70, SEQ_SLOW_CNT_PHASE);
  f14[18] = ramp314;
  f14[16] = mg4_crc8(&f14[17]);
}

void Mg4Battery::reset_reclose_tracker() {
  reclose_count = 0;
  reclose_pos = 0;
  if (reclose_blocked) {
    reclose_blocked = false;
    clear_event(EVENT_CONTACTOR_RECLOSE_FAULT, battery_index);
    logging.printf("[MG4] Reclose fault cleared\n");
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
        logging.printf("[MG4] Req open\n");
        reset_reclose_tracker();
        mg4_seq_start = mg4_seq_tick;
        contactorState = ContactorState::OPENING;
      } else if (pack_contactors.received || currentMillis - contactorWaitStartMillis >= CONTACTOR_STARTUP_GRACE_MS) {
        // We now know the pack state, or have given up waiting for it.
        if (pack_contactors.isClosed()) {
          if (batteryIdentified) {
            // Pack contactors were already closed (eg, we rebooted without opening them).
            // Keep them closed.
            contactorWaitStartMillis = 0;
            logging.printf("[MG4] Stay closed\n");
            clear_event(EVENT_CONTACTOR_OPEN, battery_index);
            mg4_seq_start = mg4_seq_tick;
            contactorState = ContactorState::CLOSED;
          } else if (!startup_grace_expired) {
            // We haven't yet identified the battery, but the contactors were
            // already closed at boot, so keep them closed until the grace
            // period expires.
            if (contactorWaitStartMillis == 0) {
              // Start the wait timer for the grace period if needed.
              contactorWaitStartMillis = currentMillis;
            }
            logging.printf("[MG4] Remaining closed\n");
            clear_event(EVENT_CONTACTOR_OPEN, battery_index);
            mg4_seq_start = mg4_seq_tick;
            contactorState = ContactorState::CLOSED;
          } else {
            // Grace period expired and we still don't know the battery
            // identity. Open contactors.
            logging.printf("[MG4] No ID, opening\n");
            mg4_seq_start = mg4_seq_tick;
            contactorState = ContactorState::OPENING;
          }
        } else {
          if (batteryIdentified) {
            // Pack contactors are open, start the closing sequencer from its start.
            contactorWaitStartMillis = 0;
            logging.printf("[MG4] Closing\n");
            mg4_seq_start = mg4_seq_tick;
            contactorState = ContactorState::CLOSING;
          } else if (!pack_contactors.received && startup_grace_expired) {
            // Both ID and contactor state is still unknown, force contactors
            // open.
            logging.printf("[MG4] Unknown state and ID, opening\n");
            mg4_seq_start = mg4_seq_tick;
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
      // Driving the close sequencer (signals follow their own schedules
      // from mg4_seq_start, saturating at the final levels).

      if (open_requested) {
        // Open was requested, abort!
        logging.printf("[MG4] Req open\n");
        reset_reclose_tracker();
        mg4_seq_start = mg4_seq_tick;
        contactorState = ContactorState::OPENING;
      } else if (!batteryIdentified) {
        // Must not close from open while unidentified.
        logging.printf("[MG4] No ID, close aborted\n");
        mg4_seq_start = mg4_seq_tick;
        contactorState = ContactorState::OPENING;
      } else if (pack_contactors.isClosed()) {
        // The sequence has worked, the pack has closed. Any earlier surprise
        // open is resolved. We'll now stay in the closed state.
        clear_event(EVENT_CONTACTOR_OPEN, battery_index);
        contactorState = ContactorState::CLOSED;
      }
      break;

    case ContactorState::CLOSED:
      // The contactors are (presumably) currently closed. Holds the
      // closed signal levels; the free-running tick keeps counters alive.

      if (open_requested) {
        // Open requested, do that immediately.
        logging.printf("[MG4] Req open\n");
        reset_reclose_tracker();
        mg4_seq_start = mg4_seq_tick;
        contactorState = ContactorState::OPENING;
      } else if (!batteryIdentified && startup_grace_expired) {
        // We were staying closed over a reboot, but didn't identify the pack in
        // time. Open contactors.
        logging.printf("[MG4] No ID, opening\n");
        mg4_seq_start = mg4_seq_tick;
        contactorState = ContactorState::OPENING;
      } else if (pack_contactors.received && !pack_contactors.isClosed()) {
        // Contactors opened unexpectedly.
        if (batteryIdentified) {
          set_event(EVENT_CONTACTOR_OPEN, 0, battery_index);
          if (record_contactor_reclose(currentMillis)) {
            // The pack keeps opening by itself (e.g. HV isolation fault):
            // stop wearing out the contactors, hold open and raise a fatal
            // event. Only a manual open/close clears this latch.
            logging.printf("[MG4] Reclose fault, stay open\n");
            reclose_blocked = true;
            mg4_seq_start = mg4_seq_tick;
            contactorState = ContactorState::OPENING;
            set_event(EVENT_CONTACTOR_RECLOSE_FAULT, RECLOSE_TRIP_COUNT, battery_index);
          } else {
            // Try to reclose them by restarting the closing sequencer.
            logging.printf("[MG4] Opened, reclosing\n");
            mg4_seq_start = mg4_seq_tick;
            contactorState = ContactorState::CLOSING;
          }
        } else {
          // Must not reclose while unidentified.
          set_event(EVENT_CONTACTOR_OPEN, 0, battery_index);
          logging.printf("[MG4] Opened, no ID\n");
          mg4_seq_start = mg4_seq_tick;
          contactorState = ContactorState::OPENING;
        }
      }
      break;

    case ContactorState::OPENING:
      // Holds the open signal levels to open contactors and keep them open.

      // If close was requested, only proceed if we've identified the pack and
      // reclose is not blocked.
      if (!open_requested && batteryIdentified && !reclose_blocked) {
        logging.printf("[MG4] Req close\n");
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
    return;
  }

  if (currentMillis - previousMillis10 >= INTERVAL_10_MS) {
    previousMillis10 = currentMillis;

    contactor_state_tick(currentMillis);

    // We don't send any contactor messages until we've decided whether we're
    // opening or closing (to allow a closed pack to stay closed during reboot).
    if (contactorState != ContactorState::WAITING_FOR_PACK) {
      update_047_08a();
      transmit_can_frame(&MG4_047_FD);
      transmit_can_frame(&MG4_08A_FD);

      // Free-running: advance the shared 10ms tick. Counters derive from its
      // absolute value, sequence signals from (tick - mg4_seq_start). No
      // wrapping, no per-state loops; CLOSING signals saturate and
      // CLOSED/OPENING hold steady levels (see update_047_08a).
      ++mg4_seq_tick;
    }

    if (currentMillis - previousMillis100 >= INTERVAL_100_MS) {
      previousMillis100 = currentMillis;

      // 0x4F3 (FD) wakeup/keep-alive (unsure if this is needed).
      transmit_can_frame(&MG4_4F3_FD);

      if (contactorState != ContactorState::WAITING_FOR_PACK) {
        // Called after the fast tick increment, so the slow derivation
        // (tick - start)/10 matches the legacy (index+1)/10 exactly.
        update_313_314();
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
    case POLL_ISOLATION_RESISTANCE:
      // 2-byte count, scaled by 500 to get ohms.
      iso_raw = (uint16_t)value;
      iso_received = true;
      break;
    // case POLL_MIN_CELL_TEMPERATURE:
    //   datalayer.battery.status.temperature_min_dC = ((int32_t)value - 20000) / 50;
    //   temp_freshness = 10;
    //   break;
    // case POLL_MAX_CELL_TEMPERATURE:
    //   datalayer.battery.status.temperature_max_dC = ((int32_t)value - 20000) / 50;
    //   temp_freshness = 10;
    //   break;
    case POLL_ECU_HARDWARE_NUMBER:
      memcpy(pid_ecu_hw_number, data, length > sizeof(pid_ecu_hw_number) ? sizeof(pid_ecu_hw_number) : length);
      if (!batteryIdentified)
        identify_battery();
      break;
    case POLL_ECU_SOFTWARE_NUMBER:
      memcpy(pid_ecu_sw_number, data, length > sizeof(pid_ecu_sw_number) ? sizeof(pid_ecu_sw_number) : length);
      break;  // End of cycle
  }

  if (pid == did_sweep_in_flight) {
    // Answer to a background sweep request: record it and let the poll list
    // continue.
    record_did_sweep_result(pid, data, length);
    did_sweep_in_flight = 0;
    return 0;
  }

  if (did_sweep_active()) {
    // Answer from the poll list: interleave the next sweep DID. Sweep DIDs
    // that are rejected or time out aren't retried - the scan list just
    // resumes, and the next poll answer hands out the following DID.
    did_sweep_in_flight = (uint16_t)did_sweep_next++;
    return did_sweep_in_flight;
  }

  return 0;  // Continue normal PID cycling
}

Mg4Battery::~Mg4Battery() {
  free(did_sweep_buf);
}

void Mg4Battery::record_did_sweep_result(uint16_t did, const uint8_t* data, uint16_t length) {
  const uint16_t kept = length > DID_SWEEP_MAX_DATA_LEN ? DID_SWEEP_MAX_DATA_LEN : length;
  const uint32_t needed = did_sweep_buf_len + DID_SWEEP_RECORD_HEADER_LEN + kept;
  if (needed > DID_SWEEP_BUF_MAX) {
    did_sweep_dropped++;
    return;
  }
  if (needed > did_sweep_buf_capacity) {
    uint32_t new_capacity = did_sweep_buf_capacity + DID_SWEEP_BUF_CHUNK;
    if (new_capacity > DID_SWEEP_BUF_MAX) {
      new_capacity = DID_SWEEP_BUF_MAX;
    }
    uint8_t* grown = (uint8_t*)realloc(did_sweep_buf, new_capacity);
    if (grown == nullptr) {
      did_sweep_dropped++;
      return;
    }
    did_sweep_buf = grown;
    did_sweep_buf_capacity = new_capacity;
  }

  uint8_t* rec = did_sweep_buf + did_sweep_buf_len;
  rec[0] = did >> 8;
  rec[1] = did & 0xFF;
  rec[2] = length > 255 ? 255 : length;
  memcpy(rec + DID_SWEEP_RECORD_HEADER_LEN, data, kept);
  did_sweep_buf_len = needed;
  did_sweep_answered++;
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

    working_cell_max_mV = datalayer.battery.info.max_cell_voltage_mV -
                          ((chem == LFP) ? WORKING_MAX_MARGIN_LFP_MV : WORKING_MAX_MARGIN_MV);
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

  static const uint16_t POLL_LIST[] = {POLL_BATTERY_SOH, POLL_ISOLATION_RESISTANCE,
                                       // POLL_MIN_CELL_TEMPERATURE,
                                       // POLL_MAX_CELL_TEMPERATURE,
                                       POLL_ECU_HARDWARE_NUMBER, POLL_ECU_SOFTWARE_NUMBER};

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

  datalayer.battery.info.max_design_voltage_dV =
      (datalayer.battery.info.number_of_cells * datalayer.battery.info.max_cell_voltage_mV) / 100;
  datalayer.battery.info.min_design_voltage_dV =
      (datalayer.battery.info.number_of_cells * datalayer.battery.info.min_cell_voltage_mV) / 100;
}

// Renders characters if printable, otherwise as [xx] hex. Truncates rather
// than overflowing buf_size.
static void print_chars_or_hex(char* buf, uint16_t buf_size, const uint8_t* data, uint16_t length) {
  int ptr = 0;
  for (int i = 0; i < length && ptr < buf_size - 5; i++) {
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
  // Reserve enough up front that the appends below never reallocate: ~1KB of
  // fixed text and sweep header, up to 9 chars per isolation history sample
  // (8-digit ohms + comma), and one page of the sweep listing.
  String html;
  html.reserve(1024 + iso_history_count * 9 + DID_SWEEP_PAGE_HTML_BUDGET);

  // Pack-reported precharge/contactor state (0x15B byte[21]&0xF)
  html += "<h3>Precharge/contactor state</h3>";
  html += "State: ";
  html += pack_contactors.label();
  html += " (";
  html += pack_contactors.state;
  html += ")<br>Snap attempt active: ";
  html += snapEnded ? "NO" : "YES";
  html += "<br>Isolation resistance: ";
  if (iso_received) {
    html += String((uint32_t)iso_raw * ISO_OHMS_PER_COUNT);
    html += " ohms";
  } else {
    html += "N/A";
  }
  if (iso_history_count > 0) {
    // Oldest sample first, comma separated, so the series can be pasted into a
    // spreadsheet and plotted.
    html += "<br>Isolation history (ohms): ";
    for (uint16_t i = 0; i < iso_history_count; i++) {
      uint16_t idx = (iso_history_head + ISO_HISTORY_SAMPLES - iso_history_count + i) % ISO_HISTORY_SAMPLES;
      if (i > 0) {
        html += ",";
      }
      html += String((uint32_t)iso_history[idx] * ISO_OHMS_PER_COUNT);
    }
  }
  html += "<br>Pack serial: " + String(ntsc_serial);
  char buf[64];
  print_chars_or_hex(buf, sizeof(buf), pid_ecu_hw_number, sizeof(pid_ecu_hw_number));
  html += "<br>ECU hardware: ";
  html += buf;
  print_chars_or_hex(buf, sizeof(buf), pid_ecu_sw_number, sizeof(pid_ecu_sw_number));
  html += "<br>ECU software: ";
  html += buf;

  render_did_sweep_html(html);

  return html;
}

void Mg4Battery::render_did_sweep_html(String& html) {
  char buf[DID_SWEEP_MAX_DATA_LEN * 4 + 8];

  html += "<h3>UDS DID sweep</h3>";
  if (did_sweep_active()) {
    snprintf(buf, sizeof(buf), "In progress: %lu/%lu DIDs tried, next 0x%04lX",
             (unsigned long)(did_sweep_next - DID_SWEEP_FIRST), (unsigned long)DID_SWEEP_TOTAL,
             (unsigned long)did_sweep_next);
  } else {
    snprintf(buf, sizeof(buf), "Complete: %lu DIDs tried", (unsigned long)DID_SWEEP_TOTAL);
  }
  html += buf;
  snprintf(buf, sizeof(buf), "<br>%u answered, %lu/%lu bytes stored", did_sweep_answered,
           (unsigned long)did_sweep_buf_len, (unsigned long)DID_SWEEP_BUF_MAX);
  html += buf;
  if (did_sweep_dropped > 0) {
    snprintf(buf, sizeof(buf), ", %u dropped (buffer full)", did_sweep_dropped);
    html += buf;
  }
  html += "<br>";

  if (did_sweep_buf_len == 0) {
    return;
  }

  // The listing is split into pages of at most DID_SWEEP_PAGE_HTML_BUDGET
  // rendered bytes, showing the next page on each reload so the page never
  // has to hold the whole listing at once. Records are only ever appended, so
  // earlier page boundaries stay put while the sweep runs.
  uint16_t pages = 0;
  uint32_t page_start = 0;
  for (uint32_t pos = 0; pos < did_sweep_buf_len; pos = did_sweep_page_end(pos)) {
    if (pages == did_sweep_page) {
      page_start = pos;
    }
    pages++;
  }
  if (did_sweep_page >= pages) {
    did_sweep_page = 0;
    page_start = 0;
  }
  const uint32_t page_end = did_sweep_page_end(page_start);

  if (pages > 1) {
    snprintf(buf, sizeof(buf), "Page %u/%u (reload for the next page)<br>", did_sweep_page + 1, pages);
    html += buf;
  }

  // Each answer is listed as "DID: data (length)", flowing into columns.
  html += "<div style='columns: 3 320px; font-family: monospace;'>";
  for (uint32_t pos = page_start; pos < page_end;) {
    const uint8_t* rec = did_sweep_buf + pos;
    const uint16_t did = (rec[0] << 8) | rec[1];
    const uint8_t length = rec[2];
    const uint16_t kept = length > DID_SWEEP_MAX_DATA_LEN ? DID_SWEEP_MAX_DATA_LEN : length;
    snprintf(buf, sizeof(buf), "%04X: ", did);
    html += buf;
    print_chars_or_hex(buf, sizeof(buf), rec + DID_SWEEP_RECORD_HEADER_LEN, kept);
    html += buf;
    snprintf(buf, sizeof(buf), kept < length ? " (%u, truncated)<br>" : " (%u)<br>", length);
    html += buf;
    pos += DID_SWEEP_RECORD_HEADER_LEN + kept;
  }
  html += "</div>";

  did_sweep_page = (did_sweep_page + 1) % pages;
}

uint32_t Mg4Battery::did_sweep_page_end(uint32_t start) const {
  // Takes records from start while their worst-case rendered size (4 chars per
  // data byte plus the "XXXX: " / " (nnn, truncated)<br>" decoration) fits the
  // page budget, always taking at least one.
  uint32_t pos = start;
  uint32_t rendered = 0;
  while (pos + DID_SWEEP_RECORD_HEADER_LEN <= did_sweep_buf_len) {
    const uint8_t length = did_sweep_buf[pos + 2];
    const uint16_t kept = length > DID_SWEEP_MAX_DATA_LEN ? DID_SWEEP_MAX_DATA_LEN : length;
    const uint32_t cost = kept * 4 + DID_SWEEP_RECORD_HTML_OVERHEAD;
    if (pos > start && rendered + cost > DID_SWEEP_PAGE_HTML_BUDGET) {
      break;
    }
    rendered += cost;
    pos += DID_SWEEP_RECORD_HEADER_LEN + kept;
  }
  return pos;
}
