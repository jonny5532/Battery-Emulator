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

  // Sleep/wake (More battery info buttons): shut the pack down the way the car
  // does, then stop all transmission so the BMS can go to sleep.
  bool supports_sleep() override { return true; }
  bool is_sleeping() override;
  void request_sleep() override { sleep_requested = true; }
  void request_wake() override { wake_requested = true; }
  // Balance cycle (More battery info buttons): sleep the pack for a fixed
  // time, then wake it and hold it awake and open while the BMS balances.
  bool supports_balance_cycle() override { return true; }
  bool is_balance_cycle_active() override { return balance_cycle_requested || bal_cycle != BalCycle::OFF; }
  void request_balance_cycle() override { balance_cycle_requested = true; }
  void request_balance_stop() override { balance_stop_requested = true; }
  const char* get_dtc_json_filename() override { return "mg_dtc.json"; }

  // Contactor management state machine
  enum class ContactorState {
    WAITING_FOR_PACK,  // Silent: waiting for the first 0x15B state (or grace expiry)
    CLOSING,           // Driving the close sequencer from its start (free-running, see below)
    CLOSED,            // Pack confirmed closed, holding the closed signal levels
    OPENING,           // Open requested, holding the open signal levels
    SHUTTING_DOWN,     // Running the car's graceful open sequence before sleeping
    SLEEPING,          // Transmitting nothing at all, so the BMS can sleep
    WAKING,            // Car-style power-up pattern, waiting for the BMS to talk
    BAL_HOLD,          // Balance cycle: awake with the open levels, never closing
  };

  // Balance cycle phase: OFF, or SLEEP (shutting down, asleep or waking) and
  // HOLD (awake in BAL_HOLD while the BMS balances).
  enum class BalCycle { OFF, SLEEP, HOLD };

  // Steps of SHUTTING_DOWN, as captured from the car (see SHUTDOWN_HOLD_T).
  enum class ShutdownPhase {
    HOLD,          // request 0x01, level 0x20; waiting out the hold and for zero current
    REQUEST_OPEN,  // request 0x00, level 0x20; waiting for the pack to report open
    OPEN_HOLD,     // pack open, holding the open levels before going silent
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

  // Graceful shutdown and wake timings, in 10ms ticks of the sequencer. The
  // car holds request 0x01 for 6.0s (level 0x40 -> 0x20 after 0.2s) before
  // requesting open, and the pack reports open 0.3-0.8s later.
  static constexpr uint32_t SHUTDOWN_LEVEL_T = 20;
  static constexpr uint32_t SHUTDOWN_HOLD_T = 600;
  // Past the hold, wait up to here for the current to fall, else cancel.
  static constexpr uint32_t SHUTDOWN_HOLD_MAX_T = 1000;
  static constexpr uint32_t SHUTDOWN_OPEN_TIMEOUT_T = 300;
  static constexpr uint32_t SHUTDOWN_OPEN_HOLD_T = 200;
  static constexpr int16_t SHUTDOWN_IDLE_DA = 10;  // |current| below 1A counts as idle
  // Car power-up: request 0x03 (flag 0x04 on the first frame) for ~2s, then 0x00.
  static constexpr uint32_t WAKE_REQ03_T = 200;
  // The BMS counts as silent once nothing has arrived for this long.
  static constexpr unsigned long SLEEP_SILENT_MS = 5000;

  ShutdownPhase shutdown_phase = ShutdownPhase::HOLD;
  uint32_t shutdown_phase_start = 0;
  // Set from the web task, consumed by contactor_state_tick().
  bool sleep_requested = false;
  bool wake_requested = false;
  // Why the last sleep attempt didn't reach SLEEPING, or nullptr.
  const char* sleep_abort_reason = nullptr;
  // Sleep bookkeeping for the info page and log: when SLEEPING began, when
  // the BMS was last heard (0 = not since sleep began), when it first went
  // silent (0 = not yet) and how often it woke up again by itself.
  unsigned long sleep_start_ms = 0;
  unsigned long sleep_last_rx_ms = 0;
  unsigned long sleep_silent_after_ms = 0;
  bool sleep_ever_silent = false;
  uint16_t sleep_self_wakes = 0;
  bool sleep_bms_silent = false;
  // Frames received since the last 10ms tick (set by the CAN receive path).
  uint16_t sleep_rx_count = 0;
  uint32_t sleep_first_rx_id = 0;
  unsigned long last_tick_ms = 0;

  void begin_shutdown();
  void abort_shutdown(ContactorState next, const char* reason);
  void sleep_tick(unsigned long currentMillis);
  bool shutdown_current_idle() const;
  void render_sleep_html(String& html);
  // Worst-case rendered size of the power state section, with the balance
  // cycle block.
  static constexpr uint32_t SLEEP_HTML_BUDGET = 640;

  // ---- Balance cycle ----
  // The BMS only plans balancing in the first ~16s after a wake that follows
  // a valid sleep measured by its RTC, and rest (sleep + time awake with the
  // contactors open) counts towards its triggers: top of charge needs a cell
  // >= 3.362V and 30 min rest; SoC >= 80% or <= 26% needs a spread threshold
  // that falls from 3.0% at 1 min rest to 1.0% at 60 min. Mid-SoC packs never
  // balance. So the cycle is: graceful shutdown, BAL_CYCLE_SLEEP_MS from the
  // BMS going silent, wake, then BAL_HOLD (open levels, limits 0) while any
  // cell has 0x689 minutes left. 0xB0E5 page 6 confirms the sleep counted.
  // Not persisted: a reboot mid-cycle starts up into the normal logic.
  static constexpr unsigned long BAL_CYCLE_SLEEP_MS = 35UL * 60 * 1000;
  static constexpr unsigned long BAL_HOLD_MAX_MS = 13UL * 60 * 60 * 1000;  // 12h target cap plus margin
  // Page 6 must report a valid sleep of at least this long.
  static constexpr uint32_t BAL_SLEEP_ACCEPT_S = BAL_CYCLE_SLEEP_MS / 1000 * 9 / 10;
  // End the cycle if the BMS keeps talking this long after shutdown (or a self-wake).
  static constexpr unsigned long BAL_SILENCE_TIMEOUT_MS = 10UL * 60 * 1000;
  // Full sweeps without the remaining total falling before reporting a pause.
  static constexpr uint8_t BAL_PAUSE_SWEEPS = 2;
  // SoC trigger windows, for explaining "no balancing planned".
  static constexpr uint16_t BAL_HIGH_SOC_CP = 8000;
  static constexpr uint16_t BAL_LOW_SOC_CP = 2600;
  static constexpr uint16_t BAL_TOP_CELL_MV = 3362;
  // Execution pauses at or below this SoC.
  static constexpr uint16_t BAL_INHIBIT_SOC_CP = 1000;

  BalCycle bal_cycle = BalCycle::OFF;
  // Set from the web task, consumed by contactor_state_tick().
  bool balance_cycle_requested = false;
  bool balance_stop_requested = false;
  bool bal_wake_armed = false;  // bal_wake_at_ms is set (the BMS is silent)
  unsigned long bal_wake_at_ms = 0;
  unsigned long bal_noisy_since_ms = 0;  // when the BMS last started talking while asleep
  unsigned long bal_hold_start_ms = 0;
  bool bal_sleep_checked = false;  // 0xB0E5 page 6 accepted
  uint32_t bal_hold_sweeps = 0;    // bal_sweep_count when BAL_HOLD began / last handled
  bool bal_hold_have_sum = false;  // a full sweep has been seen in this hold
  uint32_t bal_hold_prev_sum = 0;
  uint8_t bal_stall_sweeps = 0;
  // Outcome of the last cycle ("" while none has finished).
  char bal_cycle_result[96] = {0};

  void start_bal_cycle();
  void end_bal_cycle(const char* fmt, ...);
  void enter_bal_hold(unsigned long now_ms);
  void leave_bal_hold();
  void bal_hold_tick(unsigned long now_ms);
  const char* bal_no_plan_hint() const;
  const char* bal_inhibit_hint() const;
  void render_bal_cycle_html(String& html);

  // ---- BMS mode (drive vs charge), display only ----
  // 0x4E1 (BMS NM) b7 is the partial network: 0x01 default, 0x13 AC, 0x04 DC,
  // 0x20 V2L. Driving = contactors closed (0x15B state 7) with PN 0x01;
  // charging = any other PN.
  static constexpr uint8_t NM_PN_DEFAULT = 0x01;
  bool bms_pn_received = false;
  uint8_t bms_pn = 0;  // 0x4E1 b7
  bool bms_closed() const;
  bool bms_driving() const;
  bool bms_charging() const;
  const char* bms_mode_label() const;
  void render_mode_html(String& html);
  // Worst-case rendered size of the BMS mode section.
  static constexpr uint32_t MODE_HTML_BUDGET = 96;

  // Drive-mode car frames (found by emulating the pack firmware, matching the
  // driving car's FD bus). The original 0x313 template came from an AC-charge
  // log: its PDU 0x000402 b5 = 0xF2 and close status b6 = 1/3/5 make the BMS
  // join the AC-charge network (0x4E1 PN 0x13) and close in charge mode. The
  // driving car sends b5 = 0xF0, b6 = 0x01 throughout. With those alone the
  // pack stays in drive mode but never closes: its drive close also needs
  // 0x169 PDU 0x010002 b3 bits 7..6 non-zero (the car sends b3 = 0x88).
  // Set false for the legacy frames (AC-charge 0x313, no 0x169).
  static constexpr bool DRIVE_MODE_FRAMES = true;
  uint8_t cnt169 = 0;  // 0x169 PDU 0x010002 counter, 0..14

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
  // - From 3.68V/cell the 5A clamp overrides the BMS's own charge limit from
  //   0x17E, which drops straight to 0 once a cell reaches ~3.70V, short of
  //   the 3.75V the snap needs. Below 3.68V a BMS cut is for some other reason
  //   and is respected. The limits below bound how long the override can last.
  // - Give up on reaching absorption unless the pack is between 5C and 45C.
  // - Give up 30 seconds after first reaching 3.75V/cell (dips back under
  //   don't restart the timer), or 15 minutes after entering absorption.
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
  static constexpr int32_t SNAP_ABSORB_MAX_S = 900;      // give up this long after entering absorption
  static constexpr int32_t SNAP_BMS_OVERRIDE_MV = 3680;  // clamp overrides the BMS limit from here
  static constexpr int32_t SNAP_MIN_TEMP_DC = 50;        // give up unless cell temps are above this...
  static constexpr int32_t SNAP_MAX_TEMP_DC = 450;       // ...and below this
  static constexpr int32_t SNAP_ABORT_MV = 3760;
  static constexpr int32_t SNAP_SKIP_MV = 3600;   // no-drift check threshold
  static constexpr int32_t SNAP_SKIP_SOC = 9800;  // skip snap if BMS SoC here
  static constexpr int32_t SNAP_RESET_MV = 3450;
  static constexpr int32_t SNAP_HOLD_MV = 3600;  // forced 100% SoC held down to here
  static constexpr int32_t SNAP_MAX_DA = 50;     // 5A max during snap
  static constexpr int32_t SNAP_MIN_DA = 30;     // 3A min during snap
  static constexpr float SNAP_EMA_ALPHA = 1.0f / 30;

  bool snapEnded = false;
  // Seconds since cell max first reached SNAP_OVER_MV in this attempt.
  int32_t snap_over_s = 0;
  // Seconds since the attempt entered absorption.
  int32_t snap_absorb_s = 0;
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

  // ---- Per-cell balancing remaining time ----
  // The BMS debug stream on 0x689 carries a 50-slot multiplex (payload byte 0
  // = 0x92, byte 1 = slot). Slot 0x0D walks the pack cells one at a time
  // (1-based pack cell number in byte 6, a new cell every 2s, ~3.5 minutes per
  // sweep) with that cell's remaining balancing time in minutes in bytes 2-3
  // (big-endian). The BMS sizes a target (up to 720 min) per cell after a rest,
  // keeps it in NvM across sleeps and counts it down while the cell bleeds;
  // 0 = nothing to balance. Confirmed by emulating the firmware: two cells
  // balancing with 12 h targets reported 719 and 718 min. BAL_NOT_SEEN until
  // that cell has been reported.
  static constexpr uint8_t BAL_SLOT = 0x0D;
  static constexpr uint8_t BAL_MAX_CELLS = 108;
  static constexpr uint16_t BAL_NOT_SEEN = 0xFFFF;
  // Worst-case rendered size: header text plus "108: 1092h15m<br>" per cell.
  static constexpr uint32_t BAL_HTML_BUDGET = 256 + BAL_MAX_CELLS * 18;

  uint16_t bal_demand[BAL_MAX_CELLS];
  // Last cell index reported on slot 0x0D (0 = none yet), to spot each sweep
  // wrapping round. The last complete sweep's total remaining minutes, cells
  // with time left and longest time, and a count of complete sweeps.
  uint8_t bal_last_cell = 0;
  uint32_t bal_last_sum = 0;
  uint16_t bal_last_nonzero = 0;
  uint16_t bal_last_max = 0;
  bool bal_have_sum = false;
  uint32_t bal_sweep_count = 0;

  void bal_sweep_complete();
  void bal_reset();

  void render_balancing_html(String& html);

  // ---- Balancing gate status (DID 0xB0E5) ----
  // 24 bytes = six big-endian 32-bit words, multiplexed: every read returns the
  // next of 6 pages (word 0 = page number 1..6), so one page arrives per poll.
  // Page 6 carries the inputs that gate new balancing decisions: word 1 = how
  // long the BMS slept before this wake (s, from its RTC; balancing needs a
  // valid sleep of 2 s .. ~1 year), word 2 = 1 with a valid sleep (0 seen
  // without one), word 5 = rest time (s) = sleep + time awake with the
  // contactors open (top-of-charge balancing needs >= 30 min). The other pages
  // are kept raw. (DID 0xB11A, polled before, is a 16-byte status from an
  // unrelated table: it did not change while the emulated BMS balanced.)
  static constexpr uint16_t BAL_STATUS_LEN = 24;
  static constexpr uint8_t BAL_STATUS_PAGES = 6;
  static constexpr uint8_t BAL_STATUS_WORDS = 5;  // after the page number
  static constexpr uint8_t BAL_GATE_PAGE = 6;
  static constexpr uint32_t BAL_MIN_SLEEP_S = 2;
  static constexpr uint32_t BAL_TOP_REST_S = 30 * 60;

  bool bal_page_seen[BAL_STATUS_PAGES] = {false};
  uint32_t bal_page[BAL_STATUS_PAGES][BAL_STATUS_WORDS] = {{0}};
  uint32_t bal_status_polls = 0;

  void record_bal_status(const uint8_t* data, uint16_t length);
  void render_bal_status_html(String& html);
  // Worst-case rendered size of the status section.
  static constexpr uint32_t BAL_STATUS_HTML_BUDGET = 768;

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

  // The BMS's own power limits from 0x17E, before our hard caps.
  uint32_t bms_max_charge_W = 0;
  uint32_t bms_max_discharge_W = 0;
  int32_t bms_limit_freshness = 0;

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
  void update_169();
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
  static const uint16_t POLL_BALANCING_STATUS = 0xB0E5;
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
  // 0x169 as the driving car sends it every 20ms, reduced to PDU 0x010002
  // (the other three PDUs in the car's 48-byte frame make no difference):
  // b0 CRC, b1 counter 0..14, b2 0x64 (13.0 V at 0.1 V + 3.0 V; the car
  // sends 12.7-14.3 V), b3 0x88 (bits 7..6 = 2 is the drive-close enable),
  // b4 0x20. update_169 patches the counter and CRC.
  CAN_frame MG4_169_FD = {.FD = true,
                          .ext_ID = false,
                          .DLC = 12,
                          .ID = 0x169,
                          .data = {0x01, 0x00, 0x02, 0x08, 0x00, 0x00, 0x64, 0x88, 0x20, 0x00, 0x00, 0x00}};
  CAN_frame MG4_314_FD = {.FD = true,
                          .ext_ID = false,
                          .DLC = 24,
                          .ID = 0x314,
                          .data = {0x00, 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xE4, 0x40, 0x56,
                                   0x00, 0x04, 0x05, 0x08, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x04, 0xE0, 0x41}};
};
