#pragma once

#include "../lib/uds_isotp/isotp.h"
#include "../lib/uds_isotp/uds.h"
#include "CanBattery.h"

// Extend this class to add UDS features to a battery integration.

// 1. Call `setup_uds(uint16_t uds_address, uint16_t uds_response_address,
//    uint16_t first_pid)` in your battery's setup() function to initialize UDS
//    handling.
//     - uds_address (the CAN ID of the ECU to query, e.g. 0x7DF for generic
//       requests)
//     - uds_response_address (the CAN ID that UDS responses must come from, or
//       0 to auto-detect)
//     - first_pid (the first PID to request, e.g. 0xB042 for MG HS PHEV battery
//       voltage)
//
// 2. Call `transmit_uds_can(unsigned long currentMillis)` in your battery's
//    transmit_can() function to send UDS requests periodically.
//
// 3. Call `handle_incoming_uds_can_frame(CAN_frame rx_frame)` in your battery's
//    `handle_incoming_can_frame(CAN_frame rx_frame)` function to process
//    incoming UDS responses. If it returns true, the frame was handled as a UDS
//    response so you can ignore it.
//
// 4. Override `handle_pid(uint16_t pid, uint32_t value, const uint8_t* data,
//    uint16_t length, UdsStatus status)` to be passed PID query responses. The arguments are:
//     - pid: the PID that the response is for
//     - value: the value of the PID (big-endian, truncated to four bytes if the response is longer)
//     - data: the raw data bytes of the value
//     - length: the length of the value in bytes
//     - status: whether the response was complete, shortened, or a negative response
//    The value returned by handle_pid is used as the next PID to query. Return 0 to restart the cycle.
//

/*
class UdsCanBattery;
typedef bool (*UdsFutureCallback)(UdsCanBattery *battery, const uint8_t* data, uint16_t len);

class UdsFuture {
 public:
  UdsFuture(UdsCanBattery *battery) : battery(battery) {}
  
  void on_response(uint8_t sid, UdsFutureCallback callback) {
    this->callback = callback;
  }
 private:
  UdsCanBattery *battery;
  UdsFutureCallback callback = nullptr;
};
*/

class UdsCanBattery : public CanBattery, public IsoTp {
 public:
  UdsCanBattery(CAN_Speed speed = CAN_Speed::CAN_SPEED_500KBPS) : CanBattery(speed) {}
  UdsCanBattery(CAN_Interface interface, CAN_Speed speed = CAN_Speed::CAN_SPEED_500KBPS)
      : CanBattery(interface, speed) {}

  enum class UdsStatus : uint8_t {
    OK = 0,
    TIMEOUT,
    NEGATIVE_RESPONSE,  // The ECU returned an NRC (e.g., 0x7F)
  };

  enum class UdsAction : uint8_t {
    NONE = 0,
    READ_DTC,
    CLEAR_DTC,
    RESET_BMS,
    READ_MEMORY,
    PAUSE,
  };

  inline bool uds_is_busy() const { return pending_action != UdsAction::NONE || uds_action_cooldown > 0; }
  bool perform_uds_action(UdsAction action, int16_t max_retries, uint32_t cooldown);
  // Temporarily pause UDS requests for the specified number of 100ms ticks.
  void pause_uds(uint16_t ticks_100ms) { perform_uds_action(UdsAction::PAUSE, ticks_100ms, 0); }

  virtual bool supports_read_DTC();
  virtual bool supports_reset_DTC();
  virtual bool supports_reset_BMS();
  virtual void read_DTC();
  virtual void reset_DTC();
  virtual void reset_BMS();

  virtual void print_formatted_dtc(uint32_t dtc24, uint8_t status);

  // The range of response IDs (addresses) we'll accept UDS responses from.
  static const uint16_t MIN_UDS_RESPONSE_ID = 0x780;
  static const uint16_t MAX_UDS_RESPONSE_ID = 0x7EF;

  //static const uint32_t SHORT_PID = 0x10000;

  UdsAction pending_action = UdsAction::NONE;
  uint8_t expected_response_sid = 0;

  uint32_t previousUdsMillis100 = 0;
  uint32_t first_pid = 0;
  uint32_t next_pid = 0;
  uint32_t pending_pid = 0;
  uint32_t pid_retries = 0;
  uint32_t pid_age = 0;
  // Whether we've received a non-zero PID response yet since boot/reset.
  bool uds_started = false;
  // How many ticks left for the individual UDS transaction to complete, before
  // we time out and allow new transactions to be started.
  int16_t uds_transaction_timeout = 0;
  // How many ticks left for the overall UDS action to complete, which can
  // include multiple transactions (e.g., security access or retries)
  //int16_t uds_action_timeout = 0;

