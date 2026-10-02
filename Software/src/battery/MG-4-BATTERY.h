#pragma once

#include "UdsCanBattery.h"

class Mg4Battery : public UdsCanBattery {
 public:
  virtual void setup(void);
  virtual void handle_incoming_can_frame(CAN_frame rx_frame);
  virtual uint16_t handle_pid(uint16_t pid, uint32_t value, const uint8_t* data, uint16_t length);
  virtual void update_values();
  virtual void transmit_can(unsigned long currentMillis);

  static constexpr const char* Name = "MG4 battery";

  String get_uds_info_html() override;
  const char* get_dtc_json_filename() override { return "mg_dtc.json"; }

  // Contactor management state machine
  enum class ContactorState {
    WAITING_FOR_PACK,  // Silent: waiting for the first 0x15B state (or grace expiry)
    CLOSING,           // Driving the close sequencer from its start (free-running, see below)
    CLOSED,            // Pack confirmed closed, holding the closed signal levels
    OPENING,           // Open requested, holding the open signal levels
  };

 protected:
  // Visible to tests via subclass; production code treats as private.
  bool batteryIdentified = false;
  ContactorState contactorState = ContactorState::WAITING_FOR_PACK;
  bool reclose_blocked = false;

  // Free-running contactor-close sequencer. A single 10ms tick counts
  // transmitted 047/08A frames since boot (paused while WAITING, so the
  // first sequence always starts at 0). Rolling counters derive from the
  // absolute tick so they never jump on state changes; every other signal
  // derives from time-in-sequence (tick - mg4_seq_start) with its own
  // thresholds below, saturating at its final level instead of looping.
  // Threshold values preserve the legacy 800-frame capture timing exactly.
  uint32_t mg4_seq_tick = 0;
  uint32_t mg4_seq_start = 0;

  static constexpr uint32_t SEQ_VAL12_STEP = 252;  // fast ticks till precharge plateau

  static constexpr uint32_t SEQ_08A_REQ_T1 = 190;
  static constexpr uint32_t SEQ_08A_REQ_T2 = 309;  // 190 + 119
  static constexpr uint32_t SEQ_08A_FLAG_T1 = 188;
  static constexpr uint32_t SEQ_08A_FLAG_T2 = 407;  // 188 + 219
  static constexpr uint32_t SEQ_08A_LVL_T1 = 307;
  static constexpr uint32_t SEQ_08A_LVL_T2 = 337;  // 307 + 30

  static constexpr uint32_t SEQ_313_STAT_T1 = 10;
  static constexpr uint32_t SEQ_313_STAT_T2 = 33;  // 10 + 23
  static constexpr int SEQ_313_RAMP_START = 32;
  static constexpr int SEQ_313_RAMP_END = 78;
  static constexpr uint16_t SEQ_313_VALA_MAX = 77;
  static constexpr uint16_t SEQ_313_VALC_MAX = 90;

  static constexpr uint32_t SEQ_314_VAL_T1 = 27;
  static constexpr uint32_t SEQ_314_VAL_T2 = 28;
  static constexpr uint32_t SEQ_314_VAL_T3 = 29;
  static constexpr uint32_t SEQ_314_VAL_T4 = 64;  // 29 + 35
  static constexpr uint32_t SEQ_314_HI_T1 = 27;
  static constexpr uint32_t SEQ_314_HI_T2 = 28;
  static constexpr uint32_t SEQ_314_HI_T3 = 29;
  static constexpr uint32_t SEQ_314_HI_T4 = 31;  // 29 + 2
  static constexpr uint32_t SEQ_314_HI_T5 = 32;
  static constexpr uint32_t SEQ_314_HI_T6 = 64;  // 32 + 32
  static constexpr int SEQ_314_RAMP_START = 44;
  static constexpr int SEQ_314_RAMP_END = 59;
  static constexpr uint16_t SEQ_314_RAMP_MAX = 48;

  static constexpr uint32_t SEQ_FAST_CNT_PHASE = 11;
  static constexpr uint32_t SEQ_SLOW_CNT_PHASE = 7;

  static uint8_t seq_08a_request(uint32_t t);
  static uint8_t seq_08a_flag(uint32_t t);
  static uint8_t seq_08a_level(uint32_t t);
  static uint8_t seq_313_stat(uint32_t t);
  static uint8_t seq_314_val(uint32_t t);
  static uint8_t seq_314_hi(uint32_t t);
  static uint8_t seq_cnt15(uint32_t tick, uint8_t base, uint32_t phase);

