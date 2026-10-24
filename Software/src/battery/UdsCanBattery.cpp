#include "UdsCanBattery.h"

#include <Arduino.h>
#include "../devboard/utils/events.h"
#include "../devboard/utils/logging.h"

// Timeouts (to wait for a UDS response) in 100ms ticks
constexpr uint16_t UDS_TIMEOUT_SESSION_CONTROL = 10;
constexpr uint16_t UDS_TIMEOUT_CLEAR_DTC = 25;
constexpr uint16_t UDS_TIMEOUT_READ_DTC = 20;
constexpr uint16_t UDS_TIMEOUT_READ_DID = 2;
constexpr uint16_t UDS_TIMEOUT_SECURITY = 20;
// The ECU answers an ECUReset before it resets, so a short wait is enough.
constexpr uint16_t UDS_TIMEOUT_RESET = 10;
// How long to ignore CAN errors after a reset, while the BMS restarts.
constexpr uint32_t UDS_RESET_IGNORE_CAN_ERRORS_MS = 10000;
// How many times to retry a particular PID read before giving up.
constexpr uint16_t UDS_PID_MAX_RETRIES = 10;
// Traffic on 0x7DF (or on uds_address) that isn't ours implies an external
// diagnostic tool is in use.
constexpr uint16_t OBD2_REQUEST_ADDRESS = 0x7DF;
// The range of response IDs (addresses) we'll accept UDS responses from if auto-detecting.
constexpr uint32_t MIN_UDS_RESPONSE_ID = 0x780;
constexpr uint32_t MAX_UDS_RESPONSE_ID = 0x7EF;
// KWP2000 service for one-byte local identifier reads - not part of ISO 14229
// UDS proper, but similar enough.
constexpr uint8_t UDS_SID_READ_LOCAL_IDENTIFIER = 0x21;
// How long to back off after detecting another diagnostic tool on the bus.
constexpr uint16_t UDS_EXTERNAL_TOOL_BACKOFF_TICKS = 50;  // 5 seconds
// How long to hold off sequences after a failed unlock, so callers retrying
// straight away can't run up the ECU's failed-attempt counter (NRC 0x36) and
// any required delay (NRC 0x37) has time to expire.
constexpr uint16_t UDS_SECURITY_FAIL_BACKOFF_TICKS = 100;  // 10 seconds
// Largest key we can send: the sequence buffer less the sendKey sub-function.
constexpr uint8_t UDS_SECURITY_MAX_KEY_LEN = 15;

//#define UDS_DEBUG 1

void UdsCanBattery::transmit_uds_can(unsigned long currentMillis) {
  // Called from batteries' own transmit_can() methods.

  // Poll the underlying ISO-TP layer (at the native ~1KHz rate).
  isotp_poll();

  // Otherwise we do UDS operations every 100ms.
  if (currentMillis - previousUdsMillis100 < INTERVAL_100_MS) {
    return;
  }
  previousUdsMillis100 = currentMillis;

  if (reset_holdoff_ticks > 0) {
    reset_holdoff_ticks--;
  }

  if (seq_wait_ticks > 0) {
    seq_wait_ticks--;
  }

  if (seq_pause_ticks > 0) {
    // We're in a pause period (eg. after a BMS reset, or while an external
    // tool is using the bus); count it down. New sends are gated by priority
    // at the send points (send_sequence_message / transmit_uds_pid_scan),
    // while in-flight transactions keep being ticked normally.
    seq_pause_ticks--;
  }

  if (transaction_tick()) {
    // A request is still in flight (sequence step or PID request), bail.
    return;
  }

  if (isotp_is_busy()) {
    // ISO-TP transaction in progress, wait for it to finish before sending new requests.
    return;
  }

  if (seq_state != UDS_STATE_IDLE) {
    // A sequence is active but nothing is awaiting a response right now: hold
    // the bus so PID scanning doesn't get in the way.
    return;
  }

  if (seq_wait_state != UDS_STATE_IDLE) {
    // A sequence is waiting between steps. Queued sequences stay queued until
    // it finishes, but the PID scan carries on if the pause allows it.
    if (seq_wait_ticks == 0 && !uds_paused_for(UdsPriority::Sequence)) {
      // The wait is over: continue the sequence.
      const uint16_t state = seq_wait_state;
      seq_wait_state = UDS_STATE_IDLE;
      handle_sequence(state, 0, nullptr, 0);
      return;
    }
  } else if (pending_seq_state != UDS_STATE_IDLE && !uds_paused_for(UdsPriority::Sequence)) {
    // A new sequence was requested, start it now. While paused it stays
    // queued, and the PID scan carries on if the pause allows it.
    uint16_t state = pending_seq_state;
    pending_seq_state = UDS_STATE_IDLE;
    handle_sequence(state, 0, nullptr, 0);
    return;
  }

  if (transmit_uds_pid_scan()) {
    // We did a PID scan.
    return;
  }
}

