#include <gtest/gtest.h>

#include <Arduino.h>  // Emul: set_millis64() to control the test clock

#include "../Software/src/battery/BATTERIES.h"
#include "../Software/src/battery/MG-GEN1-BATTERY.h"
#include "../Software/src/battery/TEST-FAKE-BATTERY.h"
#include "../Software/src/communication/contactorcontrol/comm_contactorcontrol.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/safety/parallel_safety.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

#include <vector>

// TX frame capture injected by the emulated CAN layer (see emul/can.cpp).
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

// Mirrors the file-scope contactor FSM in comm_contactorcontrol.cpp so the
// gate test can place it in a known state. Must match the definition there.
enum State { DISCONNECTED, START_PRECHARGE, PRECHARGE, POSITIVE, PRECHARGE_OFF, COMPLETED, SHUTDOWN_REQUESTED };
extern State contactorStatus;

// Tests for the symmetric parallel-join gate (#2711): the 1.5 V rule used to
// gate only battery 2/3 toward battery 1 - battery 1's own (re-)close was
// never checked, so after a main-pack dropout it could close onto a link
// another pack holds live, with the surge limited only by pack resistances.

class ParallelJoinSymmetryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    datalayer = DataLayer();
    reset_all_events();
    init_hal();
    battery2_detected = true;
    battery3_detected = false;
    datalayer.system.status.system_status = ACTIVE;
    // Not 0 (no data) - 3700 is a real voltage, not a sentinel anymore
    datalayer.battery.status.voltage_dV = 3900;
    datalayer.battery2.status.voltage_dV = 3900;
  }

  void TearDown() override {
    battery2_detected = false;
    if (battery2) {
      delete battery2;
      battery2 = nullptr;
    }
  }
};

TEST_F(ParallelJoinSymmetryTest, EngagedPackWithLargeDiffBlocksMainClose) {
  datalayer.system.status.contactors_battery2_engaged = true;
  datalayer.battery2.status.voltage_dV = 3700 + 250;  // 25 V below main

  check_parallel_battery_safety();

  EXPECT_FALSE(datalayer.system.status.battery1_allowed_contactor_closing)
      << "Main battery must not close onto a live link with a large voltage difference";
}

TEST_F(ParallelJoinSymmetryTest, EngagedPackWithinWindowAllowsMainClose) {
  datalayer.system.status.contactors_battery2_engaged = true;
  datalayer.battery2.status.voltage_dV = 3910;  // 1.0 V difference

  check_parallel_battery_safety();

  EXPECT_TRUE(datalayer.system.status.battery1_allowed_contactor_closing);
}

TEST_F(ParallelJoinSymmetryTest, DisengagedPackDoesNotBlockMain) {
  datalayer.system.status.contactors_battery2_engaged = false;
  datalayer.battery2.status.voltage_dV = 3600;  // 30 V difference, but link is dead

  check_parallel_battery_safety();

  EXPECT_TRUE(datalayer.system.status.battery1_allowed_contactor_closing)
      << "A pack with open contactors holds no link - any voltage difference is fine";
}

TEST_F(ParallelJoinSymmetryTest, ExistingBattery2GatingUnchanged) {
  // Regression guard for the original direction of the rule
  datalayer.battery2.status.voltage_dV = 3905;
  check_parallel_battery_safety();
  EXPECT_TRUE(datalayer.system.status.battery2_allowed_contactor_closing);

  datalayer.battery2.status.voltage_dV = 3600;
  for (int i = 0; i < 11; i++) {
    check_parallel_battery_safety();
  }
  EXPECT_FALSE(datalayer.system.status.battery2_allowed_contactor_closing)
      << "Battery 2 must still disengage after 10 s out of sync";
}