  // Constants controlling the snapping attempt mechanism. The BMS will snap the
  // SoC to 100% (resetting its internal coulomb counter) when a cell hits 3.75V
  // briefly. We want to do this carefully to avoid charging cells to 3.75V on a
  // regular basis:
  // - Don't try the snap if the SoC is already high enough (drift not worth
  //   correcting)
  // - Set a max current limit of 5A during the attempt, but also a 3A min
  //   average current limit, so we don't trickle charge the cells up to 3.75V.
  //   The whole snapping cycle should be quick, and the cells should relax back
  //   down to a sane voltage once it is over.
  // - Give up if we're over 3.75V/cell for more than 30 seconds.
  // - Abort if we ever hit 3.76V/cell. This sits just below the 3.765V
  //   MAX_CELL_VOLTAGE_LFP_MV, where safety.cpp blocks charging and raises
  //   EVENT_CELL_OVER_VOLTAGE, so a normal snap never raises that event.
  // - Once the attempt has ended (for any reason), cap the working max cell
  //   voltage at SNAP_ABSORB_MV so the pack runs normally below the knee, and
  //   don't try again till we've discharged back down below SNAP_RESET_MV.
  //   Report 100% SoC from SNAP_ABSORB_MV until the cells fall below
  //   SNAP_HOLD_MV.
  static constexpr int32_t SNAP_ABSORB_MV = 3650;
  static constexpr int32_t SNAP_OVER_MV = 3750;
  static constexpr int32_t SNAP_OVER_S = 30;
  static constexpr int32_t SNAP_ABORT_MV = 3760;
  static constexpr int32_t SNAP_SKIP_MV = 3600;   // no-drift check threshold
  static constexpr int32_t SNAP_SKIP_SOC = 9800;  // skip snap if BMS SoC here
  static constexpr int32_t SNAP_RESET_MV = 3450;
  static constexpr int32_t SNAP_HOLD_MV = 3600;  // forced 100% SoC held down to here
  static constexpr int32_t SNAP_MAX_DA = 50;     // 5A max during snap
  static constexpr int32_t SNAP_MIN_DA = 30;     // 3A min during snap
  static constexpr float SNAP_EMA_ALPHA = 1.0f / 30;

  bool snapEnded = false;
  int32_t snap_over_s = 0;
  float snap_ema_dA = 0.0f;
  // Latched once cell max reaches SNAP_ABSORB_MV during an armed snap, so the
  // 5A clamp doesn't drop out (and back in) when the reduced current lets the
  // cell voltage sag back under the threshold. Cleared at SNAP_RESET_MV.
  bool snapAbsorbing = false;
  // Forced 100% SoC once the snap has ended: set at SNAP_ABSORB_MV, released
  // below SNAP_HOLD_MV.
  bool snapHoldFull = false;

  void snap_tick();
  void snap_tick_attempt(uint16_t cell_max_mV);
  bool snap_should_force_soc();
  int32_t snap_clamp_power_W();

 private:
  static const uint16_t MAX_CELL_DEVIATION_LFP_MV = 400;
  static const uint16_t MAX_CELL_DEVIATION_NMC_MV = 150;

  static const uint16_t WORKING_MAX_MARGIN_MV = 10;
  // LFP gets a wider margin so the event limit clears the snap abort
  // threshold while the working max stays at 3750mV.
  static const uint16_t WORKING_MAX_MARGIN_LFP_MV = 15;
  static const uint16_t WORKING_MIN_MARGIN_MV = 300;

  static const uint16_t MAX_CELL_VOLTAGE_LFP_MV = 3750 + WORKING_MAX_MARGIN_LFP_MV;
  static const uint16_t MIN_CELL_VOLTAGE_LFP_MV = 2500;
  static const uint16_t MAX_CELL_VOLTAGE_NMC_MV = 4200 + WORKING_MAX_MARGIN_MV;
  static const uint16_t MIN_CELL_VOLTAGE_NMC_MV = 2700;

  int32_t working_cell_min_mV = 0;
  int32_t working_cell_max_mV = 0;
  // Latched flags for cell-voltage hysteresis (persist between update_values calls)
  bool voltageAtCellMax = false;
  bool voltageAtCellMin = false;
  int32_t cell_voltage_freshness = 0;
  int32_t soc_freshness = 0;
  uint16_t bms_soc_centipercent = 0;  // last SoC reported in 0x15B
  int32_t temp_freshness = 0;

  int16_t module_temperatures_dC[12] = {0};
  int16_t module_temps_received = 0;

  // For monitoring the actual battery-reported contactor state.
  struct PackContactorFeedback {
    bool received = false;
    uint8_t state = 0xFF;  // 0xFF = no data yet
    bool isClosed() const { return received && state == 7; }
    bool isPrecharging() const { return received && state == 11; }
    const char* label() const {
      if (isClosed()) {
        return "Closed";
      }
      if (isPrecharging()) {
        return "Precharge";
      }
      if (received && state == 3) {
        return "Open";
      }
      return "Unknown";
    }
    uint8_t contactsEngaged() const { return isClosed() ? 1 : (isPrecharging() ? 3 : 0); }
  };

  char ntsc_serial[29] = {0};  // eg: 0AFPEG10879103D7N3000095
  PackContactorFeedback pack_contactors;
  unsigned long contactorWaitStartMillis = 0;