bool UdsCanBattery::transaction_tick() {
  if (uds_transaction_timeout <= 0) {
    return false;
  }

  if (--uds_transaction_timeout > 0) {
    // Still busy, do not send new requests
    return true;
  }

  if (isotp_is_busy()) {
    // We're still waiting for the ISO-TP transfer to complete, so we don't
    // consider this a timeout yet. We'll retry once the transfer finishes.

    uds_transaction_timeout = 1;
    return true;
  }

  // The current request timed out.
  uds_current_response_address = 0;

  if (seq_state != UDS_STATE_IDLE) {
    return on_sequence_step_timeout();
  }
  if (pending_pid != 0) {
    // Our PID scan must have timed out
    on_uds_pid_scan_timeout();
  }
  return false;
}

inline bool UdsCanBattery::on_sequence_step_timeout() {
  // If the step has retries left and the priority isn't paused, retry it.
  if (++seq_msg.retries <= seq_msg.max_retries && !uds_paused_for(seq_msg.priority)) {
    resend_seq_msg();
    return true;
  }
  const uint16_t failed = seq_state;
  seq_state = UDS_STATE_IDLE;
  security_retry_in_flight = false;
  if (deferred_seq_state != UDS_STATE_IDLE) {
    // A step of the unlock itself timed out.
    security_access_failed(failed, 0);
  } else {
    // Notify the subclass that it failed
    on_uds_sequence_timeout(failed);
  }
  return false;
}

bool UdsCanBattery::start_sequence(uint16_t state) {
  // May be called from other tasks (e.g. the webserver), so claim the slot
  // atomically: refuse if a sequence is already queued.
  uint32_t expected = UDS_STATE_IDLE;
  return pending_seq_state.compare_exchange_strong(expected, state);
}

void UdsCanBattery::wait_sequence(uint16_t state, uint16_t ticks, bool block_pid_scan) {
  // Only called from the CAN task, so no need for the start_sequence() slot.
  seq_wait_state = state;
  seq_wait_ticks = ticks;
  if (block_pid_scan) {
    extend_pause(ticks, UdsPriority::PidScan);
  }
}

bool UdsCanBattery::send_sequence_message(uint16_t state, SID sid, const uint8_t* data, uint16_t length,
                                          uint16_t timeout_ticks, uint8_t max_retries, UdsPriority priority) {
  const bool pid_in_flight = pending_pid != 0 && uds_transaction_timeout > 0;
  if (seq_state != UDS_STATE_IDLE || pid_in_flight || uds_paused_for(priority)) {
    // A sequence is already active, a PID request is in flight, or the pause
    // blocks this priority: refuse.
    return false;
  }

  if (length > sizeof(seq_msg.data)) {
    // Payload doesn't fit the sequence buffer; refuse rather than truncate.
    return false;
  }

  if (timeout_ticks == 0) {
    // Timeout must be non-zero.
    return false;
  }

  // A timed-out PID waiting for its retry gives way to the sequence. next_pid
  // is kept, so the scan resumes on it afterwards.
  pending_pid = 0;
  pid_retries = 0;

  seq_state = state;
  seq_msg.sid = sid;
  seq_msg.len = length;
  memcpy(seq_msg.data, data, seq_msg.len);
  seq_msg.timeout_ticks = timeout_ticks;
  seq_msg.retries = 0;
  seq_msg.max_retries = max_retries;
  seq_msg.priority = priority;
  uds_send(sid, seq_msg.data, seq_msg.len, timeout_ticks);
  return true;
}

