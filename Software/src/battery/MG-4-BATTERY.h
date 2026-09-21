#pragma once

#include <vector>

#include "UdsCanBattery.h"

class Mg4Battery : public UdsCanBattery {
 public:
  virtual void setup(void);
  virtual void handle_incoming_can_frame(CAN_frame rx_frame);
  virtual uint16_t handle_pid(uint16_t pid, uint32_t value, const uint8_t* data, uint16_t length);
  virtual void update_values();
  virtual void transmit_can(unsigned long currentMillis);
  virtual uint32_t calculate_max_discharge_power_W();
  virtual uint32_t calculate_max_charge_power_W();
  //virtual uint32_t calculate_pack_voltage_limit_max_dV();

  static constexpr const char* Name = "MG4 battery";

  // ---- Background DID sweep ----
  // The exact DID map of this BMS is unknown (the MG-GEN1 identifier DIDs
  // don't work here), so handle_pid() alternately returns 0 (continue the
  // runtime poll list, so the pack keeps operating normally) and the next
  // untried DID as an out-of-sequence detour request, gently sweeping the
  // 0xB000-0xB0FF and 0xF100-0xF1FF DID ranges in the background (the same
  // ranges MG-GEN1 uses; the gap in between is skipped). Every DID that
  // returns data (no negative response / timeout) is recorded and shown on
  // the battery info page in the usual alpha-or-hex format.
  // Max payload bytes kept per DID that answered.
  static constexpr uint8_t DID_SWEEP_MAX_DATA_LEN = 32;
  // Maximum number of answering DIDs we keep.
  static constexpr uint16_t DID_SWEEP_MAX_RESULTS = 192;
  // The result storage grows in chunks of this many entries (one allocation
  // per chunk, first one on first use), up to the maximum. Never reallocated
  // beyond that (the firmware builds without exceptions, so an out-of-memory
  // growth would abort). Further answering DIDs are dropped.
  static constexpr uint16_t DID_SWEEP_RESERVE_CHUNK = 32;
  // Number of results per HTML column on the info page.
  static constexpr uint8_t DID_SWEEP_RESULTS_PER_COLUMN = 32;

  // One answering DID recorded during the sweep.
  struct DidSweepResult {
    uint16_t did = 0;  // the DID that answered
    uint8_t len = 0;   // number of payload bytes in data
    uint8_t data[DID_SWEEP_MAX_DATA_LEN] = {0};
  };

  String get_uds_info_html() override;
  const char* get_dtc_json_filename() override { return "mg_dtc.json"; }

 private:
  static const uint16_t MAX_CELL_DEVIATION_MV = 150;

  int32_t working_cell_min_mV = 0;
  int32_t working_cell_recharge_threshold_mV = 0;
  int32_t working_cell_max_mV = 0;
  // Latched flags for cell-voltage hysteresis (persist between update_values calls)
  bool voltageAtCellMax = false;
  bool voltageAtCellMin = false;
  int32_t cell_voltage_freshness = 0;
  int32_t soc_freshness = 0;
  int32_t temp_freshness = 0;

  int16_t module_temperatures_dC[12] = {0};
  int16_t module_temps_received = 0;

  // For monitoring the actual battery-reported contactor state.
  struct PackContactorFeedback {
    bool received = false;
    uint8_t state = 0xFF;  // 0xFF = no data yet
    bool isClosed() const { return received && state == 7; }
    bool isPrecharging() const { return received && state == 11; }
    // Value for datalayer.system.status.contactors_engaged (shown on the main
    // BE contactor widget): 1=closed, 3=precharge active, 0=otherwise.
    uint8_t contactsEngaged() const {
      if (isClosed()) {
        return 1;
      }
      if (isPrecharging()) {
        return 3;
      }
      return 0;
    }
    const char* label() const {
      if (!received) {
        return "No data received yet";
      }
      switch (state) {
        case 7:
          return "Closed / charging";
        case 11:
          return "Precharge active";
        case 3:
          return "Idle";
        default:
          return "Unknown";
      }
    }
    const char* color() const {
      if (!received) {
        return "#9e9e9e";  // Grey
      }
      switch (state) {
        case 7:
          return "#4CAF50";  // Green
        case 11:
          return "#ff9800";  // Orange
        case 3:
          return "#f44336";  // Red
        default:
          return "#9e9e9e";  // Grey
      }
    }
  };

  // Contactor management state machine.
  enum class ContactorState {
    WAITING_FOR_PACK,  // Silent: waiting for the first 0x15B state (or grace expiry)
    CLOSING,           // Replaying the full message cycle from index 0
    CLOSED,            // Pack confirmed closed, replaying the end of the cycle
    OPENING,           // Open requested, replaying the start of the cycle
  };

  bool reportsFDVoltages = false;
  bool reportsSoC = false;
  bool coulombCounting = false;
  ContactorState contactorState = ContactorState::WAITING_FOR_PACK;
  PackContactorFeedback pack_contactors;
  int replayFrameIndex047_08A = 0;  // Master cursor through the message cycle; the
                                    // 313/314/315 index is derived from it (see cpp)
  int wakeupCounter = 0;            // Paces the 0x4F3 FD wakeup keep-alive