TEST_F(ParallelJoinSymmetryTest, UnknownReportedStateFallsBackToCommanded) {
  // TestFake does not override reported_contactor_state() -> Unknown
  battery2 = new TestFakeBattery(&datalayer.battery2, CAN_NATIVE);
  datalayer.system.status.contactors_battery2_engaged = true;
  datalayer.battery2.status.voltage_dV = 3600;

  check_parallel_battery_safety();

  EXPECT_FALSE(datalayer.system.status.battery1_allowed_contactor_closing)
      << "Unknown reported state must fall back to the BE-commanded engaged state";

  datalayer.system.status.contactors_battery2_engaged = false;
  check_parallel_battery_safety();
  EXPECT_TRUE(datalayer.system.status.battery1_allowed_contactor_closing);
}

TEST_F(ParallelJoinSymmetryTest, GateBlocksStartPrecharge) {
  set_millis64(100000);  // Past the 10 s startup window
  contactor_control_enabled = true;
  contactorStatus = DISCONNECTED;
  battery_detected = true;
  datalayer.system.status.inverter_allows_contactor_closing = true;
  datalayer.system.info.equipment_stop_active = false;

  datalayer.system.status.battery1_allowed_contactor_closing = false;
  handle_contactors();
  EXPECT_EQ(contactorStatus, DISCONNECTED) << "The join gate must hold the main battery in DISCONNECTED";

  datalayer.system.status.battery1_allowed_contactor_closing = true;
  handle_contactors();
  // The FSM transitions DISCONNECTED -> START_PRECHARGE -> PRECHARGE within one tick
  EXPECT_EQ(contactorStatus, PRECHARGE);

  contactor_control_enabled = false;
  contactorStatus = DISCONNECTED;
  set_millis64(0);
}

// The MG-GEN1 is a CAN-controlled pack: the BMS closes its own contactors, so
// the BE-commanded GPIO flag (contactors_battery2_engaged) never reflects
// reality for it. reported_contactor_state() must expose the BMS's own view
// from the 0x297 status byte, otherwise pack_holds_link() reports "not on the
// bus" for an MG secondary and the main gate never engages.
TEST_F(ParallelJoinSymmetryTest, MgGen1ReportsBmsContactorState) {
  MgGen1Battery mg(&datalayer.battery2, CAN_NATIVE, &datalayer.system.status.battery2_allowed_contactor_closing);
  Battery* pack = &mg;  // exercise the virtual via the base interface

  auto status_frame = [](uint8_t state) {
    CAN_frame f = {};
    f.ID = 0x297;
    f.DLC = 8;
    f.data.u8[1] = state;
    return f;
  };

  // No 0x297 frame seen yet -> Unknown
  EXPECT_EQ(pack->reported_contactor_state(), ContactorState::Unknown);

  mg.handle_incoming_can_frame(status_frame(3));  // connected
  EXPECT_EQ(pack->reported_contactor_state(), ContactorState::Closed);

  mg.handle_incoming_can_frame(status_frame(1));  // disconnected
  EXPECT_EQ(pack->reported_contactor_state(), ContactorState::Open);

  mg.handle_incoming_can_frame(status_frame(15));  // fault
  EXPECT_EQ(pack->reported_contactor_state(), ContactorState::Open);

  mg.handle_incoming_can_frame(status_frame(2));  // precharge
  EXPECT_EQ(pack->reported_contactor_state(), ContactorState::Unknown);

  mg.handle_incoming_can_frame(status_frame(8));  // checking
  EXPECT_EQ(pack->reported_contactor_state(), ContactorState::Unknown);
}