void UdsCanBattery::pause_uds(uint16_t ticks_100ms, UdsPriority block_upto) {
  seq_pause_ticks = ticks_100ms;
  seq_pause_level = block_upto;
}

bool UdsCanBattery::handle_incoming_uds_can_frame(CAN_frame rx) {
  // A UDS request frame being received implies that there is a diagnostic tool
  // on the bus. Back off so we don't interfere with it.
  if (rx.ID == uds_address || rx.ID == OBD2_REQUEST_ADDRESS) {
    if (seq_pause_ticks == 0) {
      logging.println("UDS: external diagnostic traffic detected, backing off");
    }
    pause_uds(UDS_EXTERNAL_TOOL_BACKOFF_TICKS, UdsPriority::Custom);
    return true;
  }

  if (!is_from_ecu(rx.ID)) {
    return false;
  }

#ifdef UDS_DEBUG
  logging.printf("UDS RX: ID=0x%03X DLC=%d data=", rx.ID, rx.DLC);
  for (int i = 0; i < rx.DLC; i++) {
    logging.printf("%02X ", rx.data.u8[i]);
  }
  logging.println();
#endif

  // Record the address the current transaction is coming from.
  uds_current_response_address = rx.ID;

  // Pass down to the ISO-TP layer for reassembly.
  isotp_receive(rx.data.u8, rx.DLC, ISOTP_TATYPE_PHYSICAL);

  return true;
}

inline bool UdsCanBattery::is_from_ecu(uint32_t id) const {
  if (uds_current_response_address > 0 && id != uds_current_response_address) {
    // Not from the address we're mid-transaction with
    return false;
  }
  if (uds_response_address > 0) {
    // Only from the address we're expecting responses from
    return id == uds_response_address;
  }
  // Auto-detecting: anything in the range of potential UDS response IDs
  return id >= MIN_UDS_RESPONSE_ID && id <= MAX_UDS_RESPONSE_ID;
}

void UdsCanBattery::on_isotp_can_tx(uint32_t can_id, const uint8_t* can_data, uint8_t can_dlc) {

#ifdef UDS_DEBUG
  logging.printf("UDS TX: ID=0x%03X DLC=%d data=", can_id, can_dlc);
  for (int i = 0; i < can_dlc; i++) {
    logging.printf("%02X ", can_data[i]);
  }
  logging.println();
#endif

  // This is called by isotp_poll() from transmit_uds_can(..)
  CAN_frame frame = {};
  frame.ID = uds_address;  // Ignore the can_id from the ISO-TP layer, use our own.
  if (can_id > 0x7FF) {
    frame.ext_ID = true;
  } else {
    frame.ext_ID = false;
  }
  frame.FD = fd_uds_requests;
  frame.DLC = can_dlc;
  memcpy(frame.data.u8, can_data, can_dlc);
  transmit_can_frame(&frame);
}

void UdsCanBattery::on_isotp_rx_complete(const uint8_t* data, int len, isotp_tatype tatype) {
  // The ISO-TP layer has reassembled a complete UDS response, pass it on for processing.
  on_uds_receive(data, len);
}

static inline uint32_t parseBigEndianValue(const uint8_t* data, uint16_t length) {
  uint32_t val = 0;
  for (uint16_t i = 0; i < length && i < 4; i++) {
    val = (val << 8) | data[i];
  }
  return val;
}

bool UdsCanBattery::transmit_uds_pid_scan() {
  // Called during the transmit phase if there's nothing else to do. Sends the
  // next request in the PID scan cycle. The PID list is walked in order and
  // wraps around at the end; if handle_pid() requested a one-shot detour PID,
  // that is sent instead and the list walk resumes afterwards.

  if (pid_list == nullptr || pid_list_len == 0) {
    return false;
  }

  if (uds_paused_for(UdsPriority::PidScan)) {
    // The current pause blocks PID scanning (pause level >= PidScan). Leave
    // the scan position untouched so it resumes where it left off.
    return false;
  }

  if (next_pid == 0) {
    // Pick the next PID from the scan list (wrapping around at the end).
    if (pid_scan_index >= pid_list_len) {
      pid_scan_index = 0;
    }
    next_pid = pid_list[pid_scan_index];
  }

  // Request the next PID.
  const uint8_t data[2] = {(uint8_t)((next_pid >> 8) & 0xFF), (uint8_t)(next_pid & 0xFF)};
  pending_pid = next_pid;
  uds_send((SID)pid_scan_sid, &data[2 - pid_scan_id_bytes], pid_scan_id_bytes, UDS_TIMEOUT_READ_DID);
  return true;
}

