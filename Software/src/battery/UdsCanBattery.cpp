#include "UdsCanBattery.h"

#include <Arduino.h>
#include "../devboard/utils/logging.h"

// Timeouts (to wait for a UDS response) in 100ms ticks
constexpr uint16_t UDS_TIMEOUT_CLEAR_DTC = 25;        // 2.5s (2.5s / 100ms)
constexpr uint16_t UDS_TIMEOUT_READ_DTC = 20;         // 2s (2s / 100ms)
constexpr uint16_t UDS_TIMEOUT_READ_DID = 5;          // 0.5s (0.5s / 100ms)
constexpr uint16_t UDS_TIMEOUT_CONTINUE = 5;          // 0.5s (0.5s / 100ms)
constexpr uint16_t UDS_TIMEOUT_SESSION_CONTROL = 10;  // 1s (1s / 100ms)
constexpr uint16_t UDS_TIMEOUT_RESET = 50;            // 2s (2s / 100ms)
constexpr uint16_t UDS_DEFAULT_ACTION_COOLDOWN = 10;  // 2s (2s / 100ms)

void UdsCanBattery::transmit_uds_can(unsigned long currentMillis) {
  // Called during the CAN transmit phase.

  // Poll the underlying ISO-TP layer.
  isotp_poll();

  // Check if it is time to do the UDS tick (every 100ms)
  if (currentMillis - previousUdsMillis100 < INTERVAL_100_MS) {
    return;
  }
  previousUdsMillis100 = currentMillis;

  if (uds_transaction_timeout > 0) {
    // Still busy, do not send new requests
    uds_transaction_timeout--;
    return;
  }

  if (isotp_is_busy()) {
    // ISO-TP transaction in progress, wait for it to finish before sending new requests
    return;
  }

  if (transmit_uds_action()) {
    // There's an active action in progress.
    return;
  } else {
    // No action active, do the PID cycle.
    if (next_pid == 0) {
      // Reset PID cycle
      next_pid = first_pid;
    }

    if (next_pid) {
      // Request the next PID
      uds_send(SID::ReadDataByIdentifier, {(uint8_t)((next_pid >> 8) & 0xFF), (uint8_t)(next_pid & 0xFF)},
               UDS_TIMEOUT_READ_DID);
    }
  }
}

bool UdsCanBattery::transmit_uds_action() {
  // Called during the CAN transmit phase, if there is no current UDS
  // transaction in progress. Will progress the current action (or end it if it
  // has timed out). Returns true if it did something, false if there is no action to progress.

  if (uds_action_timeout > 0) {
    // We're mid-action.
    uds_action_timeout--;
  } else if (pending_action != UdsAction::NONE) {
    // Action timed out.
    logging.println("UDS action timed out.");
    pending_action = UdsAction::NONE;
  }

  switch (pending_action) {
    case UdsAction::READ_DTC:
      uds_send(SID::ReadDTCInformation, {0x02, 0xff}, UDS_TIMEOUT_READ_DTC);
      expected_response_sid = (uint8_t)SID::ReadDTCInformation + 0x40;
      return true;
    case UdsAction::CLEAR_DTC:
      uds_send(SID::ClearDiagnosticInformation, {0xff, 0xff, 0xff}, UDS_TIMEOUT_CLEAR_DTC);
      expected_response_sid = (uint8_t)SID::ClearDiagnosticInformation + 0x40;
      return true;
    case UdsAction::RESET_BMS:
      uds_send(SID::ECUReset, {0x01}, UDS_TIMEOUT_RESET);
      expected_response_sid = (uint8_t)SID::ECUReset + 0x40;
      return true;
    default:
      break;
  }

  if (uds_action_cooldown > 0) {
    // During post-action cooldown
    uds_action_cooldown--;
  }

  // Not within an action right now
  return false;
}

void UdsCanBattery::print_formatted_dtc(uint32_t dtc24, uint8_t status) {
  // DTC bytes: A B C (24 bits). SAE letter from top 2 bits of A.
  uint8_t A = (dtc24 >> 16) & 0xFF;
  uint8_t B = (dtc24 >> 8) & 0xFF;
  // uint8_t C =  dtc24        & 0xFF; // often a failure-type byte; keep if you need it

  const char sysMap[4] = {'P', 'C', 'B', 'U'};
  char sys = sysMap[(A & 0xC0) >> 6];

  // Four digits: D1 D2 D3 D4 from the remaining nibbles of A and B
  uint8_t d1 = (A & 0x30) >> 4;
  uint8_t d2 = (A & 0x0F);
  uint8_t d3 = (B & 0xF0) >> 4;
  uint8_t d4 = (B & 0x0F);

  logging.printf("DTC %c%X%X%X%X  status=0x%X [", sys, d1, d2, d3, d4, status);

  static constexpr struct {
    uint8_t bit;
    const char* label;
  } statusFlags[] = {
      {0x08, "Confirmed"}, {0x04, "Pending"},           {0x20, "FailSinceClear"},
      {0x01, "Fail"},      {0x10, "NotCompSinceClear"}, {0x40, "NotCompThisCycle"},
      {0x80, "MIL"},       {0x02, "FailThisCycle"},
  };

  bool first = true;
  for (size_t i = 0; i < sizeof(statusFlags) / sizeof(statusFlags[0]); i++) {
    if (status & statusFlags[i].bit) {
      if (!first) {
        logging.print(", ");
      }
      first = false;
      logging.print(statusFlags[i].label);
    }
  }

  if (first)
    logging.print("NoFlags");
  logging.println("]");
}

