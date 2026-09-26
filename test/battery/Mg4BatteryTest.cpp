#include <gtest/gtest.h>
#include <vector>
#include "../../Software/src/battery/BATTERIES.h"
#include "../../Software/src/battery/MG-4-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/utils/events.h"

// TX frame capture injected by the emulated CAN layer (see test/emul/can.cpp).
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

class Mg4BatteryTest : public ::testing::Test {
 protected:
  Mg4Battery* battery;
  unsigned long now_ms = 0;

  void SetUp() override {
    battery = new Mg4Battery();
    // Reset datalayer to a known state
    memset(&datalayer, 0, sizeof(datalayer));
    user_selected_battery_chemistry = battery_chemistry_enum::NMC;
    battery->setup();
    // Event levels / states are process-global: reset so the reclose tests
    // observe this fixture's events only.
    init_events();
    reset_all_events();
    // Set some default info since setup sets some but not all
    datalayer.battery.info.total_capacity_Wh = 51000;  // 51kWh pack
    datalayer.battery.info.number_of_cells = 104;
    now_ms = 0;
    clear_transmitted_frames();
  }

  void TearDown() override { delete battery; }

  // Pack contactor states seen on 0x15B byte 21 low nibble: 3 = idle/open,
  // 11 = precharge active, 7 = closed/charging.
  void send_15b_fd(uint8_t contactor_state, uint16_t soc_times_ten = 500) {
    CAN_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.ID = 0x15B;
    frame.FD = true;
    frame.DLC = 32;
    frame.data.u8[7] = (soc_times_ten >> 6) & 0xFF;
    frame.data.u8[8] = (soc_times_ten & 0x3F) << 2;
    frame.data.u8[21] = contactor_state & 0x0F;
    battery->handle_incoming_can_frame(frame);
  }

  void send_308_serial() {
    // Captured 0x308 payloads (subfield 000554, NTSC serial). Decodes to
    // "0AFPEF20878703D782000497", index 4 = 'E' => NMC chemistry.
    static const char* frames[4] = {
        "0005150800000083FC0000080005160800001700000000000005290826F1FFFDFFFC00FF000554083041465045463200",
        "0005150800000083FC00000800051608000017000000000000052908C1F2FFFDFFFC00FF000554083038373837303301",
        "0005150800000083FC000008000516080000170000000000000529089CF3FFFDFFFC00FF000554084437383230303002",
        "0005150800000083FC0000080005160800001700000000000005290812F4FFFDFFFC00FF00055408343937FFFFFFFF03",
    };
    for (int k = 0; k < 4; k++) {
      CAN_frame frame;
      memset(&frame, 0, sizeof(frame));
      frame.ID = 0x308;
      frame.FD = true;
      frame.DLC = 48;
      for (int b = 0; b < 48; b++) {
        unsigned v = 0;
        sscanf(frames[k] + 2 * b, "%2x", &v);
        frame.data.u8[b] = (uint8_t)v;
      }
      battery->handle_incoming_can_frame(frame);
    }
  }

  // Drives identify_battery() through its real inputs: NTSC serial (NMC)
  // plus ECU hardware number with capacity digits "64" => 64kWh NMC pack.
  void identify_as_64kwh_nmc() {
    send_308_serial();
    const uint8_t hw[10] = {'S', 'H', 'Y', 'X', 'X', 'X', 'X', 'X', '6', '4'};
    battery->handle_pid(0xF192, 0, hw, sizeof(hw));
  }

  void step_10ms(int n) {
    for (int i = 0; i < n; i++) {
      now_ms += 10;
      battery->transmit_can(now_ms);
    }
  }

  bool transmitted_047() {
    for (const auto& f : get_transmitted_frames()) {
      if (f.ID == 0x047) {
        return true;
      }
    }
    return false;
  }
};