inline void UdsCanBattery::schedule_next_pid(uint16_t detour) {
  // Ends the current PID request and sets up the next one, which will be
  // `detour` if nonzero, otherwise the next one in the scan list.

  next_pid = detour;
  if (detour == 0) {
    advance_pid_scan();
  }
  pending_pid = 0;
  pid_retries = 0;
}

bool UdsCanBattery::on_uds_pid_scan_response(uint8_t sid, const uint8_t* data, uint16_t len) {
  // Handle a UDS PID scan response, returning true if successful, false to keep
  // waiting.

  if (sid == kNegativeResponseSid) {
    if (len < 3 || data[1] != pid_scan_sid ||
        data[2] == NegativeResponseCode::RequestCorrectlyReceived_ResponsePending) {
      return false;
    }
    // Negative response to a PID request. Errors are swallowed for now: move
    // on to the next PID in the scan list.
    schedule_next_pid(0);
    return true;
  }

  const uint8_t value_offset = 1 + pid_scan_id_bytes;  // SID + identifier
  if (sid != UDS_RESPONSE_SID_OF(pid_scan_sid) || len < value_offset) {
    return false;
  }
  const uint16_t did = pid_scan_id_bytes == 2 ? (data[1] << 8) | data[2] : data[1];
  if (did != pending_pid) {
    // This wasn't for the PID we requested, keep waiting.
    return false;
  }

  // Decode and pass the value to the handler, scheduling its response as the next PID.
  const uint8_t* value = &data[value_offset];
  const uint16_t value_len = len - value_offset;
  schedule_next_pid(handle_pid(did, parseBigEndianValue(value, value_len), value, value_len));
  return true;
}

void UdsCanBattery::on_uds_pid_scan_timeout() {
  // Called when a PID scan request times out.

  pid_retries++;
  if (pid_retries < UDS_PID_MAX_RETRIES) {
    // Keep retrying...
    return;
  }

  // Move on to the next PID in the scan list.
  schedule_next_pid(0);
}

void UdsCanBattery::on_uds_receive(const uint8_t* data, uint16_t len) {
  // We've received a complete UDS response message.

  if (len < 1) {
    return;
  }

  const uint8_t sid = data[0];

#ifdef UDS_DEBUG
  if (sid != UDS_RESPONSE_SID_OF(SID::ReadDataByIdentifier)) {
    logging.printf("UDS RX: SID=0x%02X data=", sid);
    for (int i = 0; i < len; i++) {
      logging.printf("%02X ", data[i]);
    }
    logging.println();
  }
#endif

  if (seq_state == UDS_STATE_IDLE && pending_pid == 0) {
    // Nothing in flight: this can't be a response to anything we sent. Don't
    // let it pin the response address for a future transaction.
    uds_current_response_address = 0;
    return;
  }

  if (seq_state != UDS_STATE_IDLE) {
    on_sequence_response(sid, data, len);
  } else if (on_uds_pid_scan_response(sid, data, len)) {
    // The response finished the PID transaction.
    end_transaction();
  }
}

inline void UdsCanBattery::end_transaction() {
  uds_transaction_timeout = 0;
  uds_current_response_address = 0;
}

