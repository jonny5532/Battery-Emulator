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

class UdsCanBattery : public CanBattery, public IsoTp {
 public:
  UdsCanBattery(CAN_Speed speed = CAN_Speed::CAN_SPEED_500KBPS) : CanBattery(speed) {}
  UdsCanBattery(CAN_Interface interface, CAN_Speed speed = CAN_Speed::CAN_SPEED_500KBPS)
      : CanBattery(interface, speed) {}

  enum class UdsStatus : uint8_t {
    OK = 0,
    OK_SHORT,
    TIMEOUT,
    NEGATIVE_RESPONSE,  // The ECU returned an NRC (e.g., 0x7F)
  };

  void setup_uds(uint16_t uds_address, uint16_t uds_response_address, uint32_t first_pid = 0);
  void transmit_uds_can(unsigned long currentMillis);
  bool handle_incoming_uds_can_frame(CAN_frame rx_frame);

 protected:
  /** Called by the protocol layer when it needs to emit a raw CAN frame. */
  virtual void on_isotp_can_tx(uint32_t can_id, uint8_t* can_data, uint8_t can_dlc) override;

  /** Called when a complete ISO-TP message has been assembled. */
  virtual void on_isotp_rx_complete(uint8_t* data, int len, isotp_tatype tatype) override;

 public:
  // Temporarily pause UDS requests for the specified number of 200ms ticks.
  void pause_uds(uint16_t ticks_200ms) { uds_busy_timeout = ticks_200ms; }
  // If you let UdsCanBattery handle UDS responses, you can override this be
  // passed the PID query responses. The value returned is used as the next PID
  // to query. Return 0 to let the PID cycle continue as normal.
  virtual uint32_t handle_pid(uint16_t pid, uint32_t value, const uint8_t* data, uint16_t length, UdsStatus status) {
    return 0;
  }
  //virtual uint32_t handle_long_pid(uint16_t pid, const uint8_t* data, uint16_t length) { return 0; }
  virtual bool supports_read_DTC();
  virtual bool supports_reset_DTC();
  virtual void read_DTC();
  virtual void reset_DTC();

  // void startUDSMultiFrameReception(uint16_t totalLength, uint8_t moduleID);
  // bool storeUDSPayload(const uint8_t* payload, uint8_t length);
  // bool isUDSMessageComplete();
  virtual void print_formatted_dtc(uint32_t dtc24, uint8_t status);

  // The range of response IDs (addresses) we'll accept UDS responses from.
  static const uint16_t MIN_UDS_RESPONSE_ID = 0x780;
  static const uint16_t MAX_UDS_RESPONSE_ID = 0x7EF;

  static const uint32_t SHORT_PID = 0x10000;

  uint32_t previousUdsMillis200 = 0;
  uint32_t first_pid = 0;
  uint32_t next_pid = 0;
  uint16_t uds_busy_timeout = 0;
  // The address we'll send UDS requests to.
  uint16_t uds_address = 0x7DF;
  // The address we require UDS responses to come from, or 0 to accept from any
  // address in the valid range.
  uint16_t uds_response_address = 0;
  // The address we are currently receiving a UDS response from.
  uint16_t uds_current_response_address = 0;

  bool user_request_read_dtc = false;
  bool user_request_clear_dtc = false;

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

 private:
  // Normal UDS rx buffer
  uint8_t uds_rx_buffer[128];
  // Put received DTCs in a different buffer
  uint8_t dtc_buffer[1024];
  uint16_t dtc_len = 0;

  bool is_requesting_dtc = false;

  void sendUdsRequest(SID service_id, uint8_t d0 = 0, uint8_t d1 = 0, uint8_t d2 = 0, uint8_t d3 = 0);
  void processUdsMessage(const uint8_t* data, uint16_t len, bool cutShort);
  void handleDtcResponse(const uint8_t* data, uint16_t len);
};
