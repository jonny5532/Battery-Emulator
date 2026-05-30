#include "UdsCanBattery.h"

#include <Arduino.h>
#include "../devboard/utils/logging.h"

constexpr uint16_t UDS_TIMEOUT_CLEAR_DTC = 25;  // 2.5s (2.5s / 100ms)
constexpr uint16_t UDS_TIMEOUT_READ_DTC = 20;   // 2s (2s / 100ms)
constexpr uint16_t UDS_TIMEOUT_READ_DID = 5;    // 0.5s (0.5s / 100ms)
constexpr uint16_t UDS_TIMEOUT_CONTINUE = 5;    // 0.5s (0.5s / 100ms)

void UdsCanBattery::transmit_uds_can(unsigned long currentMillis) {
  isotp_poll();

  if (currentMillis - previousUdsMillis200 >= 100) {
    previousUdsMillis200 = currentMillis;

    if (uds_busy_timeout > 0) {
      // Still busy, do not send new requests
      uds_busy_timeout--;
      return;
    }

    if (user_request_clear_dtc) {
      sendUdsRequest(SID::ClearDiagnosticInformation, 0xFF, 0xFF, 0xFF);
      uds_busy_timeout = UDS_TIMEOUT_CLEAR_DTC;
      return;
    }

    if (user_request_read_dtc) {
      is_requesting_dtc = true;
      dtc_len = 0;

      sendUdsRequest(SID::ReadDTCInformation, 0x02, 0xFF);  // 0x02 = reportDTCByStatusMask, 0xFF = all DTCs
      uds_busy_timeout = UDS_TIMEOUT_READ_DTC;
      return;
    }

    if (next_pid == 0 && first_pid != 0) {
      // Reset PID cycle
      next_pid = first_pid;
    }

    if (next_pid) {
      // Request the next PID
      is_requesting_dtc = false;
      sendUdsRequest(SID::ReadDataByIdentifier, (next_pid >> 8) & 0xFF, next_pid & 0xFF);
      uds_busy_timeout = UDS_TIMEOUT_READ_DID;
    }
  }
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

  isotp_receive(rx.data.u8, rx.DLC, ISOTP_TATYPE_PHYSICAL);

  return true;
}

void UdsCanBattery::on_isotp_can_tx(uint32_t can_id, uint8_t* can_data, uint8_t can_dlc) {
  CAN_frame frame = {};
  frame.ID = can_id;
  frame.DLC = can_dlc;
  memcpy(frame.data.u8, can_data, can_dlc);
  transmit_can_frame(&frame);
}

void UdsCanBattery::on_isotp_rx_complete(uint8_t* data, int len, isotp_tatype tatype) {
  processUdsMessage(data, len, false);
}

static inline uint32_t parseBigEndianValue(const uint8_t* data, uint16_t length) {
  uint32_t val = 0;
  for (uint16_t i = 0; i < length && i < 4; i++) {
    val = (val << 8) | data[i];
  }
  return val;
}

void UdsCanBattery::processUdsMessage(const uint8_t* data, uint16_t len, bool cutShort) {
  uint8_t sid = data[0];

  // Transaction is finished
  uds_busy_timeout = 0;

  switch (sid) {
    case SID::ReadDataByIdentifier + 0x40: {
      uint16_t did = (data[1] << 8) | data[2];
      // Value starts at data[3]
      // Decode up to 4 bytes of value, big endian.
      uint32_t val = len > 3 ? parseBigEndianValue(&data[3], len - 3) : 0;
      // The handler returns the next PID to query.
      next_pid = handle_pid(did, val, &data[3], len - 3, cutShort ? UdsStatus::OK_SHORT : UdsStatus::OK);
      break;
    };
    case SID::ReadDTCInformation + 0x40:
      dtc_len = len;
      memcpy(dtc_buffer, data, len);
      user_request_read_dtc = false;
      break;
    case SID::ClearDiagnosticInformation + 0x40:
      user_request_clear_dtc = false;
      break;
    case 0x7F: {  // NegativeResponse
      uint8_t origSid = data[1];
      uint8_t nrc = data[2];
      logging.printf("UDS negative response to 0x%02X: NRC=0x%02X\n", origSid, nrc);
      switch (origSid) {
        case SID::ReadDataByIdentifier:
          next_pid = handle_pid(next_pid & 0xFFFF, 0, &nrc, 1, UdsStatus::NEGATIVE_RESPONSE);
          break;
        case SID::ReadDTCInformation:
          user_request_read_dtc = false;
          break;
        case SID::ClearDiagnosticInformation:
          user_request_clear_dtc = false;
          break;
      }
      break;
    }
  }
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

void UdsCanBattery::sendUdsRequest(SID service_id, uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3) {
  uint8_t payload[5];
  payload[0] = static_cast<uint8_t>(service_id);
  payload[1] = d0;
  payload[2] = d1;
  payload[3] = d2;
  payload[4] = d3;

  int len = 1;
  if (service_id == SID::ReadDataByIdentifier)
    len = 3;
  else if (service_id == SID::ReadDTCInformation)
    len = 3;
  else if (service_id == SID::ClearDiagnosticInformation)
    len = 4;

  isotp_init(uds_address);
  isotp_send(payload, len);
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

void UdsCanBattery::read_DTC() {
  user_request_read_dtc = true;
}

void UdsCanBattery::reset_DTC() {
  user_request_clear_dtc = true;
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
