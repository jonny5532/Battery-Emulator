#include "parallel_safety.h"
#include <cstdlib>  // std::abs
#include "../../battery/BATTERIES.h"
#include "../../datalayer/datalayer.h"
#include "../utils/events.h"

constexpr uint16_t MAX_PARALLEL_DIFF_DV = 15;  // 1.5 V
constexpr uint8_t ALERT_AFTER_S = 3;
constexpr uint8_t DISENGAGE_AFTER_S = 10;

// |a - b| in decivolts.
uint16_t abs_diff_dV(uint16_t a, uint16_t b) {
  return (uint16_t)std::abs((int)a - (int)b);
}

// True when the pack currently holds the DC link (contactors closed). Prefers
// the state the pack's own BMS reports; Unknown falls back to the
// BE-commanded state.
bool pack_holds_link(Battery* pack, bool commanded_engaged) {
  if (pack) {
    ContactorState reported = pack->reported_contactor_state();
    if (reported == ContactorState::Closed) {
      return true;
    }
    if (reported == ContactorState::Open) {
      return false;
    }
  }
  return commanded_engaged;
}

// One gate per secondary pack. These track how long any drift has been present.
SecondaryJoinGate gate2;
SecondaryJoinGate gate3;

// Raise/clear the "out of sync" alert event for one secondary.
void update_diff_event(EVENTS_ENUM_TYPE event, const SecondaryJoinGate& gate, uint16_t v_main, uint16_t v_pack) {
  if (gate.drift_seconds() > ALERT_AFTER_S) {
    set_event(event, (uint8_t)(abs_diff_dV(v_main, v_pack) / 10));
  } else {
    clear_event(event);
  }
}

// Given the main pack's voltage and a particular secondary, determine whether
// the main is allowed to close against it.
bool main_may_close_against(uint16_t v_main, const PackBusState& secondary) {
  if (!secondary.holds_bus) {
    // Secondary is not on the bus: the main may close.
    return true;
  }
  if (!v_main || !secondary.voltage_dV) {
    // One of the voltages is zero (unknown), don't allow closing yet. Due to
    // the main-battery flag semantics, this won't reopen them if they are
    // already closed.
    return false;
  }
  // Allow closing only if the voltages are close enough.
  return abs_diff_dV(v_main, secondary.voltage_dV) <= MAX_PARALLEL_DIFF_DV;
}

// Returns true if this pack is allowed to close.
bool SecondaryJoinGate::update(uint16_t main_voltage_dV, uint16_t my_voltage_dV, bool main_in_fault,
                               bool currently_allowed) {
  const bool data_ok = main_voltage_dV != 0 && my_voltage_dV != 0;
  // The voltage difference (or zero if no data yet). Derived, not state.
  const uint16_t diff = data_ok ? abs_diff_dV(main_voltage_dV, my_voltage_dV) : 0;

  if (!data_ok) {
    // If we don't have voltage data yet, just pass back the existing permission.
    // Also reset the drift timer.
    seconds_ = 0;
    return currently_allowed;
  }

  if (diff <= MAX_PARALLEL_DIFF_DV) {
    // We're in sync, so clear the drift count.
    seconds_ = 0;
    // Allow closing as long as the main is not in FAULT.
    return !main_in_fault;
  }

  // We're out of sync, count how long it has been.
  seconds_++;
  if (seconds_ > DISENGAGE_AFTER_S) {
    // Too long, disengage the pack.
    return false;
  }
  // We're out of sync, but still within the allowed window, just pass back the
  // existing permission.
  return currently_allowed;
}

void check_parallel_battery_safety() {
  const uint16_t v_main = datalayer.battery.status.voltage_dV;
  const bool main_fault = datalayer.system.status.system_status == FAULT;

  // Retrieve the state of each secondary:
  //  - what its own BMS-reported voltage is
  //  - whether it currently holds the bus (contactors closed)
  const PackBusState b2 =
      battery2_detected ? PackBusState{datalayer.battery2.status.voltage_dV,
                                       pack_holds_link(battery2, datalayer.system.status.contactors_battery2_engaged)}
                        : PackBusState{};
  const PackBusState b3 =
      battery3_detected ? PackBusState{datalayer.battery3.status.voltage_dV,
                                       pack_holds_link(battery3, datalayer.system.status.contactors_battery3_engaged)}
                        : PackBusState{};

  // Is the main allowed to close? Only if checks pass for both secondaries.
  // This flag isn't intended to reopen closed contactors though, just gate the
  // initial closing.
  datalayer.system.status.battery1_allowed_contactor_closing =
      main_may_close_against(v_main, b2) && main_may_close_against(v_main, b3);

  if (battery2_detected) {
    // Determine whether battery2 is allowed to close against the main. This
    // flag will actually reopen the contactors if it reverts, hence there is a
    // grace period (eg, to let the main rejoin first).
    datalayer.system.status.battery2_allowed_contactor_closing =
        gate2.update(v_main, datalayer.battery2.status.voltage_dV, main_fault,
                     datalayer.system.status.battery2_allowed_contactor_closing);
    // Raise an event if the gate has discovered drift.
    update_diff_event(EVENT_VOLTAGE_DIFFERENCE_BAT2, gate2, v_main, datalayer.battery2.status.voltage_dV);
  } else {
    gate2.reset();
  }

  // Same for battery3.
  if (battery3_detected) {
    datalayer.system.status.battery3_allowed_contactor_closing =
        gate3.update(v_main, datalayer.battery3.status.voltage_dV, main_fault,
                     datalayer.system.status.battery3_allowed_contactor_closing);
    update_diff_event(EVENT_VOLTAGE_DIFFERENCE_BAT3, gate3, v_main, datalayer.battery3.status.voltage_dV);
  } else {
    gate3.reset();
  }
}