// The MG-GEN1 primary must honour battery1_allowed_contactor_closing like a
// secondary honours its pointer, otherwise a blocked main pack still commands
// its contactors closed onto a live, discrepant bus.
TEST_F(ParallelJoinSymmetryTest, MgGen1PrimaryHonoursJoinGate) {
  MgGen1Battery primary;  // Default ctor = primary battery (no gate pointer)
  primary.setup();
  primary.got_battery_type(0x00010203);  // BATTERY_TYPE_MG5 -> 96 cells

  // Feed all 96 cell voltages so the pack is "identified".
  CAN_frame cell = {};
  cell.ID = 0x3BE;
  cell.DLC = 8;
  cell.data.u8[2] = (3500 - 1000) >> 8;  // v = 1000 + (u8[2]<<8 | u8[3]) = 3500 mV
  cell.data.u8[3] = (3500 - 1000) & 0xFF;
  for (uint16_t id = 0; id < 96; id++) {
    cell.data.u8[5] = id;
    primary.handle_incoming_can_frame(cell);
  }

  // A recent pack voltage so voltageValidTime > 0.
  CAN_frame volt = {};
  volt.ID = 0x3AC;
  volt.DLC = 8;
  const uint16_t v = (3900 * 2) / 5;  // voltage_dV = v * 5 / 2 -> 3900 dV
  volt.data.u8[2] = 0;
  volt.data.u8[3] = 0;
  volt.data.u8[4] = (v >> 8) & 0x0F;
  volt.data.u8[5] = v & 0xFF;
  volt.data.u8[6] = 20000 >> 8;  // i = 20000 -> 0 A
  volt.data.u8[7] = 20000 & 0xFF;
  primary.handle_incoming_can_frame(volt);

  datalayer.system.status.inverter_allows_contactor_closing = true;
  datalayer.system.status.system_status = ACTIVE;

  // Last 0x08A frame the driver transmitted: u8[5] == 0x00 means "open",
  // 0x02 means "close" (the frame default). -1 = none sent yet.
  auto last_8a_byte5 = []() -> int {
    const auto& frames = get_transmitted_frames();
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
      if (it->ID == 0x08A) {
        return it->data.u8[5];
      }
    }
    return -1;
  };

  set_millis64(100000);  // Past the 30 s startup grace so commands are sent

  // Join gate denies: the primary must command the contactors OPEN.
  datalayer.system.status.battery1_allowed_contactor_closing = false;
  clear_transmitted_frames();
  primary.transmit_can(100000);  // send_phase 0: the 0x08A frame is sent
  EXPECT_EQ(last_8a_byte5(), 0x00) << "Primary must open when the join gate denies closing";

  // Join gate allows: the primary commands CLOSE.
  datalayer.system.status.battery1_allowed_contactor_closing = true;
  clear_transmitted_frames();
  primary.transmit_can(100100);  // phase 1: no 0x08A
  primary.transmit_can(100200);  // phase 2: no 0x08A
  primary.transmit_can(100300);  // phase 0: 0x08A sent
  EXPECT_EQ(last_8a_byte5(), 0x02) << "Primary must close when the join gate allows";

  set_millis64(0);
}

// 3700 dV (370.0 V) is a real operating point for MG packs (a 108s ZS mid-SoC
// reads exactly 370.0 V) as well as the datalayer startup default. The old
// check aborted on it, silently disabling the whole rule at a real voltage.
// The rewrite has no sentinel: 3700 is data, so a pack holding the bus at
// 3700 genuinely blocks a main >1.5 V away, and equal values are simply in
// sync.
TEST_F(ParallelJoinSymmetryTest, ThirtySevenHundredVoltsIsTreatedAsARealVoltage) {
  datalayer.system.status.contactors_battery2_engaged = true;  // b2 holds the bus
  datalayer.battery.status.voltage_dV = 3900;                  // main 390.0 V
  datalayer.battery2.status.voltage_dV = 3700;                 // b2 at exactly 370.0 V

  check_parallel_battery_safety();
  EXPECT_FALSE(datalayer.system.status.battery1_allowed_contactor_closing)
      << "3700 is data, not a sentinel: a 20 V discrepancy must block the main";

  datalayer.battery.status.voltage_dV = 3700;  // now both at 370.0 V
  check_parallel_battery_safety();
  EXPECT_TRUE(datalayer.system.status.battery1_allowed_contactor_closing) << "Equal genuine voltages are in sync";
}