inline void UdsCanBattery::on_sequence_response(uint8_t sid, const uint8_t* data, uint16_t len) {
  // Is this a response to the in-flight sequence step (positive or negative)?
  const bool matched =
      (sid == UDS_RESPONSE_SID_OF(seq_msg.sid)) || (sid == kNegativeResponseSid && len >= 3 && data[1] == seq_msg.sid);
  if (!matched) {
    // Unmatched messages (stray/late responses) are ignored without touching
    // the in-flight transaction, so a stray frame can't kill the request.
    return;
  }
  if (sid == kNegativeResponseSid && (data[2] == NegativeResponseCode::RequestCorrectlyReceived_ResponsePending ||
                                      data[2] == NegativeResponseCode::BusyRepeatRequest)) {
    // ResponsePending: the ECU is still working on the step, so wait again
    // for the real response. BusyRepeatRequest: no response is coming, but
    // waiting out the timeout makes the usual retry path re-send the step.
    uds_transaction_timeout = seq_msg.timeout_ticks;
    return;
  }

  end_transaction();
  const uint16_t state = seq_state;
  // Go back to idle, the following functions may then override with the next step
  seq_state = UDS_STATE_IDLE;
  // If this was a security-denied response, engage the security access
  // mechanism (if enabled)
  const bool was_security_retry = security_retry_in_flight;
  security_retry_in_flight = false;
  const bool security_denied = sid == kNegativeResponseSid && data[2] == NegativeResponseCode::SecurityAccessDenied;
  if (security_denied && !was_security_retry && begin_security_access(state)) {
    // The step is deferred until the seed/key exchange completes.
    return;
  }
  handle_sequence(state, sid, data, len);
}

void UdsCanBattery::handle_sequence(uint16_t state, uint8_t sid, const uint8_t* data, uint16_t len) {
  // Dispatches responses for sequence states.
  if (state & UDS_STATE_INTERNAL) {
    // We handle our internal ones
    handle_internal_sequence(state, sid, data, len);
  } else {
    // Subclasses can override this to handle their own sequence states.
    on_uds_sequence_step(state, sid, data, len);
  }
}

void UdsCanBattery::handle_internal_sequence(uint16_t state, uint8_t sid, const uint8_t* data, uint16_t len) {
  // Try each internal sequence handler in turn.
  if (handle_dtc_step(state, sid, data, len)) {
    return;
  }
  if (handle_reset_step(state, sid)) {
    return;
  }
  handle_security_step(state, sid, data, len);
}

inline bool UdsCanBattery::handle_dtc_step(uint16_t state, uint8_t sid, const uint8_t* data, uint16_t len) {
  switch (state) {
    case UDS_STATE_READ_DTC_START:
      // Start a DTC readout sequence
      send_sequence_message(UDS_STATE_READ_DTC, SID::ReadDTCInformation, (const uint8_t*)"\x02\x09", 2,
                            UDS_TIMEOUT_READ_DTC, 2);
      return true;
    case UDS_STATE_READ_DTC:
      if (sid == UDS_RESPONSE_SID_OF(SID::ReadDTCInformation)) {
        handle_dtc_response(data, len);
      } else if (dtc != nullptr) {
        // Negative response to the DTC read: mark the readout as failed.
        dtc->dtc_read_failed = true;
        dtc->dtc_last_read_millis = millis();
      }
      return true;

    case UDS_STATE_CLEAR_DTC_START:
      // Start a DTC clear sequence
      // First enter a diagnostic session (might not be necessary but can't hurt)
      send_sequence_message(UDS_STATE_CLEAR_DTC_DIAG, SID::DiagnosticSessionControl, (const uint8_t*)"\x03", 1,
                            UDS_TIMEOUT_SESSION_CONTROL, 2);
      return true;
    case UDS_STATE_CLEAR_DTC_DIAG:
      // Now clear the DTCs
      send_sequence_message(UDS_STATE_CLEAR_DTC, SID::ClearDiagnosticInformation, (const uint8_t*)"\xFF\xFF\xFF", 3,
                            UDS_TIMEOUT_CLEAR_DTC, 2);
      return true;
    case UDS_STATE_CLEAR_DTC:
      // Positive response (0x54): nothing to do, the sequence has ended.
      return true;
  }
  return false;
}