bool UdsCanBattery::handle_incoming_uds_can_frame(CAN_frame rx) {
  if (uds_response_address > 0) {
    if (rx.ID != uds_response_address) {
      // Not from the address we're currently accepting responses from, ignore
      return false;
    }
  } else if (rx.ID < MIN_UDS_RESPONSE_ID || rx.ID > MAX_UDS_RESPONSE_ID) {
    return false;
  }

  // Record the address it's coming from.
  uds_current_response_address = rx.ID;

  // Pass down to the ISO-TP layer for reassembly.
  isotp_receive(rx.data.u8, rx.DLC, ISOTP_TATYPE_PHYSICAL);

  return true;
}

void UdsCanBattery::on_isotp_can_tx(uint32_t can_id, const uint8_t* can_data, uint8_t can_dlc) {
  // This is called by isotp_poll() from transmit_uds_can(..)
  CAN_frame frame = {};
  frame.ID = can_id;
  frame.DLC = can_dlc;
  memcpy(frame.data.u8, can_data, can_dlc);
  transmit_can_frame(&frame);
}

void UdsCanBattery::on_isotp_rx_complete(const uint8_t* data, int len, isotp_tatype tatype) {
  uds_receive(data, len);
}

static inline uint32_t parseBigEndianValue(const uint8_t* data, uint16_t length) {
  uint32_t val = 0;
  for (uint16_t i = 0; i < length && i < 4; i++) {
    val = (val << 8) | data[i];
  }
  return val;
}

void UdsCanBattery::uds_receive(const uint8_t* data, uint16_t len) {
  // We've received a complete UDS response message.

  logging.printf("UDS Rx: ");
  for (uint16_t i = 0; i < len; i++) {
    logging.printf("%02X ", data[i]);
  }
  logging.println();

  // The current transaction is now finished
  uds_transaction_timeout = 0;

  const SID sid = (SID)data[0];

  if (pending_action == UdsAction::NONE) {
    // There's no pending action.

    if (sid == (SID::ReadDataByIdentifier + 0x40)) {
      // This is a normal PID response, pass it to the handler
      uint16_t did = (data[1] << 8) | data[2];
      // Value starts at data[3]
      // Decode up to 4 bytes of value, big endian.
      uint32_t val = len > 3 ? parseBigEndianValue(&data[3], len - 3) : 0;
      // The handler returns the next PID to query.
      next_pid = handle_pid(did, val, &data[3], len - 3, UdsStatus::OK);
    } else if (sid == SID::NegativeResponse && len >= 3 && data[1] == (uint8_t)SID::ReadDataByIdentifier) {
      // This is a negative response to a PID request
      union {
        uint32_t u32;
        uint8_t u8[4];
      } val = {};
      next_pid = handle_pid(next_pid, val.u32, val.u8, 4, UdsStatus::NEGATIVE_RESPONSE);
    }

    return;
  }

  if (sid == SID::NegativeResponse) {
    SID origSid = (SID)data[1];
    uint8_t nrc = data[2];

    switch (nrc) {
        //case NegativeResponseCode::SecurityAccessDenied:
        // ?
        //logging.printf("UDS response pending for 0x%02X\n", origSid);
      //  break;
      case NegativeResponseCode::ServiceNotSupportedInActiveSession:
        logging.printf("UDS service 0x%02X not supported in current session, trying to enter extended session\n",
                       origSid);
        uds_send(SID::DiagnosticSessionControl, {Session::ExtendedSession}, UDS_TIMEOUT_SESSION_CONTROL);
        return;
      default:
        logging.printf("UDS negative response to 0x%02X: NRC=0x%02X\n", origSid, nrc);
        break;
    }
  }

  if (sid != (SID)expected_response_sid) {
    logging.printf("Received unexpected UDS response SID 0x%02X (expected 0x%02X), ignoring\n", (uint8_t)sid,
                   expected_response_sid);
    return;
  }
  expected_response_sid = 0;

  on_uds_action_complete(sid, data, len);
}

