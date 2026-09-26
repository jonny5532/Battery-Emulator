#pragma once

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

  String get_uds_info_html() override;
  const char* get_dtc_json_filename() override { return "mg_dtc.json"; }

 private:
  static const uint16_t MAX_CELL_DEVIATION_LFP_MV = 400;
  static const uint16_t MAX_CELL_DEVIATION_NMC_MV = 150;

  static const uint16_t WORKING_MAX_MARGIN_MV = 10;
  static const uint16_t WORKING_MIN_MARGIN_MV = 300;

  static const uint16_t MAX_CELL_VOLTAGE_LFP_MV = 3750 + WORKING_MAX_MARGIN_MV;
  static const uint16_t MIN_CELL_VOLTAGE_LFP_MV = 2500;
  static const uint16_t MAX_CELL_VOLTAGE_NMC_MV = 4200 + WORKING_MAX_MARGIN_MV;
  static const uint16_t MIN_CELL_VOLTAGE_NMC_MV = 2700;

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
  bool batteryIdentified = false;
  // Pack serial (NTSC identifier) from 0x308 subfield 000554: 7 ASCII bytes
  // + 1 index byte per frame over 4 frames; FF-padded tail, NUL-terminated.
  // Eg: 0AFPEG10879103D7N3000095
  char ntsc_serial[29] = {0};
  bool coulombCounting = false;
  ContactorState contactorState = ContactorState::WAITING_FOR_PACK;
  PackContactorFeedback pack_contactors;
  unsigned long contactorWaitStartMillis = 0;  // Grace timer base while WAITING_FOR_PACK
  int replayFrameIndex047_08A = 0;             // Master cursor through the message cycle; the
                                               // 313/314 index is derived from it (see cpp)
  int wakeupCounter = 0;                       // Paces the 0x4F3 FD wakeup keep-alive

  void identify_battery();

  void refresh_047_08a(int i);
  void refresh_313_314(int i);

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

  static const uint16_t POLL_BATTERY_VOLTAGE = 0xB042;
  static const uint16_t POLL_BATTERY_CURRENT = 0xB043;
  static const uint16_t POLL_BATTERY_SOC = 0xB046;
  static const uint16_t POLL_MIN_CELL_TEMPERATURE = 0xB057;
  static const uint16_t POLL_MAX_CELL_TEMPERATURE = 0xB056;
  static const uint16_t POLL_BATTERY_SOH = 0xB061;
  static const uint16_t POLL_ECU_HARDWARE_NUMBER = 0xF192;
  static const uint16_t POLL_ECU_SOFTWARE_NUMBER = 0xF194;

  // ECU identifier payloads (10 ASCII characters each)
  uint8_t pid_ecu_hw_number[10] = {0};
  uint8_t pid_ecu_sw_number[10] = {0};

  CAN_frame MG4_4F3_FD = {.FD = true,
                          .ext_ID = false,
                          .DLC = 8,
                          .ID = 0x4F3,
                          .data = {0xF3, 0x10, 0x48, 0x00, 0xFF, 0xFF, 0x00, 0x11}};
  // Static templates for the generated frames. refresh_047_08a /
  // refresh_313_314 patch only the dynamic bytes (counters, RLE steps,
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