  // Automatic-reclose guard. A pack that opens by itself (e.g. HV isolation
  // fault) must not be reclosed forever: reclose timestamps form a sliding
  // window, and RECLOSE_TRIP_COUNT recloses within RECLOSE_WINDOW_MS latches
  // the drive in OPENING and raises a fatal event. A manual open/close
  // (homepage button / estop) resets the tracker so the user can retry.
  static constexpr int RECLOSE_TRIP_COUNT = 10;
  static constexpr unsigned long RECLOSE_WINDOW_MS = 300000;  // 300 s
  unsigned long reclose_times[RECLOSE_TRIP_COUNT] = {0};
  int reclose_count = 0;  // valid entries in reclose_times
  int reclose_pos = 0;    // next write index (oldest entry when full)
  bool last_equipment_stop = false;

  uint32_t calculate_max_discharge_power_W();
  uint32_t calculate_max_charge_power_W();
  void identify_battery();
  void update_047_08a();
  void update_313_314();
  void contactor_state_tick(unsigned long currentMillis);
  void reset_reclose_tracker();
  bool record_contactor_reclose(unsigned long now_ms);

  unsigned long lastTickMillis = 0;

  unsigned long previousMillis10 = 0;   // will store last time a 10ms CAN Message was send
  unsigned long previousMillis100 = 0;  // will store last time a 100ms CAN Message was send

  static const uint16_t POLL_BATTERY_VOLTAGE = 0xB042;
  static const uint16_t POLL_BATTERY_CURRENT = 0xB043;
  static const uint16_t POLL_BATTERY_SOC = 0xB046;
  static const uint16_t POLL_MIN_CELL_TEMPERATURE = 0xB057;
  static const uint16_t POLL_MAX_CELL_TEMPERATURE = 0xB056;
  static const uint16_t POLL_BATTERY_SOH = 0xB061;
  static const uint16_t POLL_ISOLATION_RESISTANCE = 0xB045;
  static const uint16_t POLL_ECU_HARDWARE_NUMBER = 0xF192;
  static const uint16_t POLL_ECU_SOFTWARE_NUMBER = 0xF194;

  // ECU identifier payloads (10 ASCII characters each)
  uint8_t pid_ecu_hw_number[10] = {0};
  uint8_t pid_ecu_sw_number[10] = {0};

  // Pack-reported isolation resistance. The PID value is a 2-byte count scaled
  // by 500 to get ohms. Keep a 1 Hz ring of recent raw counts so the info page
  // can dump a plottable series without storing every PID response.
  static constexpr uint32_t ISO_OHMS_PER_COUNT = 500;
  static constexpr int ISO_HISTORY_SAMPLES = 120;  // 2 minutes at 1 Hz
  uint16_t iso_raw = 0;
  bool iso_received = false;
  uint16_t iso_history[ISO_HISTORY_SAMPLES] = {0};
  uint16_t iso_history_count = 0;
  uint16_t iso_history_head = 0;  // next write position

  CAN_frame MG4_4F3_FD = {.FD = true,
                          .ext_ID = false,
                          .DLC = 8,
                          .ID = 0x4F3,
                          .data = {0xF3, 0x10, 0x48, 0x00, 0xFF, 0xFF, 0x00, 0x11}};
  // Static templates for the generated frames. update_047_08a /
  // update_313_314 patch only the dynamic bytes (counters, sequenced steps,
  // ramps, voltage plateaus) plus CRCs; everything else here is sent as-is.
  CAN_frame MG4_047_FD = {.FD = true,
                          .ext_ID = false,
                          .DLC = 24,
                          .ID = 0x047,
                          .data = {0x00, 0x01, 0x27, 0x08, 0x00, 0x00, 0x80, 0x04, 0x00, 0x00, 0x0C, 0x01,
                                   0x00, 0x01, 0x48, 0x08, 0x00, 0x00, 0x6A, 0x06, 0x9F, 0xFF, 0xF0, 0xFF}};
  CAN_frame MG4_08A_FD = {
      .FD = true,
      .ext_ID = false,
      .DLC = 48,
      .ID = 0x08A,
      .data = {0x00, 0x01, 0x18, 0x08, 0x00, 0x00, 0x75, 0x2F, 0x75, 0x30, 0x75, 0x30, 0x00, 0x01, 0x00, 0x08,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0x00, 0x01, 0x53, 0x08, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
  CAN_frame MG4_313_FD = {
      .FD = true,
      .ext_ID = false,
      .DLC = 48,
      .ID = 0x313,
      .data = {0x00, 0x04, 0x02, 0x08, 0x00, 0x00, 0x3D, 0x54, 0x55, 0xF2, 0x00, 0xE7, 0x00, 0x04, 0x00, 0x08,
               0x00, 0x01, 0x00, 0x78, 0x80, 0x08, 0x69, 0x00, 0x00, 0x04, 0x01, 0x08, 0x00, 0x00, 0xFF, 0x4C,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
  CAN_frame MG4_314_FD = {.FD = true,
                          .ext_ID = false,
                          .DLC = 24,
                          .ID = 0x314,
                          .data = {0x00, 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xE4, 0x40, 0x56,
                                   0x00, 0x04, 0x05, 0x08, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x04, 0xE0, 0x41}};
};