inline bool UdsCanBattery::handle_reset_step(uint16_t state, uint8_t sid) {
  switch (state) {
    case UDS_STATE_RESET_START:
      // If configured, start by entering the appropriate diagnostic session
      if (reset_session != 0) {
        send_sequence_message(UDS_STATE_RESET_SESSION, SID::DiagnosticSessionControl, &reset_session, 1,
                              UDS_TIMEOUT_SESSION_CONTROL, 2);
        return true;
      }
      // No session change configured: reset straight away.
      [[fallthrough]];
    case UDS_STATE_RESET_SESSION:
      if (sid == kNegativeResponseSid) {
        logging.println("UDS: session for ECU reset refused");
        return true;
      }
      // The BMS drops off the bus while it restarts which will cause CAN errors
      ignore_can_errors_for(can_interface, UDS_RESET_IGNORE_CAN_ERRORS_MS);
      // Don't retry, the BMS might be busy resetting
      send_sequence_message(UDS_STATE_RESET, SID::ECUReset, &reset_type, 1, UDS_TIMEOUT_RESET, 0);
      return true;
    case UDS_STATE_RESET:
      logging.println(sid == kNegativeResponseSid ? "UDS: ECU reset refused" : "UDS: ECU reset accepted");
      return true;
  }
  return false;
}

static bool all_zero(const uint8_t* data, uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    if (data[i] != 0) {
      return false;
    }
  }
  return true;
}

inline bool UdsCanBattery::handle_security_step(uint16_t state, uint8_t sid, const uint8_t* data, uint16_t len) {
  switch (state) {
    case UDS_STATE_SECURITY_START:
      if (security_access_session != 0) {
        // Request a security access session of the configured type
        const uint8_t session = security_access_session;
        if (!send_sequence_message(UDS_STATE_SECURITY_SESSION, SID::DiagnosticSessionControl, &session, 1,
                                   UDS_TIMEOUT_SESSION_CONTROL, 2)) {
          security_access_failed(UDS_STATE_SECURITY_SESSION, 0);
        }
        return true;
      }
      // No session change configured: request the seed straight away.
      [[fallthrough]];
    case UDS_STATE_SECURITY_SESSION: {
      if (sid == kNegativeResponseSid) {
        security_access_failed(UDS_STATE_SECURITY_SESSION, len >= 3 ? data[2] : 0);
        return true;
      }
      // Now request the seed for the configured security access level.
      const uint8_t level = security_access_level;
      if (!send_sequence_message(UDS_STATE_SECURITY_SEED, SID::SecurityAccess, &level, 1, UDS_TIMEOUT_SECURITY, 2)) {
        security_access_failed(UDS_STATE_SECURITY_SEED, 0);
      }
      return true;
    }
    case UDS_STATE_SECURITY_SEED: {
      // 67 <level> <seed...>
      if (sid == kNegativeResponseSid || len < 3 || data[1] != security_access_level) {
        security_access_failed(UDS_STATE_SECURITY_SEED, sid == kNegativeResponseSid && len >= 3 ? data[2] : 0);
        return true;
      }
      // Extract the seed from the response.
      const uint8_t* seed = &data[2];
      const uint16_t seed_len = len - 2;
      if (all_zero(seed, seed_len)) {
        // The ECU is already unlocked for this level.
        resume_deferred_sequence();
        return true;
      }
      uint8_t key_payload[1 + UDS_SECURITY_MAX_KEY_LEN];
      key_payload[0] = security_access_level + 1;
      // Generate the key via the subclass implementation
      const uint8_t key_len = security_access_key(security_access_level, seed, seed_len, &key_payload[1]);
      // Only try once (the seed is single-use, so re-sending a stale key is futile)
      if (key_len == 0 || key_len > UDS_SECURITY_MAX_KEY_LEN ||
          !send_sequence_message(UDS_STATE_SECURITY_KEY, SID::SecurityAccess, key_payload, 1 + key_len,
                                 UDS_TIMEOUT_SECURITY, 0)) {
        security_access_failed(UDS_STATE_SECURITY_KEY, 0);
      }
      return true;
    }
    case UDS_STATE_SECURITY_KEY:
      if (sid == kNegativeResponseSid) {
        security_access_failed(UDS_STATE_SECURITY_KEY, len >= 3 ? data[2] : 0);
      } else {
        resume_deferred_sequence();
      }
      return true;
  }
  return false;
}