  void contactor_state_tick(unsigned long currentMillis);

  uint32_t total_discharge_dC = 0;  // in deci-Coulombs
  bool total_discharge_initialized = false;

  unsigned long lastTickMillis = 0;

  unsigned long previousMillis10 = 0;   // will store last time a 10ms CAN Message was send
  unsigned long previousMillis100 = 0;  // will store last time a 100ms CAN Message was send
  unsigned long previousMillis200 = 0;  // will store last time a 200ms CAN Message was send

  uint32_t* nonvolatile_cookie = 0;
  uint32_t* nonvolatile_total_discharge_dC = 0;
  static const uint32_t NONVOLATILE_COOKIE_VALUE = 0x7734b1f5;

  // ---- Background DID sweep state ----
  // Swept DID ranges: 0xB000-0xB0FF followed by 0xF100-0xF1FF (512 DIDs
  // total, the gap in between is skipped).
  static constexpr uint16_t DID_SWEEP_FIRST_B0 = 0xB000;
  static constexpr uint16_t DID_SWEEP_LAST_B0 = 0xB0FF;
  static constexpr uint16_t DID_SWEEP_FIRST_F1 = 0xF100;
  static constexpr uint16_t DID_SWEEP_LAST_F1 = 0xF1FF;
  static constexpr uint16_t DID_SWEEP_TOTAL = 512;

  // DIDs that answered, in ascending order (i.e. hand-out order). Grows in
  // DID_SWEEP_RESERVE_CHUNK-entry allocations up to DID_SWEEP_MAX_RESULTS;
  // empty (no RAM used) until the first DID answers.
  std::vector<DidSweepResult> did_sweep_results;
  // The next DID to hand out as a detour request. DIDs are handed out exactly
  // once: the sweep starts at 0xB000, hops from 0xB0FF to 0xF100, and stops
  // (did_sweep_active = false) once 0xF1FF has been handed out, so no DID is
  // ever re-added.
  uint16_t did_sweep_next = DID_SWEEP_FIRST_B0;
  // Cleared once every DID in the ranges has been handed out once.
  bool did_sweep_active = true;
  // Alternates handle_pid() return values between 0 (runtime poll list
  // continues) and the next sweep DID (detour request).
  bool did_sweep_toggle = false;

  void did_sweep_advance();
  void record_did_sweep_result(uint16_t did, const uint8_t* data, uint16_t length);

  static const uint16_t POLL_BATTERY_VOLTAGE = 0xB042;
  static const uint16_t POLL_BATTERY_CURRENT = 0xB043;
  static const uint16_t POLL_BATTERY_SOC = 0xB046;
  static const uint16_t POLL_MIN_CELL_TEMPERATURE = 0xB057;
  static const uint16_t POLL_MAX_CELL_TEMPERATURE = 0xB056;
  static const uint16_t POLL_BATTERY_SOH = 0xB061;

  // PIDs read regularly
  static constexpr uint16_t UDS_STEADY_PID_LIST[] = {POLL_BATTERY_SOH, POLL_BATTERY_VOLTAGE, POLL_MIN_CELL_TEMPERATURE,
                                                     POLL_MAX_CELL_TEMPERATURE};

  CAN_frame MG4_4F3_FD = {.FD = true,
                          .ext_ID = false,
                          .DLC = 8,
                          .ID = 0x4F3,
                          .data = {0xF3, 0x10, 0x48, 0x00, 0xFF, 0xFF, 0x00, 0x11}};
  // 0x047 (FD), 0x08A, 0x313, 0x314 and 0x315 are all populated at runtime
  // by concise generators (see MG-4-BATTERY.cpp) rather than by replaying
  // a long captured table.
  CAN_frame MG4_047_FD = {.FD = true,
                          .ext_ID = false,
                          .DLC = 24,
                          .ID = 0x047,
                          .data = {0x00, 0x01, 0x27, 0x08, 0xF4, 0xF0, 0x80, 0x04, 0x00, 0x5F, 0x3C, 0x00,
                                   0x00, 0x01, 0x48, 0x08, 0x61, 0xF0, 0x6A, 0x06, 0xA0, 0xFF, 0xF0, 0xFF}};
  CAN_frame MG4_08A_FD = {.FD = true, .ext_ID = false, .DLC = 48, .ID = 0x08A, .data = {0}};
  CAN_frame MG4_313_FD = {.FD = true, .ext_ID = false, .DLC = 48, .ID = 0x313, .data = {0}};
  CAN_frame MG4_314_FD = {.FD = true, .ext_ID = false, .DLC = 24, .ID = 0x314, .data = {0}};
  CAN_frame MG4_315_FD = {.FD = true, .ext_ID = false, .DLC = 48, .ID = 0x315, .data = {0}};
};