  int16_t uds_action_max_retries = 0;
  int16_t uds_action_attempts = 0;

  int16_t uds_promotion_attempts = 0;

  // How long to wait after a UDS action before we allow another one (which
  // gives time for PID cycles in the interim).
  int16_t uds_action_cooldown = 0;

  // The address we'll send UDS requests to.
  uint16_t uds_address = 0x7DF;
  // The address we require UDS responses to come from, or 0 to accept from any
  // address in the valid range.
  uint16_t uds_response_address = 0;
  // The address we are currently receiving a UDS response from.
  uint16_t uds_current_response_address = 0;

  // CAN_frame UDS_PID_REQUEST = {.FD = false,
  //                              .ext_ID = false,
  //                              .DLC = 8,
  //                              .ID = 0x7E5,
  //                              .data = {0x03, 0x22, 0xB0, 0x42, 0x00, 0x00, 0x00, 0x00}};

  // //0x781 UDS diagnostic requests - request all DTC's
  // CAN_frame UDS_RQ_DTCs = {.FD = false,
  //                          .ext_ID = false,
  //                          .DLC = 8,
  //                          .ID = 0x781,
  //                          .data = {0x03, 0x19, 0x02, 0xFF, 0x00, 0x00, 0x00, 0x00}};

  // //0x781 UDS diagnostic requests - clear all DTC's
  // CAN_frame UDS_CLEAR_DTCs = {.FD = false,
  //                             .ext_ID = false,
  //                             .DLC = 8,
  //                             .ID = 0x781,
  //                             .data = {0x04, 0x14, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00}};

  // CAN_frame UDS_RQ_CONTINUE_MULTIFRAME = {.FD = false,
  //                                         .ext_ID = false,
  //                                         .DLC = 8,
  //                                         .ID = 0x781,
  //                                         .data = {0x30, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00}};

  std::pair<int, uint8_t*> getUdsResponse() { return {dtc_len, dtc_buffer}; }
  String getDtcScript();

 protected:
  void setup_uds(uint16_t uds_address, uint16_t uds_response_address, uint32_t first_pid = 0);

  void transmit_uds_can(unsigned long currentMillis);
  bool handle_incoming_uds_can_frame(CAN_frame rx_frame);

  // Called by the ISO-TP layer to emit CAN frames or notify of complete UDS responses.
  virtual void on_isotp_can_tx(uint32_t can_id, const uint8_t* can_data, uint8_t can_dlc) override;
  virtual void on_isotp_rx_complete(const uint8_t* data, int len, isotp_tatype tatype) override;

  bool transmit_uds_action();
  void on_uds_action_complete(uint8_t sid, const uint8_t* data, uint16_t len);

  // Subclasses can override these to add their own custom UDS requests and responses.
  virtual bool transmit_custom_uds() { return false; }
  virtual bool on_custom_uds_response(uint8_t sid, const uint8_t* data, uint16_t len) { return false; }

  // Subclasses can override these to modify or disable the default PID scanning behavior.
  virtual bool transmit_uds_pid_scan();
  virtual bool on_uds_pid_scan_response(uint8_t sid, const uint8_t* data, uint16_t len);
  virtual void on_uds_pid_scan_timeout();

  virtual int32_t calculate_uds_security_key(uint16_t seed) { return -1; }

  // If you let UdsCanBattery handle UDS responses, you can override this be
  // passed the PID query responses. The value returned is used as the next PID
  // to query. Return 0 to let the PID cycle restart.
  virtual uint32_t handle_pid(uint16_t pid, uint32_t value, const uint8_t* data, uint16_t length, UdsStatus status) {
    return 0;
  }

  void uds_send(SID service_id, const std::string_view data, uint32_t timeout = 0);
  inline void uds_send(SID service_id, const std::initializer_list<uint8_t> data, uint32_t timeout = 0) {
    uds_send(service_id, std::string_view((const char*)data.begin(), data.size()), timeout);
  }

 private:
  // Put received DTCs in a different buffer
  uint8_t dtc_buffer[1024];
  uint16_t dtc_len = 0;

  bool transaction_tick();

  void on_uds_receive(const uint8_t* data, uint16_t len);
  void handleDtcResponse(const uint8_t* data, uint16_t len);
};