TEST_F(Mg4BatteryTest, ParsesPackSerialFrom308) {
  // Captured 0x308 payloads from mg4_dev/308.log: subfield 000554 has no
  // CRC slot, it carries 7 ASCII bytes + 1 index byte per frame.
  static const char* frames[4] = {
      "0005150800000083FC0000080005160800001700000000000005290826F1FFFDFFFC00FF000554083041465045463200",
      "0005150800000083FC00000800051608000017000000000000052908C1F2FFFDFFFC00FF000554083038373837303301",
      "0005150800000083FC000008000516080000170000000000000529089CF3FFFDFFFC00FF000554084437383230303002",
      "0005150800000083FC0000080005160800001700000000000005290812F4FFFDFFFC00FF00055408343937FFFFFFFF03",
  };
  // Deliver out of order to prove index-based assembly, then repeat frame 0
  // to prove reassembly is idempotent.
  for (int k : {2, 0, 3, 1, 0}) {
    CAN_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.ID = 0x308;
    frame.FD = true;
    frame.DLC = 48;
    for (int b = 0; b < 48; b++) {
      unsigned v = 0;
      sscanf(frames[k] + 2 * b, "%2x", &v);
      frame.data.u8[b] = (uint8_t)v;
    }
    battery->handle_incoming_can_frame(frame);
  }
  std::string html = battery->get_uds_info_html().c_str();
  EXPECT_NE(html.find("0AFPEF20878703D782000497"), std::string::npos);
}

TEST_F(Mg4BatteryTest, ContactorHoldsOpenUntilIdentified) {
  // Pack reports open while the battery is still unidentified: the emulator
  // must stay silent in WAITING (no 0x047 close sequence), even past the
  // 5 s startup grace.
  send_15b_fd(3);
  step_10ms(600);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::WAITING_FOR_PACK);
  EXPECT_FALSE(battery->battery_identified_for_test());
  clear_transmitted_frames();
  step_10ms(50);
  EXPECT_FALSE(transmitted_047());

  // Once identified, the same open pack must start the closing sequence.
  identify_as_64kwh_nmc();
  EXPECT_TRUE(battery->battery_identified_for_test());
  clear_transmitted_frames();
  step_10ms(3);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSING);
  EXPECT_TRUE(transmitted_047());
}

TEST_F(Mg4BatteryTest, ContactorRidesThroughClosedWithinGrace) {
  // Reboot with already-closed pack: may stay at the closed tail while
  // unidentified, provided startup grace has not expired.
  send_15b_fd(7);
  step_10ms(2);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);
  EXPECT_FALSE(battery->battery_identified_for_test());
  clear_transmitted_frames();
  step_10ms(5);
  EXPECT_TRUE(transmitted_047());
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);
}

TEST_F(Mg4BatteryTest, ContactorOpensPastGraceWhenUnidentified) {
  // Ride-through expires: an unidentified pack past the 5 s grace must drive
  // open and stick there (continuous open loop) until identified.
  send_15b_fd(7);
  step_10ms(2);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);
  step_10ms(600);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);
  clear_transmitted_frames();
  step_10ms(20);  // Past one full 150-frame (1.5 s) open loop
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);
  EXPECT_TRUE(transmitted_047());
}

TEST_F(Mg4BatteryTest, ContactorDrivesOpenWhenPackNeverHeard) {
  // No 0x15B ever received and still unidentified past grace: drive open
  // rather than closing blind, and stick there until identified.
  step_10ms(600);
  EXPECT_FALSE(battery->battery_identified_for_test());
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);
  clear_transmitted_frames();
  step_10ms(20);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);
  EXPECT_TRUE(transmitted_047());
}

TEST_F(Mg4BatteryTest, ContactorReturnsToWaitingOnceIdentified) {
  // Sticky OPENING releases back to WAITING as soon as the pack identifies,
  // then follows the normal close flow (pack open => CLOSING).
  step_10ms(600);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);
  identify_as_64kwh_nmc();
  send_15b_fd(3);
  step_10ms(1);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::WAITING_FOR_PACK);
  step_10ms(3);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSING);
}

TEST_F(Mg4BatteryTest, ContactorReclosesWhenIdentified) {
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);
  // Pack drops out on its own: an identified pack replays the closing sequence.
  send_15b_fd(3);
  step_10ms(2);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSING);
}

TEST_F(Mg4BatteryTest, ContactorDoesNotRecloseWhenUnidentified) {
  // Ride-through CLOSED, then the pack opens on its own while still
  // unidentified: must drive open and stick there, never enter CLOSING.
  send_15b_fd(7);
  step_10ms(2);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);
  send_15b_fd(3);
  step_10ms(1);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_OPEN)->state, EVENT_STATE_ACTIVE);
  for (int i = 0; i < 50; i++) {
    step_10ms(1);
    auto s = battery->contactor_state_for_test();
    EXPECT_NE(s, Mg4Battery::ContactorState::CLOSING);
    EXPECT_NE(s, Mg4Battery::ContactorState::CLOSED);
  }
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);
}