bool UdsCanBattery::begin_security_access(uint16_t state) {
  if (security_access_level == 0 || deferred_seq_state != UDS_STATE_IDLE) {
    return false;
  }

  // Save the request that hit NRC 0x33, then start the unlock straight away.
  // We can bypass start_sequence() because we're on the CAN task, which also
  // leaves its slot free for other callers.
  deferred_seq_state = state;
  deferred_seq_msg = seq_msg;
  security_retry_in_flight = false;
  handle_internal_sequence(UDS_STATE_SECURITY_START, 0, nullptr, 0);
  return true;
}

void UdsCanBattery::resume_deferred_sequence() {
  if (deferred_seq_state == UDS_STATE_IDLE) {
    return;
  }

  // Retry the deferred step with a fresh timeout and retry budget.
  seq_msg = deferred_seq_msg;
  seq_msg.retries = 0;
  seq_state = deferred_seq_state;
  deferred_seq_state = UDS_STATE_IDLE;
  security_retry_in_flight = true;  // so we don't get stuck in a loop
  resend_seq_msg();
}

inline void UdsCanBattery::resend_seq_msg() {
  uds_send(seq_msg.sid, seq_msg.data, seq_msg.len, seq_msg.timeout_ticks);
}

void UdsCanBattery::security_access_failed(uint16_t failed_state, uint8_t nrc) {
  if (deferred_seq_state == UDS_STATE_IDLE) {
    // We weren't in the middle of a sequence so nothing to resume
    return;
  }

  log_security_failure(failed_state, nrc);
  // Backoff so we don't get locked out by the BMS
  extend_pause(UDS_SECURITY_FAIL_BACKOFF_TICKS, UdsPriority::Sequence);

  // Let the routine see the original security-denied response.
  const uint16_t state = deferred_seq_state;
  const uint8_t denied[3] = {kNegativeResponseSid, deferred_seq_msg.sid,
                             (uint8_t)NegativeResponseCode::SecurityAccessDenied};
  deferred_seq_state = UDS_STATE_IDLE;
  security_retry_in_flight = false;
  handle_sequence(state, kNegativeResponseSid, denied, sizeof(denied));
}

inline void UdsCanBattery::log_security_failure(uint16_t failed_state, uint8_t nrc) {
  static const char* const steps[] = {"session", "seed request", "key"};
  const uint16_t step = failed_state - UDS_STATE_SECURITY_SESSION;
  const char* step_name = step < 3 ? steps[step] : "start";
  logging.printf("UDS: l%u security access failed at %s: ", security_access_level, step_name);
  if (nrc != 0) {
    logging.printf("NRC 0x%02X\n", nrc);
  } else {
    logging.println("no valid response");
  }
}

inline void UdsCanBattery::extend_pause(uint16_t ticks, UdsPriority block_upto) {
  // Don't weaken an existing stronger pause.
  if (seq_pause_ticks > 0 &&
      (seq_pause_level > block_upto || (seq_pause_level == block_upto && seq_pause_ticks >= ticks))) {
    return;
  }

  // Replace the current pause with the new one.
  pause_uds(ticks, block_upto);
}

void UdsCanBattery::handle_dtc_response(const uint8_t* data, uint16_t len) {
  if (dtc == nullptr) {
    return;
  }

  const bool is_kwp2000 = (pid_scan_id_bytes == 1);
  const bool valid_header = len >= 2 && (is_kwp2000 ? (data[0] == 0x59) : (data[1] == 0x02));

  if (!valid_header) {
    // Unexpected report type or a malformed response — treat as a failed readout.
    dtc->dtc_read_failed = true;
  } else {
    dtc->dtc_read_failed = false;
    // Each DTC is 3 code bytes then a status byte. KWP2000 starts after 59 <n>,
    // standard UDS after 59 02 <mask>.
    const uint16_t start = is_kwp2000 ? 2 : 3;
    uint16_t count = len > start ? (len - start) / 4 : 0;
    if (count > dtc->MAX_DTC_COUNT) {
      count = dtc->MAX_DTC_COUNT;
      logging.println("DTC count exceeds buffer, truncating");
    }
    for (uint16_t i = 0; i < count; i++) {
      const uint8_t* entry = &data[start + i * 4];
      dtc->dtc_codes[i] = parseBigEndianValue(entry, 3);
      dtc->dtc_status[i] = entry[3];
    }
    dtc->dtc_count = count;
  }
  dtc->dtc_last_read_millis = millis();
}

