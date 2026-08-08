#ifndef PARALLEL_SAFETY_H
#define PARALLEL_SAFETY_H

#include <stdint.h>

/**
 * @brief Parallel-join safety for multiple packs sharing one DC bus.
 *
 * The safety rule is:
 *
 *   A pack may close only when its voltage is within 1.5V of the other pack,
 *   except the MAIN pack, which may always close onto a dead bus (one no
 *   secondary holds). A secondary never closes out of sync.
 *
 *   Drift: if a secondary drifts out of sync whilst on the bus, alert after 3s,
 *   disengage it after 10 s. The main is never forcibly opened by this rule.
 *
 */

/** What we know about one pack, as input to the parallel-join rule. */
struct PackBusState {
  uint16_t voltage_dV = 0;  // 0 = no valid reading yet
  bool holds_bus = false;   // contactors closed onto the shared bus
};

/**
 * Pure: may the MAIN pack close, given one secondary's state? True when the
 * secondary holds no bus (dead bus), or it does and the voltages are within
 * 1.5V of each other.
 */
bool main_may_close_against(uint16_t main_voltage_dV, const PackBusState& secondary);

/**
 * One instance per secondary pack. Has a drift counter that disengages a
 * secondary drifting out of sync (alert after 3s, disengage after 10s). The
 * `update` method calculates and returns the permission for this battery.
 */
class SecondaryJoinGate {
 public:
  /**
   * Call once per second.
   * @param main_voltage_dV   Main pack voltage, dV (0 = no valid reading)
   * @param my_voltage_dV     This pack's voltage, dV (0 = no valid reading)
   * @param main_in_fault     True while the main battery is in FAULT
   * @param currently_allowed The permission from the previous second
   * @return The permission for this second.
   */
  bool update(uint16_t main_voltage_dV, uint16_t my_voltage_dV, bool main_in_fault, bool currently_allowed);

  /** Consecutive seconds of a measurable discrepancy (0 when in sync or no data). */
  uint8_t drift_seconds() const { return seconds_; }

  void reset() { *this = SecondaryJoinGate(); }

 private:
  uint8_t seconds_ = 0;
};

/** Recompute all parallel-join permissions. Call once per second. */
void check_parallel_battery_safety();

#endif