TEST_F(Mg4BatteryTest, ContactorFaultOpensAndReleaseReturnsToWaiting) {
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);
  datalayer.system.status.system_status = FAULT;
  step_10ms(2);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);
  // Clearing the fault returns to WAITING first (pack state re-checked).
  datalayer.system.status.system_status = ACTIVE;
  step_10ms(1);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::WAITING_FOR_PACK);
}

TEST_F(Mg4BatteryTest, ContactorRecloseLoopTripsFatalEvent) {
  // Identified pack closes normally, then opens by itself (e.g. HV
  // isolation fault) 10 times in quick succession: the 10th reclose must
  // latch open and raise the fatal reclose event instead of closing again.
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  ASSERT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);

  for (int k = 0; k < 10; k++) {
    send_15b_fd(3);  // pack opens by itself
    step_10ms(2);
    if (k < 9) {
      EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSING);
      EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_OPEN)->state, EVENT_STATE_ACTIVE);
      send_15b_fd(7);  // pack closes again
      step_10ms(2);
      EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);
      EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_OPEN)->state, EVENT_STATE_INACTIVE);
    }
  }
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);
  EXPECT_TRUE(battery->reclose_blocked_for_test());
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_OPEN)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_RECLOSE_FAULT)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_RECLOSE_FAULT)->data, 10);
  EXPECT_EQ(datalayer.system.status.system_status, FAULT);

  // Latched: a pack that stays open must not re-enter CLOSING.
  send_15b_fd(3);
  step_10ms(50);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);
  EXPECT_TRUE(battery->reclose_blocked_for_test());
}

TEST_F(Mg4BatteryTest, SparseReclosesDoNotTrip) {
  // 9 rapid self-opens stay under the trip count, then 301 s pass with the
  // pack closed: the next reclose falls outside the 300 s window, so the
  // drive must reclose normally instead of tripping.
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  ASSERT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);

  for (int k = 0; k < 9; k++) {
    send_15b_fd(3);
    step_10ms(2);
    EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSING);
    send_15b_fd(7);
    step_10ms(2);
    EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);
  }
  clear_transmitted_frames();
  step_10ms(30100);  // 301 s, pack stays closed
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);

  send_15b_fd(3);
  step_10ms(2);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSING);
  EXPECT_FALSE(battery->reclose_blocked_for_test());
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_RECLOSE_FAULT)->state, EVENT_STATE_INACTIVE);
}

TEST_F(Mg4BatteryTest, ManualEstopCycleResetsRecloseFault) {
  // Trip the latch first.
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  ASSERT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);
  for (int k = 0; k < 10; k++) {
    send_15b_fd(3);
    step_10ms(2);
    if (k < 9) {
      send_15b_fd(7);
      step_10ms(2);
    }
  }
  ASSERT_TRUE(battery->reclose_blocked_for_test());
  ASSERT_EQ(get_event_pointer(EVENT_CONTACTOR_RECLOSE_FAULT)->state, EVENT_STATE_ACTIVE);

  // Manual open via the homepage button / estop (flag plus the
  // EQUIPMENT_STOP event, exactly as setBatteryPause() drives it) clears
  // the latch and event, while the estop itself holds the drive open.
  datalayer.system.info.equipment_stop_active = true;
  set_event(EVENT_EQUIPMENT_STOP, 1);
  step_10ms(2);
  EXPECT_FALSE(battery->reclose_blocked_for_test());
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_RECLOSE_FAULT)->state, EVENT_STATE_INACTIVE);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::OPENING);

  // Manual close resumes the normal flow (pack still reports open).
  datalayer.system.info.equipment_stop_active = false;
  clear_event(EVENT_EQUIPMENT_STOP);
  step_10ms(3);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSING);

  // Fresh tracker: one self-open recloses instead of tripping.
  send_15b_fd(7);
  step_10ms(2);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSED);
  send_15b_fd(3);
  step_10ms(2);
  EXPECT_EQ(battery->contactor_state_for_test(), Mg4Battery::ContactorState::CLOSING);
  EXPECT_FALSE(battery->reclose_blocked_for_test());
}

TEST_F(Mg4BatteryTest, StoresAndDisplaysEcuPartNumbers) {
  const uint8_t hw[10] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
  const uint8_t sw[10] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J'};
  battery->handle_pid(0xF192, 0, hw, sizeof(hw));
  battery->handle_pid(0xF194, 0, sw, sizeof(sw));
  std::string html = battery->get_uds_info_html().c_str();
  EXPECT_NE(html.find("1234567890"), std::string::npos);
  EXPECT_NE(html.find("ABCDEFGHIJ"), std::string::npos);
}