// Low level UDS send

void UdsCanBattery::uds_send(SID service_id, const uint8_t* data, uint16_t length, uint32_t timeout) {
  // Allocate a buffer large enough for the biggest UDS payload we might send
  uint8_t payload[1 + sizeof(seq_msg.data)];

  if (length >= sizeof(payload)) {
    return;
  }
  payload[0] = static_cast<uint8_t>(service_id);
  memcpy(&payload[1], data, length);
  isotp_send(payload, length + 1);

  // Debugging
  if (service_id != SID::ReadDataByIdentifier) {
    logging.printf("UDS TX: SID=0x%02X data=", (uint8_t)service_id);
    for (int i = 0; i < length; i++) {
      logging.printf("%02X ", data[i]);
    }
    logging.println();
  }

  uds_transaction_timeout = timeout;
}

void UdsCanBattery::setup_uds(uint32_t uds_address, uint32_t uds_response_address, const UdsConfig& config) {
  this->uds_address = uds_address;
  isotp_init(uds_address);
  this->uds_response_address = uds_response_address;
  restart_pid_scan();
  fd_uds_requests = config.fd;
  reset_type = config.reset_type;
  reset_session = config.reset_session;
  reset_holdoff_s = config.reset_holdoff_s;
  security_access_level = config.security_access_level;
  security_access_session = config.security_access_session;

  // Selects between UDS and KWP2000 'local ID' mode
  const bool use_local_ids = config.pid_scan_mode == UdsPidScanMode::OneByteLocalId;
  pid_scan_sid = use_local_ids ? UDS_SID_READ_LOCAL_IDENTIFIER : (uint8_t)SID::ReadDataByIdentifier;
  pid_scan_id_bytes = use_local_ids ? 1 : 2;
}

void UdsCanBattery::set_pid_scan_list(const uint16_t* pid_list, uint16_t length) {
  // Accepts a pointer and length of the list of PIDs to scan, and starts
  // scanning it from the beginning.

  this->pid_list = pid_list;
  pid_list_len = length;
  restart_pid_scan();
}

inline void UdsCanBattery::restart_pid_scan() {
  // Starts the PID scan again from the beginning of the list.

  pid_scan_index = 0;
  next_pid = 0;
  pending_pid = 0;
}

String UdsBatteryHtmlRenderer::get_status_html() {
  String ret = battery.get_uds_info_html();
  if (battery.dtc != nullptr) {
    ret += BatteryHtmlRenderer::render_dtc_section_html(*battery.dtc, battery.get_dtc_json_filename(),
                                                        battery.get_dtc_standard_code_string());
  }
  return ret;
}

bool UdsCanBattery::supports_read_DTC() {
  return true;
}

bool UdsCanBattery::supports_reset_DTC() {
  return true;
}

void UdsCanBattery::read_DTC() {
  start_sequence(UDS_STATE_READ_DTC_START);
}

void UdsCanBattery::reset_DTC() {
  start_sequence(UDS_STATE_CLEAR_DTC_START);
}

void UdsCanBattery::reset_BMS() {
  if (!request_uds_reset()) {
    logging.println("UDS: BMS reset not queued (busy, or too soon after the last one)");
  }
}

bool UdsCanBattery::request_uds_reset() {
  if (reset_type == 0) {
    return false;
  }
  // Claim the holdoff first, so concurrent callers can't both get through it.
  // It also covers the reset itself: the BMS may restart without replying.
  uint32_t expected = 0;
  if (!reset_holdoff_ticks.compare_exchange_strong(expected, reset_holdoff_s * 10)) {
    return false;
  }
  if (!start_sequence(UDS_STATE_RESET_START)) {
    // Another sequence is queued: give the holdoff back.
    reset_holdoff_ticks = 0;
    return false;
  }
  return true;
}