void UdsCanBattery::on_uds_action_complete(uint8_t response_sid, const uint8_t* data, uint16_t len) {
  switch (response_sid) {
    case (uint8_t)SID::ReadDTCInformation + 0x40:
      memcpy(dtc_buffer, data, len);
      dtc_len = len;
      handleDtcResponse(data, len);
      break;
    default:
      logging.printf("Successful SID response 0x%02X\n", (uint8_t)response_sid);
      break;
  }

  pending_action = UdsAction::NONE;
  uds_action_timeout = 0;
}

bool UdsCanBattery::perform_uds_action(UdsAction action, uint32_t timeout, uint32_t cooldown) {
  // Set the pending action and timeout. The transmit_uds_can function will then
  // trigger this action (and keep retrying until it completes or the timeout
  // expires).

  if (pending_action != UdsAction::NONE) {
    // Already an action in progress, ignore this request
    return false;
  }

  if (uds_action_cooldown > 0) {
    // Can't start a new action while we're still in cooldown from the last one
    return false;
  }

  pending_action = action;
  uds_action_timeout = timeout;
  uds_action_cooldown = cooldown;

  return true;
}

void UdsCanBattery::handleDtcResponse(const uint8_t* data, uint16_t len) {
  if (len < 2)
    return;

  uint8_t subFunc = data[0];
  if (subFunc != 0x02)
    return;  // We only handle "Report DTC by Status Mask" here

  logging.printf("UDS DTC list (%d bytes of data)\n", len - 1);

  // entries are 3-byte DTC + 1-byte status
  for (size_t i = 1; i + 4 <= len; i += 4) {
    uint32_t dtc = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    uint8_t status = data[i + 3];

    print_formatted_dtc(dtc, status);
  }
}

// Low level UDS send

void UdsCanBattery::uds_send(SID service_id, const std::string_view data, uint32_t timeout) {
  uint8_t payload[256];
  payload[0] = static_cast<uint8_t>(service_id);
  memcpy(&payload[1], data.data(), data.size());

  isotp_init(uds_address);
  isotp_send(payload, data.size() + 1);

  uds_transaction_timeout = timeout;
}

void UdsCanBattery::setup_uds(uint16_t uds_address, uint16_t uds_response_address, uint32_t first_pid) {
  this->uds_address = uds_address;
  this->uds_response_address = uds_response_address;
  this->first_pid = first_pid;
  this->next_pid = first_pid;
}

bool UdsCanBattery::supports_read_DTC() {
  return true;
}

bool UdsCanBattery::supports_reset_DTC() {
  return true;
}

bool UdsCanBattery::supports_reset_BMS() {
  return true;
}

void UdsCanBattery::read_DTC() {
  perform_uds_action(UdsAction::READ_DTC, 30, UDS_DEFAULT_ACTION_COOLDOWN);
}

void UdsCanBattery::reset_DTC() {
  perform_uds_action(UdsAction::CLEAR_DTC, 30, UDS_DEFAULT_ACTION_COOLDOWN);
}

void UdsCanBattery::reset_BMS() {
  perform_uds_action(UdsAction::RESET_BMS, 30, 20);
}

String UdsCanBattery::getDtcScript() {
  String ret;
  ret.reserve(500 + dtc_len * 4);

  // dtc_buffer[0] contains the response SID (0x59)
  if (dtc_len > 1 && dtc_buffer[0] == 0x59 && dtc_buffer[1] == 0x02) {
    char buf[32];
    ret += "<div></div><script>(()=>{var uds = [";
    for (int i = 1; i < dtc_len; i++) {
      snprintf(buf, sizeof(buf), "%u,", dtc_buffer[i]);
      ret += buf;
    }
    ret +=
        "];\n"
        "var h='<table>';\n"
        "for(let i=2;i<uds.length;i+=4){\n"
        "  let a=uds[i],b=uds[i+1],c=uds[i+2],s=uds[i+3],f=[];\n"
        "  [[8,'<b>CONFIRMED</b>'],[4,'Pending'],[32,'FailSinceClear'],[1,'<b>FAIL</b>'],   "
        "[16,'NotCompSinceClear'],[64,'NotCompThisCycle'],[128,'MIL'],[2,'FailThisCycle']]  "
        ".map(x=>{if(s&x[0])f.push(x[1])});\n"
        "  let z=(v)=>v.toString(16).padStart(2,'0');\n"
        "  let d='PCBU'[a>>6]+z(a&63)+z(b)+(c?'-'+z(c):'');\n"
        "  h+=`<tr><td><b>${d.toUpperCase()}</b></td><td>${f.join(', ')||'NoFlags'}</td></tr>`;\n"
        "}\n"
        "document.currentScript.previousElementSibling.innerHTML = h+'</table>';\n"
        "})();</script>\n";
  }

  return ret;
}
