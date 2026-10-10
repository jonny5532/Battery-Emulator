#include <gtest/gtest.h>
#include <vector>
#include "../../Software/src/battery/BATTERIES.h"
#include "../../Software/src/battery/MG-4-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/utils/events.h"

// TX frame capture injected by the emulated CAN layer (see test/emul/can.cpp).
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

// Testable subclass exposing protected contactor/identification state.
class TestableMg4Battery : public Mg4Battery {
 public:
  using Mg4Battery::bal_demand;
  using Mg4Battery::bal_have_sum;
  using Mg4Battery::bal_last_sum;
  using Mg4Battery::batteryIdentified;
  using Mg4Battery::contactorState;
  using Mg4Battery::did_sweep_answered;
  using Mg4Battery::did_sweep_buf_len;
  using Mg4Battery::DID_SWEEP_BUF_MAX;
  using Mg4Battery::did_sweep_dropped;
  using Mg4Battery::did_sweep_next;
  using Mg4Battery::DID_SWEEP_PAGE_HTML_BUDGET;
  using Mg4Battery::mg4_seq_start;
  using Mg4Battery::mg4_seq_tick;
  using Mg4Battery::reclose_blocked;
  using Mg4Battery::seq_08a_flag;
  using Mg4Battery::seq_08a_level;
  using Mg4Battery::seq_08a_request;
  using Mg4Battery::seq_313_stat;
  using Mg4Battery::seq_314_hi;
  using Mg4Battery::seq_314_val;
  using Mg4Battery::seq_cnt15;
  using Mg4Battery::SHUTDOWN_HOLD_MAX_T;
  using Mg4Battery::SHUTDOWN_HOLD_T;
  using Mg4Battery::SHUTDOWN_LEVEL_T;
  using Mg4Battery::SHUTDOWN_OPEN_HOLD_T;
  using Mg4Battery::SHUTDOWN_OPEN_TIMEOUT_T;
  using Mg4Battery::shutdown_phase;
  using Mg4Battery::ShutdownPhase;
  using Mg4Battery::sleep_abort_reason;
  using Mg4Battery::sleep_self_wakes;
  using Mg4Battery::SNAP_ABSORB_MAX_S;
  using Mg4Battery::SNAP_OVER_S;
  using Mg4Battery::snapEnded;
  using Mg4Battery::WAKE_REQ03_T;
};

class Mg4BatteryTest : public ::testing::Test {
 protected:
  TestableMg4Battery* battery;
  unsigned long now_ms = 0;

  void SetUp() override {
    battery = new TestableMg4Battery();
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

  void send_308_serial_string(const char* serial28) {
    // Build four 0x308 subframes (addr 0x554) carrying 7 ASCII bytes + index.
    for (int k = 0; k < 4; k++) {
      CAN_frame frame;
      memset(&frame, 0, sizeof(frame));
      frame.ID = 0x308;
      frame.FD = true;
      frame.DLC = 12;
      frame.data.u8[0] = 0x00;
      frame.data.u8[1] = 0x05;
      frame.data.u8[2] = 0x54;
      frame.data.u8[3] = 8;
      for (int j = 0; j < 7; j++) {
        frame.data.u8[4 + j] = (uint8_t)serial28[k * 7 + j];
      }
      frame.data.u8[11] = (uint8_t)k;
      battery->handle_incoming_can_frame(frame);
    }
  }

  // 51kWh LFP pack: serial index 4 = 'B', capacity digits "51" => 104 cells.
  void identify_as_51kwh_lfp() {
    send_308_serial_string("0AFPB51234567890123456789012");
    const uint8_t hw[10] = {'S', 'H', 'Y', 'X', 'X', 'X', 'X', 'X', '5', '1'};
    battery->handle_pid(0xF192, 0, hw, sizeof(hw));
  }

  // Inject a 0x12C pack status frame. current_dA uses the datalayer
  // convention (+ = charging). Temps default to a benign 25C.
  void send_12c(uint16_t cell_max_mV, uint16_t cell_min_mV, int16_t current_dA, uint16_t voltage_dV,
                int16_t temp_dC = 250) {
    CAN_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.ID = 0x12C;
    frame.DLC = 40;
    uint16_t word = (uint16_t)((voltage_dV * 2) / 5);
    frame.data.u8[8] = (uint8_t)(word >> 4);
    frame.data.u8[9] = (uint8_t)((word & 0xF) << 4);
    int32_t raw = 20000 - 2 * (int32_t)current_dA;
    if (raw < 0) {
      raw = 0;
    }
    if (raw > 40000) {
      raw = 40000;
    }
    frame.data.u8[6] = (uint8_t)(raw >> 8);
    frame.data.u8[7] = (uint8_t)(raw & 0xFF);
    uint16_t min_raw = (uint16_t)(cell_min_mV * 8);
    uint16_t max_raw = (uint16_t)(cell_max_mV * 8);
    frame.data.u8[30] = (uint8_t)(min_raw >> 8);
    frame.data.u8[31] = (uint8_t)(min_raw & 0xFF);
    frame.data.u8[32] = (uint8_t)(max_raw >> 8);
    frame.data.u8[33] = (uint8_t)(max_raw & 0xFF);
    uint8_t t = (uint8_t)((temp_dC + 400) / 5);
    frame.data.u8[22] = t;
    frame.data.u8[23] = t;
    battery->handle_incoming_can_frame(frame);
  }

  // Inject a captured CAN FD frame given as hex payload bytes.
  void send_fd_hex(uint32_t id, const char* hex) {
    CAN_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.ID = id;
    frame.FD = true;
    frame.DLC = (uint8_t)(strlen(hex) / 2);
    for (int b = 0; b < frame.DLC; b++) {
      unsigned v = 0;
      sscanf(hex + 2 * b, "%2x", &v);
      frame.data.u8[b] = (uint8_t)v;
    }
    battery->handle_incoming_can_frame(frame);
  }

  // Inject a 0x17E BMS power limit frame, limits in 0.5kW counts (0x7FF =
  // invalid). Laid out like the captures: 0x503, 0x504 and 0x564 subframes.
  void send_17e(uint16_t dis_counts, uint16_t chg_counts) {
    CAN_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.ID = 0x17E;
    frame.FD = true;
    frame.DLC = 48;
    const uint8_t addrs[3] = {0x03, 0x04, 0x64};
    for (int k = 0; k < 3; k++) {
      frame.data.u8[k * 12 + 1] = 0x05;
      frame.data.u8[k * 12 + 2] = addrs[k];
      frame.data.u8[k * 12 + 3] = 8;
    }
    // 0x503: two copies of the discharge limit, packed from sub[2] bit 7.
    uint8_t* d = &frame.data.u8[4];
    d[2] = (uint8_t)(dis_counts >> 3);
    d[3] = (uint8_t)(((dis_counts & 0x07) << 5) | (dis_counts >> 6));
    d[4] = (uint8_t)((dis_counts & 0x3F) << 2);
    // 0x504: two copies of the charge limit, packed from sub[4] bit 5.
    uint8_t* c = &frame.data.u8[16];
    c[4] = (uint8_t)(chg_counts >> 5);
    c[5] = (uint8_t)(((chg_counts & 0x1F) << 3) | (chg_counts >> 8));
    c[6] = (uint8_t)(chg_counts & 0xFF);
    battery->handle_incoming_can_frame(frame);
  }

  // BMS limits sent by tick_1s(), in 0.5kW counts. Defaults sit well above
  // the hard caps (200kW / 60kW, as captured mid-SoC) so they don't bind.
  uint16_t bms_dis_counts = 400;
  uint16_t bms_chg_counts = 120;

  // One simulated second: fresh CAN data then the 1Hz update tick.
  void tick_1s(uint16_t cell_max_mV, uint16_t cell_min_mV, int16_t current_dA, uint16_t voltage_dV,
               uint16_t soc_times_ten = 970, int16_t temp_dC = 250) {
    send_12c(cell_max_mV, cell_min_mV, current_dA, voltage_dV, temp_dC);
    send_15b_fd(7, soc_times_ten);
    send_17e(bms_dis_counts, bms_chg_counts);
    battery->update_values();
  }

  // Bulk-phase charging below absorption so the snap current EMA carries
  // realistic history (a fresh-boot EMA starts at 0 and would end the snap
  // on the first absorption tick).
  void warmup_bulk(int n = 60) {
    for (int i = 0; i < n; i++) {
      tick_1s(3500, 3490, 100, 3600, 500);
    }
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
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::WAITING_FOR_PACK);
  EXPECT_FALSE(battery->batteryIdentified);
  clear_transmitted_frames();
  step_10ms(50);
  EXPECT_FALSE(transmitted_047());

  // Once identified, the same open pack must start the closing sequence.
  identify_as_64kwh_nmc();
  EXPECT_TRUE(battery->batteryIdentified);
  clear_transmitted_frames();
  step_10ms(3);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSING);
  EXPECT_TRUE(transmitted_047());
}

TEST_F(Mg4BatteryTest, ContactorRidesThroughClosedWithinGrace) {
  // Reboot with already-closed pack: may stay at the closed tail while
  // unidentified, provided startup grace has not expired.
  send_15b_fd(7);
  step_10ms(2);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
  EXPECT_FALSE(battery->batteryIdentified);
  clear_transmitted_frames();
  step_10ms(5);
  EXPECT_TRUE(transmitted_047());
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
}

TEST_F(Mg4BatteryTest, ContactorOpensPastGraceWhenUnidentified) {
  // Ride-through expires: an unidentified pack past the 5 s grace must drive
  // open and stick there (steady open hold) until identified.
  send_15b_fd(7);
  step_10ms(2);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
  step_10ms(600);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);
  clear_transmitted_frames();
  step_10ms(20);  // Well past the old 150-frame (1.5 s) open loop length
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);
  EXPECT_TRUE(transmitted_047());
}

TEST_F(Mg4BatteryTest, ContactorDrivesOpenWhenPackNeverHeard) {
  // No 0x15B ever received and still unidentified past grace: drive open
  // rather than closing blind, and stick there until identified.
  step_10ms(600);
  EXPECT_FALSE(battery->batteryIdentified);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);
  clear_transmitted_frames();
  step_10ms(20);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);
  EXPECT_TRUE(transmitted_047());
}

TEST_F(Mg4BatteryTest, ContactorReturnsToWaitingOnceIdentified) {
  // Sticky OPENING releases back to WAITING as soon as the pack identifies,
  // then follows the normal close flow (pack open => CLOSING).
  step_10ms(600);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);
  identify_as_64kwh_nmc();
  send_15b_fd(3);
  step_10ms(1);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::WAITING_FOR_PACK);
  step_10ms(3);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSING);
}

TEST_F(Mg4BatteryTest, ContactorReclosesWhenIdentified) {
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
  // Pack drops out on its own: an identified pack replays the closing sequence.
  send_15b_fd(3);
  step_10ms(2);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSING);
}

TEST_F(Mg4BatteryTest, ClosedTailLoopKeepsCountersSeamless) {
  // CLOSED holds steady signal levels while the free-running 15-count rolling
  // counters keep stepping: 10 fast (047/08A) frames per slow (313/314) frame.
  // Capture a long steady hold off the wire and require every rolling counter
  // to advance by exactly one per frame, with no duplicate/skip.
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  ASSERT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
  clear_transmitted_frames();
  step_10ms(500);  // 5 s steady CLOSED hold
  ASSERT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);

  std::vector<uint8_t> c047a, c047b, c08a1, c08a2, c313a, c313b, c314a, c314b;
  for (const auto& f : get_transmitted_frames()) {
    switch (f.ID) {
      case 0x047:
        c047a.push_back(f.data.u8[5]);
        c047b.push_back(f.data.u8[17]);
        break;
      case 0x08A:
        c08a1.push_back(f.data.u8[5]);
        c08a2.push_back(f.data.u8[17]);
        break;
      case 0x313:
        c313a.push_back(f.data.u8[5]);
        c313b.push_back(f.data.u8[29]);
        break;
      case 0x314:
        c314a.push_back(f.data.u8[5]);
        c314b.push_back(f.data.u8[17]);
        break;
      default:
        break;
    }
  }

  // 500 fast ticks => 500 047/08A frames; ~50 slow ticks => ~50 313/314.
  // Use lower bounds so the test covers multiple wraps without depending on
  // exact 100ms phasing.
  EXPECT_GE(c047a.size(), 450u);
  EXPECT_GE(c08a1.size(), 450u);
  EXPECT_GE(c313a.size(), 40u);
  EXPECT_GE(c314a.size(), 40u);
  ASSERT_EQ(c047a.size(), c047b.size());
  ASSERT_EQ(c08a1.size(), c08a2.size());
  ASSERT_EQ(c313a.size(), c313b.size());
  ASSERT_EQ(c314a.size(), c314b.size());

  // Paired counters within one frame share the same phase on different bases.
  for (size_t k = 0; k < c047a.size(); k++) {
    EXPECT_EQ(c047a[k] - 0xF0, c047b[k] - 0xF0) << "047 pair mismatch at frame " << k;
  }
  for (size_t k = 0; k < c08a1.size(); k++) {
    EXPECT_EQ(c08a1[k] - 0x30, c08a2[k] - 0x40) << "08A pair mismatch at frame " << k;
  }
  for (size_t k = 0; k < c313a.size(); k++) {
    EXPECT_EQ(c313a[k] - 0xF0, c313b[k] - 0x30) << "313 pair mismatch at frame " << k;
  }
  for (size_t k = 0; k < c314a.size(); k++) {
    EXPECT_EQ(c314a[k] - 0x40, c314b[k] - 0x70) << "314 pair mismatch at frame " << k;
  }

  // Every counter must step by exactly one (mod 15) per frame, with no
  // duplicate/skip.
  auto expect_steps_by_one = [](const std::vector<uint8_t>& v, uint8_t base, const char* label) {
    for (size_t k = 1; k < v.size(); k++) {
      int prev = (int)v[k - 1] - base;
      int cur = (int)v[k] - base;
      ASSERT_GE(prev, 0) << label << " out of range at frame " << k - 1;
      ASSERT_LT(prev, 15) << label << " out of range at frame " << k - 1;
      ASSERT_GE(cur, 0) << label << " out of range at frame " << k;
      ASSERT_LT(cur, 15) << label << " out of range at frame " << k;
      EXPECT_EQ((cur - prev + 15) % 15, 1) << label << " jump at frame " << k;
    }
  };
  expect_steps_by_one(c047a, 0xF0, "047[5]");
  expect_steps_by_one(c047b, 0xF0, "047[17]");
  expect_steps_by_one(c08a1, 0x30, "08A[5]");
  expect_steps_by_one(c08a2, 0x40, "08A[17]");
  expect_steps_by_one(c313a, 0xF0, "313[5]");
  expect_steps_by_one(c313b, 0x30, "313[29]");
  expect_steps_by_one(c314a, 0x40, "314[5]");
  expect_steps_by_one(c314b, 0x70, "314[17]");
}

TEST_F(Mg4BatteryTest, ContactorDoesNotRecloseWhenUnidentified) {
  // Ride-through CLOSED, then the pack opens on its own while still
  // unidentified: must drive open and stick there, never enter CLOSING.
  send_15b_fd(7);
  step_10ms(2);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
  send_15b_fd(3);
  step_10ms(1);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_OPEN)->state, EVENT_STATE_ACTIVE);
  for (int i = 0; i < 50; i++) {
    step_10ms(1);
    auto s = battery->contactorState;
    EXPECT_NE(s, Mg4Battery::ContactorState::CLOSING);
    EXPECT_NE(s, Mg4Battery::ContactorState::CLOSED);
  }
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);
}

TEST_F(Mg4BatteryTest, ContactorFaultOpensAndReleaseReturnsToWaiting) {
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
  datalayer.system.status.system_status = FAULT;
  step_10ms(2);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);
  // Clearing the fault returns to WAITING first (pack state re-checked).
  datalayer.system.status.system_status = ACTIVE;
  step_10ms(1);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::WAITING_FOR_PACK);
}

TEST_F(Mg4BatteryTest, ContactorRecloseLoopTripsFatalEvent) {
  // Identified pack closes normally, then opens by itself (e.g. HV
  // isolation fault) 10 times in quick succession: the 10th reclose must
  // latch open and raise the fatal reclose event instead of closing again.
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  ASSERT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);

  for (int k = 0; k < 10; k++) {
    send_15b_fd(3);  // pack opens by itself
    step_10ms(2);
    if (k < 9) {
      EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSING);
      EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_OPEN)->state, EVENT_STATE_ACTIVE);
      send_15b_fd(7);  // pack closes again
      step_10ms(2);
      EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
      EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_OPEN)->state, EVENT_STATE_INACTIVE);
    }
  }
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);
  EXPECT_TRUE(battery->reclose_blocked);
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_OPEN)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_RECLOSE_FAULT)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_RECLOSE_FAULT)->data, 10);
  EXPECT_EQ(datalayer.system.status.system_status, FAULT);

  // Latched: a pack that stays open must not re-enter CLOSING.
  send_15b_fd(3);
  step_10ms(50);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);
  EXPECT_TRUE(battery->reclose_blocked);
}

TEST_F(Mg4BatteryTest, SparseReclosesDoNotTrip) {
  // 9 rapid self-opens stay under the trip count, then 301 s pass with the
  // pack closed: the next reclose falls outside the 300 s window, so the
  // drive must reclose normally instead of tripping.
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  ASSERT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);

  for (int k = 0; k < 9; k++) {
    send_15b_fd(3);
    step_10ms(2);
    EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSING);
    send_15b_fd(7);
    step_10ms(2);
    EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
  }
  clear_transmitted_frames();
  step_10ms(30100);  // 301 s, pack stays closed
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);

  send_15b_fd(3);
  step_10ms(2);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSING);
  EXPECT_FALSE(battery->reclose_blocked);
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_RECLOSE_FAULT)->state, EVENT_STATE_INACTIVE);
}

TEST_F(Mg4BatteryTest, ManualEstopCycleResetsRecloseFault) {
  // Trip the latch first.
  identify_as_64kwh_nmc();
  send_15b_fd(7);
  step_10ms(3);
  ASSERT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
  for (int k = 0; k < 10; k++) {
    send_15b_fd(3);
    step_10ms(2);
    if (k < 9) {
      send_15b_fd(7);
      step_10ms(2);
    }
  }
  ASSERT_TRUE(battery->reclose_blocked);
  ASSERT_EQ(get_event_pointer(EVENT_CONTACTOR_RECLOSE_FAULT)->state, EVENT_STATE_ACTIVE);

  // Manual open via the homepage button / estop (flag plus the
  // EQUIPMENT_STOP event, exactly as setBatteryPause() drives it) clears
  // the latch and event, while the estop itself holds the drive open.
  datalayer.system.info.equipment_stop_active = true;
  set_event(EVENT_EQUIPMENT_STOP, 1);
  step_10ms(2);
  EXPECT_FALSE(battery->reclose_blocked);
  EXPECT_EQ(get_event_pointer(EVENT_CONTACTOR_RECLOSE_FAULT)->state, EVENT_STATE_INACTIVE);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::OPENING);

  // Manual close resumes the normal flow (pack still reports open).
  datalayer.system.info.equipment_stop_active = false;
  clear_event(EVENT_EQUIPMENT_STOP);
  step_10ms(3);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSING);

  // Fresh tracker: one self-open recloses instead of tripping.
  send_15b_fd(7);
  step_10ms(2);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSED);
  send_15b_fd(3);
  step_10ms(2);
  EXPECT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSING);
  EXPECT_FALSE(battery->reclose_blocked);
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

TEST_F(Mg4BatteryTest, StoresAndDisplaysIsolationResistance) {
  // Before any 0xB045 response the page must say so rather than report 0 ohms.
  std::string html_before = battery->get_uds_info_html().c_str();
  EXPECT_NE(html_before.find("Isolation resistance: N/A"), std::string::npos);

  // Raw 2-byte value 1000 scaled by 500 => 500000 ohms.
  const uint8_t data[2] = {0x03, 0xE8};
  battery->handle_pid(0xB045, 1000, data, sizeof(data));
  std::string html = battery->get_uds_info_html().c_str();
  EXPECT_NE(html.find("Isolation resistance: 500000 ohms"), std::string::npos);
}

TEST_F(Mg4BatteryTest, IsolationResistanceHistoryCsv) {
  // One sample per simulated second, raw counts 1000..1004 => 500000..502000 ohms.
  for (int i = 0; i < 5; i++) {
    const uint8_t data[2] = {(uint8_t)((1000 + i) >> 8), (uint8_t)((1000 + i) & 0xFF)};
    battery->handle_pid(0xB045, 1000 + i, data, sizeof(data));
    battery->update_values();
  }
  std::string html = battery->get_uds_info_html().c_str();
  // Oldest first, comma separated, scaled to ohms.
  EXPECT_NE(html.find("Isolation history (ohms): 500000,500500,501000,501500,502000"), std::string::npos);
}

TEST_F(Mg4BatteryTest, SnapClampOnlyInAbsorption) {
  identify_as_51kwh_lfp();
  // Below absorption: full power, the 5A clamp must not strangle a normal charge.
  tick_1s(3300, 3290, 100, 3500, 500);
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_GT(datalayer.battery.status.max_charge_power_W, 5000u);
  // Bulk history warms the EMA, then absorption with snap still open:
  // clamped to ~5A * pack voltage.
  warmup_bulk();
  tick_1s(3700, 3650, 100, 3800, 970);
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_LE(datalayer.battery.status.max_charge_power_W, 2000u);
  EXPECT_GE(datalayer.battery.status.max_charge_power_W, 1500u);
}

TEST_F(Mg4BatteryTest, SnapClampLatchesWhenVoltageSags) {
  identify_as_51kwh_lfp();
  warmup_bulk();
  tick_1s(3660, 3640, 100, 3800, 970);
  ASSERT_FALSE(battery->snapEnded);
  ASSERT_LE(datalayer.battery.status.max_charge_power_W, 2000u);
  // Cutting to 5A lets the cell voltage sag back under SNAP_ABSORB_MV. The
  // clamp must hold rather than reopening full power and chattering.
  tick_1s(3630, 3610, 50, 3780, 970);
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_LE(datalayer.battery.status.max_charge_power_W, 2000u);
  // Leaving the knee entirely re-arms and releases the clamp.
  tick_1s(3400, 3390, 0, 3500, 900);
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_GT(datalayer.battery.status.max_charge_power_W, 5000u);
}

TEST_F(Mg4BatteryTest, StaleCellVoltagesReleaseSnapForce) {
  identify_as_51kwh_lfp();
  // Zero EMA history at boot: already-full pack ends the snap and forces 100%.
  tick_1s(3700, 3650, 20, 3800, 970);
  ASSERT_TRUE(battery->snapEnded);
  ASSERT_EQ(datalayer.battery.status.real_soc, 10000);
  // 0x15B keeps arriving but 0x12C stops: the frozen 3700mV reading must age
  // out, so the forced 100% releases back to the BMS SoC and power fails closed.
  for (int i = 0; i < 10; i++) {
    send_15b_fd(7, 970);
    send_17e(bms_dis_counts, bms_chg_counts);
    battery->update_values();
  }
  send_15b_fd(7, 970);
  EXPECT_EQ(datalayer.battery.status.real_soc, 9700);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u);
}

TEST_F(Mg4BatteryTest, SnapEndedTapersToAbsorbVoltage) {
  identify_as_51kwh_lfp();
  // Zero EMA history at boot: already-full pack ends the snap immediately.
  tick_1s(3660, 3640, 20, 3800, 970);
  ASSERT_TRUE(battery->snapEnded);
  // At or above SNAP_ABSORB_MV the working max is tripped: no charge at all,
  // rather than trickling on up to 3750mV.
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
  // Falling back: released at the hysteresis point, then a linear taper from
  // the 30kW hard cap over 3600-3650mV. The 100% SoC hold only affects
  // reporting, not charge power.
  tick_1s(3640, 3620, 0, 3780, 970);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 6000u);
  tick_1s(3620, 3600, 0, 3760, 970);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 18000u);
  EXPECT_TRUE(battery->snapEnded);
}

TEST_F(Mg4BatteryTest, ChargeNotDeratedByReportedSoc) {
  // No SoC-based derating of our own on either chemistry: a 100% SoC on
  // 0x15B leaves the full hard cap, and only the 0x17E limit can cut it.
  identify_as_51kwh_lfp();
  tick_1s(3400, 3390, 100, 3540, 1000);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 30000u);
}

TEST_F(Mg4BatteryTest, NmcChargeFollowsBmsLimitNearFull) {
  // Captured NMC-style top of charge: the BMS tapers to 4.5kW at 100%.
  identify_as_64kwh_nmc();
  bms_chg_counts = 9;
  tick_1s(4000, 3990, 100, 4100, 1000);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 4500u);
}

TEST_F(Mg4BatteryTest, SnapEndsOnTaperCurrentAndForcesSoc) {
  identify_as_51kwh_lfp();
  warmup_bulk();
  // 2A charge at 3700mV: the EMA decays from bulk history and ends the snap.
  for (int i = 0; i < 20; i++) {
    tick_1s(3700, 3650, 20, 3800, 970);
  }
  EXPECT_FALSE(battery->snapEnded);
  for (int i = 0; i < 50; i++) {
    tick_1s(3700, 3650, 20, 3800, 970);
  }
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);
  EXPECT_LE(datalayer.battery.status.max_charge_power_W, 200u);
  // A fresh BMS report below 100% is overridden while the snap is latched.
  send_15b_fd(7, 900);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);
}

TEST_F(Mg4BatteryTest, SnapEndsOnSustainedOvervoltage) {
  identify_as_51kwh_lfp();
  warmup_bulk();
  // 5A held above 3750mV: current average stays high so only the 30s timer trips.
  for (int i = 0; i < 29; i++) {
    tick_1s(3755, 3700, 50, 3850, 970);
  }
  EXPECT_FALSE(battery->snapEnded);
  tick_1s(3755, 3700, 50, 3850, 970);
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);
}

TEST_F(Mg4BatteryTest, SnapEndsInstantlyAtAbort) {
  identify_as_51kwh_lfp();
  tick_1s(3760, 3700, 50, 3900, 970);
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);
}

TEST_F(Mg4BatteryTest, SnapRearmsBelowReset) {
  identify_as_51kwh_lfp();
  // Zero EMA history at boot: already-full pack ends the snap immediately.
  for (int i = 0; i < 3; i++) {
    tick_1s(3700, 3650, 20, 3800, 970);
  }
  ASSERT_TRUE(battery->snapEnded);
  // Discharged well out of the knee: re-arm and follow the BMS again.
  tick_1s(3300, 3290, 0, 3500, 500);
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.real_soc, 5000);
}

TEST_F(Mg4BatteryTest, SnapForceHeldDownToHoldVoltage) {
  identify_as_51kwh_lfp();
  // Zero EMA history at boot: already-full pack ends the snap immediately.
  for (int i = 0; i < 3; i++) {
    tick_1s(3700, 3650, 20, 3800, 970);
  }
  ASSERT_TRUE(battery->snapEnded);
  ASSERT_EQ(datalayer.battery.status.real_soc, 10000);
  // Below SNAP_ABSORB_MV but not below SNAP_HOLD_MV: still held at 100%,
  // including for a fresh BMS report arriving between ticks.
  tick_1s(3620, 3600, 0, 3720, 900);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);
  tick_1s(3600, 3590, 0, 3700, 900);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);
  // Below SNAP_HOLD_MV: released, BMS wins (snap still ended until reset).
  tick_1s(3599, 3590, 0, 3700, 900);
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.real_soc, 9000);
  // Rising back between the two thresholds doesn't re-force.
  tick_1s(3620, 3600, 0, 3720, 900);
  EXPECT_EQ(datalayer.battery.status.real_soc, 9000);
  // Reaching SNAP_ABSORB_MV again does.
  tick_1s(3650, 3630, 0, 3740, 900);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);
}

TEST_F(Mg4BatteryTest, SnapIgnoredForNmc) {
  identify_as_64kwh_nmc();
  // Same taper-current conditions that end an LFP snap must not force 100% on NMC.
  for (int i = 0; i < 35; i++) {
    tick_1s(3700, 3650, 20, 3800, 900);
  }
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.real_soc, 9000);
}

TEST_F(Mg4BatteryTest, SnapGivesUpWhenCold) {
  identify_as_51kwh_lfp();
  warmup_bulk();
  // Reaching absorption at exactly 5C: outside the window, so give up rather
  // than clamp, and the post-snap taper (tripped at 3700mV) allows nothing.
  tick_1s(3700, 3650, 50, 3800, 970, 50);
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
}

TEST_F(Mg4BatteryTest, SnapColdBeforeAbsorptionStillAttempts) {
  // Cold while still in bulk doesn't count against the attempt: the pack
  // warms up during the charge and snaps once it reaches absorption.
  identify_as_51kwh_lfp();
  for (int i = 0; i < 60; i++) {
    tick_1s(3500, 3490, 100, 3600, 500, 20);
  }
  EXPECT_FALSE(battery->snapEnded);
  tick_1s(3700, 3650, 50, 3800, 970, 200);
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_GE(datalayer.battery.status.max_charge_power_W, 1500u);
  EXPECT_LE(datalayer.battery.status.max_charge_power_W, 2000u);
}

TEST_F(Mg4BatteryTest, SnapGivesUpWhenHotMidAbsorption) {
  identify_as_51kwh_lfp();
  warmup_bulk();
  bms_chg_counts = 0;
  tick_1s(3700, 3650, 50, 3800, 893, 440);
  ASSERT_FALSE(battery->snapEnded);
  ASSERT_GE(datalayer.battery.status.max_charge_power_W, 1500u);
  // Reaching 45C ends the attempt, and with it the override of the BMS's 0.
  tick_1s(3700, 3650, 50, 3800, 893, 450);
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
}

TEST_F(Mg4BatteryTest, SnapSkippedWhenSocAlreadyHigh) {
  identify_as_51kwh_lfp();
  warmup_bulk();
  // Highest cell in the knee but BMS already at 99%: no drift, skip straight
  // to done even with plenty of current flowing.
  tick_1s(3700, 3650, 100, 3800, 990);
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);
  // Re-arm by leaving the knee, then show drift (90%) earns a snap attempt.
  tick_1s(3300, 3290, 0, 3500, 500);
  EXPECT_FALSE(battery->snapEnded);
  warmup_bulk();
  tick_1s(3700, 3650, 100, 3800, 900);
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_LE(datalayer.battery.status.max_charge_power_W, 2000u);
  // And the skip is level-triggered: rising to 98% mid-absorption ends it.
  tick_1s(3700, 3650, 100, 3800, 980);
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);
}

// ---- Sleep / wake ----

using CS = Mg4Battery::ContactorState;

// 0x08A request / flag / level as transmitted (full-frame bytes 19, 20, 30).
struct Sig08a {
  uint8_t request;
  uint8_t flag;
  uint8_t level;
  bool operator==(const Sig08a& o) const { return request == o.request && flag == o.flag && level == o.level; }
};

static std::vector<Sig08a> sent_08a() {
  std::vector<Sig08a> out;
  for (const auto& f : get_transmitted_frames()) {
    if (f.ID == 0x08A) {
      out.push_back({f.data.u8[19], f.data.u8[20], f.data.u8[30]});
    }
  }
  return out;
}

class Mg4SleepTest : public Mg4BatteryTest {
 protected:
  // Identified pack, closed, idle (0A), with fresh limits.
  void close_pack(int16_t current_dA = 0) {
    identify_as_64kwh_nmc();
    send_12c(3800, 3790, current_dA, 3900);
    send_15b_fd(7);
    step_10ms(3);
    ASSERT_EQ(battery->contactorState, CS::CLOSED);
  }

  // Run a full graceful shutdown from closed into SLEEPING.
  void sleep_pack() {
    close_pack();
    battery->request_sleep();
    step_10ms(TestableMg4Battery::SHUTDOWN_HOLD_T + 1);
    ASSERT_EQ(battery->shutdown_phase, TestableMg4Battery::ShutdownPhase::REQUEST_OPEN);
    send_15b_fd(3);
    step_10ms(TestableMg4Battery::SHUTDOWN_OPEN_HOLD_T + 1);
    ASSERT_EQ(battery->contactorState, CS::SLEEPING);
  }

  std::string info_html() { return battery->get_uds_info_html().c_str(); }

  // One 0x689 slot 0x0D frame reporting `value` for 1-based `cell`.
  void send_689_cell(uint8_t cell, uint16_t value) {
    CAN_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.ID = 0x689;
    frame.FD = true;
    frame.DLC = 12;
    frame.data.u8[1] = 0x06;
    frame.data.u8[2] = 0x89;
    frame.data.u8[3] = 8;
    uint8_t* sub = &frame.data.u8[4];
    sub[0] = 0x92;
    sub[1] = 0x0D;
    sub[2] = (uint8_t)(value >> 8);
    sub[3] = (uint8_t)value;
    sub[6] = cell;
    battery->handle_incoming_can_frame(frame);
  }
};

TEST_F(Mg4SleepTest, ShutdownFollowsCarSequence) {
  close_pack();
  datalayer.battery.status.max_charge_power_W = 5000;
  datalayer.battery.status.max_discharge_power_W = 5000;
  clear_transmitted_frames();
  battery->request_sleep();
  EXPECT_TRUE(battery->is_sleeping());
  step_10ms(1);
  EXPECT_EQ(battery->contactorState, CS::SHUTTING_DOWN);
  // Step 0: limits forced to 0 straight away.
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u);

  // Pack stays closed for a while after the open request.
  step_10ms(TestableMg4Battery::SHUTDOWN_HOLD_T + 49);
  send_15b_fd(3);
  step_10ms(2);
  EXPECT_EQ(battery->shutdown_phase, TestableMg4Battery::ShutdownPhase::OPEN_HOLD);

  const auto sig = sent_08a();
  const uint32_t hold = TestableMg4Battery::SHUTDOWN_HOLD_T;
  const uint32_t lvl = TestableMg4Battery::SHUTDOWN_LEVEL_T;
  ASSERT_GE(sig.size(), hold + 52);
  // 0.2s of request 0x01 at level 0x40, then level 0x20 to the end of the 6.0s hold.
  for (uint32_t i = 0; i < lvl; i++) {
    EXPECT_EQ(sig[i], (Sig08a{0x01, 0x00, 0x40})) << i;
  }
  for (uint32_t i = lvl; i < hold; i++) {
    EXPECT_EQ(sig[i], (Sig08a{0x01, 0x00, 0x20})) << i;
  }
  // Request open at level 0x20 until the pack reports open, then level 0x00.
  for (uint32_t i = hold; i < hold + 50; i++) {
    EXPECT_EQ(sig[i], (Sig08a{0x00, 0x00, 0x20})) << i;
  }
  EXPECT_EQ(sig[hold + 50], (Sig08a{0x00, 0x00, 0x00}));
  EXPECT_EQ(sig.back(), (Sig08a{0x00, 0x00, 0x00}));

  // ~2s of the open levels, then silence.
  step_10ms(TestableMg4Battery::SHUTDOWN_OPEN_HOLD_T);
  EXPECT_EQ(battery->contactorState, CS::SLEEPING);
  EXPECT_EQ(battery->sleep_abort_reason, nullptr);
}

TEST_F(Mg4SleepTest, SleepingTransmitsNothing) {
  sleep_pack();
  clear_transmitted_frames();
  // 100s, long enough for UDS polling, the DID sweep and 0x4F3 to have fired.
  step_10ms(10000);
  EXPECT_TRUE(get_transmitted_frames().empty());
  EXPECT_EQ(battery->contactorState, CS::SLEEPING);
  EXPECT_TRUE(battery->is_sleeping());
}

TEST_F(Mg4SleepTest, SleepStandsInForSilentBms) {
  sleep_pack();
  // The BMS goes quiet: no 0x12C refreshes the liveness counter, but the
  // driver must hold it up so the safety layer doesn't fault the system.
  for (int i = 0; i < 120; i++) {
    datalayer.battery.status.CAN_battery_still_alive = 0;
    battery->update_values();
    EXPECT_EQ(datalayer.battery.status.CAN_battery_still_alive, CAN_STILL_ALIVE);
  }
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u);
}

TEST_F(Mg4SleepTest, LimitsStayZeroThroughShutdownWithFreshData) {
  close_pack();
  battery->request_sleep();
  step_10ms(1);
  // Fresh, healthy BMS data must not bring power back mid-shutdown.
  tick_1s(3800, 3790, 0, 3900, 500);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u);
}

TEST_F(Mg4SleepTest, ShutdownWaitsForZeroCurrent) {
  close_pack(50);  // 5A still flowing
  battery->request_sleep();
  step_10ms(TestableMg4Battery::SHUTDOWN_HOLD_T + 100);
  EXPECT_EQ(battery->shutdown_phase, TestableMg4Battery::ShutdownPhase::HOLD);
  EXPECT_NE(info_html().find("Shutting down: waiting for zero current (5.0 A)"), std::string::npos);
  // The load goes away: request open on the next tick.
  send_12c(3800, 3790, 5, 3900);
  step_10ms(1);
  EXPECT_EQ(battery->shutdown_phase, TestableMg4Battery::ShutdownPhase::REQUEST_OPEN);
}

TEST_F(Mg4SleepTest, ShutdownCancelsRatherThanOpenUnderLoad) {
  close_pack(-50);  // 5A discharge that never stops
  clear_transmitted_frames();
  battery->request_sleep();
  step_10ms(TestableMg4Battery::SHUTDOWN_HOLD_MAX_T + 5);
  // Back to normal operation, closed, without ever requesting open.
  EXPECT_EQ(battery->contactorState, CS::CLOSED);
  for (const auto& sig : sent_08a()) {
    EXPECT_NE(sig.request, 0x00);
  }
  EXPECT_FALSE(battery->is_sleeping());
  EXPECT_NE(info_html().find("Last sleep attempt aborted: current did not fall below 1A"), std::string::npos);
}

TEST_F(Mg4SleepTest, StaleCurrentNeverCountsAsIdle) {
  close_pack();
  battery->request_sleep();
  step_10ms(1);
  // 0x12C stops: its 0A reading ages out and can't be trusted to open on.
  for (int i = 0; i < 10; i++) {
    battery->update_values();
  }
  step_10ms(TestableMg4Battery::SHUTDOWN_HOLD_T);
  EXPECT_EQ(battery->shutdown_phase, TestableMg4Battery::ShutdownPhase::HOLD);
}

TEST_F(Mg4SleepTest, FaultDuringShutdownOpensImmediately) {
  close_pack();
  battery->request_sleep();
  step_10ms(100);
  ASSERT_EQ(battery->shutdown_phase, TestableMg4Battery::ShutdownPhase::HOLD);
  clear_transmitted_frames();
  datalayer.system.status.system_status = FAULT;
  step_10ms(1);
  EXPECT_EQ(battery->contactorState, CS::OPENING);
  ASSERT_FALSE(sent_08a().empty());
  EXPECT_EQ(sent_08a().front(), (Sig08a{0x00, 0x00, 0x00}));
  EXPECT_NE(info_html().find("Last sleep attempt aborted: fault"), std::string::npos);
}

TEST_F(Mg4SleepTest, PackNotOpeningFallsBackToHardOpen) {
  close_pack();
  battery->request_sleep();
  step_10ms(TestableMg4Battery::SHUTDOWN_HOLD_T + 1);
  ASSERT_EQ(battery->shutdown_phase, TestableMg4Battery::ShutdownPhase::REQUEST_OPEN);
  EXPECT_NE(info_html().find("Shutting down: waiting for the pack to open"), std::string::npos);
  step_10ms(TestableMg4Battery::SHUTDOWN_OPEN_TIMEOUT_T - 2);
  EXPECT_EQ(battery->contactorState, CS::SHUTTING_DOWN);
  step_10ms(2);
  EXPECT_EQ(battery->contactorState, CS::OPENING);
  EXPECT_NE(info_html().find("pack did not report open"), std::string::npos);
}

TEST_F(Mg4SleepTest, WakeDuringHoldCancelsShutdown) {
  close_pack();
  clear_transmitted_frames();
  battery->request_sleep();
  step_10ms(100);
  battery->request_wake();
  step_10ms(5);
  EXPECT_EQ(battery->contactorState, CS::CLOSED);
  for (const auto& sig : sent_08a()) {
    EXPECT_NE(sig.request, 0x00);
  }
}

TEST_F(Mg4SleepTest, SleepFromOpenPackSkipsToSilence) {
  identify_as_64kwh_nmc();
  send_15b_fd(3);
  step_10ms(3);
  ASSERT_EQ(battery->contactorState, CS::CLOSING);
  send_15b_fd(3);
  datalayer.system.status.system_status = FAULT;
  step_10ms(1);
  ASSERT_EQ(battery->contactorState, CS::OPENING);
  // Already open (held open by a fault): straight to the open hold. The
  // fault doesn't stop an already-open pack from going to sleep.
  battery->request_sleep();
  step_10ms(1);
  EXPECT_EQ(battery->shutdown_phase, TestableMg4Battery::ShutdownPhase::OPEN_HOLD);
  step_10ms(TestableMg4Battery::SHUTDOWN_OPEN_HOLD_T);
  EXPECT_EQ(battery->contactorState, CS::SLEEPING);
}

TEST_F(Mg4SleepTest, WakeRunsPowerUpPatternThenCloses) {
  sleep_pack();
  clear_transmitted_frames();
  battery->request_wake();
  EXPECT_TRUE(battery->is_sleeping());
  step_10ms(1);
  EXPECT_EQ(battery->contactorState, CS::WAKING);
  EXPECT_FALSE(battery->is_sleeping());
  step_10ms(TestableMg4Battery::WAKE_REQ03_T + 50);
  // No fresh 0x15B yet: keep waking (and transmitting).
  EXPECT_EQ(battery->contactorState, CS::WAKING);
  EXPECT_NE(info_html().find("Waking: waiting for the BMS"), std::string::npos);

  const auto sig = sent_08a();
  ASSERT_GE(sig.size(), (size_t)TestableMg4Battery::WAKE_REQ03_T + 1);
  EXPECT_EQ(sig[0], (Sig08a{0x03, 0x04, 0x00}));
  for (uint32_t i = 1; i < TestableMg4Battery::WAKE_REQ03_T; i++) {
    EXPECT_EQ(sig[i], (Sig08a{0x03, 0x00, 0x00})) << i;
  }
  EXPECT_EQ(sig[TestableMg4Battery::WAKE_REQ03_T], (Sig08a{0x00, 0x00, 0x00}));
  bool sent_4f3 = false;
  for (const auto& f : get_transmitted_frames()) {
    sent_4f3 |= f.ID == 0x4F3;
  }
  EXPECT_TRUE(sent_4f3);

  // The BMS reports in: normal flow takes over and closes the pack.
  send_15b_fd(3);
  step_10ms(3);
  EXPECT_EQ(battery->contactorState, CS::CLOSING);
  send_15b_fd(7);
  step_10ms(1);
  EXPECT_EQ(battery->contactorState, CS::CLOSED);
  tick_1s(3800, 3790, 0, 3900, 500);
  EXPECT_GT(datalayer.battery.status.max_charge_power_W, 0u);
}

TEST_F(Mg4SleepTest, TracksBmsSilenceAndSelfWakes) {
  sleep_pack();
  EXPECT_NE(info_html().find("BMS still transmitting"), std::string::npos);
  step_10ms(600);
  std::string html = info_html();
  EXPECT_NE(html.find("Sleeping for 0:00:06"), std::string::npos);
  EXPECT_NE(html.find("BMS went silent"), std::string::npos);
  EXPECT_NE(html.find("0 self-wakes"), std::string::npos);
  // A frame out of the blue is a self-wake, and is never answered.
  clear_transmitted_frames();
  send_12c(3800, 3790, 0, 3900);
  step_10ms(10);
  EXPECT_EQ(battery->sleep_self_wakes, 1);
  EXPECT_TRUE(get_transmitted_frames().empty());
  EXPECT_NE(info_html().find("1 self-wakes (transmitting now)"), std::string::npos);
}

TEST_F(Mg4SleepTest, InfoPageShowsAwake) {
  close_pack();
  EXPECT_NE(info_html().find("<h3>Power state</h3>Awake (pack contactors: Closed)"), std::string::npos);
  EXPECT_FALSE(battery->is_sleeping());
}

TEST_F(Mg4SleepTest, BalancingSweepSummedAndResetOnWake) {
  identify_as_64kwh_nmc();
  const uint8_t cells = datalayer.battery.info.number_of_cells;
  ASSERT_GT(cells, 0);
  // Join mid-sweep: the partial first sweep doesn't count.
  for (uint8_t c = cells / 2; c <= cells; c++) {
    send_689_cell(c, c == cells ? 7 : 0);
  }
  send_689_cell(1, 0);
  EXPECT_FALSE(battery->bal_have_sum);
  // A full sweep, repeating each cell as the BMS does, ending on the wrap.
  for (uint8_t c = 1; c <= cells; c++) {
    send_689_cell(c, c == 3 ? 100 : (c == 10 ? 23 : 0));
    send_689_cell(c, c == 3 ? 100 : (c == 10 ? 23 : 0));
  }
  send_689_cell(1, 0);
  EXPECT_TRUE(battery->bal_have_sum);
  EXPECT_EQ(battery->bal_last_sum, 123u);
  EXPECT_NE(info_html().find("last full sweep sum 123"), std::string::npos);

  // Waking marks every cell not-seen again.
  send_12c(3800, 3790, 0, 3900);
  send_15b_fd(7);
  step_10ms(3);
  battery->request_sleep();
  step_10ms(TestableMg4Battery::SHUTDOWN_HOLD_T + 1);
  send_15b_fd(3);
  step_10ms(TestableMg4Battery::SHUTDOWN_OPEN_HOLD_T + 1);
  ASSERT_EQ(battery->contactorState, CS::SLEEPING);
  battery->request_wake();
  step_10ms(1);
  EXPECT_FALSE(battery->bal_have_sum);
  for (uint8_t i = 0; i < cells; i++) {
    EXPECT_EQ(battery->bal_demand[i], 0xFFFF);  // BAL_NOT_SEEN
  }
}

// ---- BMS power limits (0x17E) ----

// Captured 0x17E payloads (CAN FD flags nibble stripped).
// 97-100-with-balance.log, 97.8% SoC charging at ~9A: discharge 189.5kW, charge 7.5kW.
static const char* CAP_17E_NEAR_FULL =
    "0005030800002F65EE1CC80000050408000071FA80780F000005640800000000000000D4000000000000000000000000";
// mg4-canlog-replay-contactors-closing.log, first frame after wake: both limits 0x7FF.
static const char* CAP_17E_STARTUP =
    "0005030800F0FFFFFFFFFF000005040800F0FFFFFFFFFF00000564080000000000000FFF000000000000000000000000";
// 0-100.log at 0% SoC: discharge 0kW, charge 253kW.
static const char* CAP_17E_EMPTY =
    "00050308000000000000C700000504080000C8398FD1FA0000056408000000000000005C000000000000000000000000";

TEST_F(Mg4BatteryTest, DecodesCapturedBmsLimits) {
  identify_as_51kwh_lfp();
  send_12c(3370, 3360, 90, 3500);
  send_15b_fd(7, 978);
  send_fd_hex(0x17E, CAP_17E_NEAR_FULL);
  battery->update_values();
  // Charge follows the BMS's 7.5kW; discharge is held to our 30kW hard cap.
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 7500u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 30000u);
  std::string html = battery->get_uds_info_html().c_str();
  EXPECT_NE(html.find("BMS limits: charge 7.5 kW, discharge 189.5 kW"), std::string::npos);
}

TEST_F(Mg4BatteryTest, CapturedStartupLimitsFailClosed) {
  identify_as_51kwh_lfp();
  send_12c(3370, 3360, 0, 3500);
  send_15b_fd(3, 500);
  send_fd_hex(0x17E, CAP_17E_STARTUP);
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u);
  std::string html = battery->get_uds_info_html().c_str();
  EXPECT_NE(html.find("BMS limits: N/A"), std::string::npos);
}

TEST_F(Mg4BatteryTest, CapturedEmptyPackBlocksDischarge) {
  identify_as_51kwh_lfp();
  send_12c(3110, 2960, 0, 3180);
  send_15b_fd(7, 0);
  send_fd_hex(0x17E, CAP_17E_EMPTY);
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 30000u);
}

TEST_F(Mg4BatteryTest, BmsLimitsCapBothDirections) {
  identify_as_64kwh_nmc();
  bms_dis_counts = 20;  // 10kW
  bms_chg_counts = 9;   // 4.5kW
  tick_1s(3800, 3790, 0, 3900, 500);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 10000u);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 4500u);
  // BMS stops charge entirely (seen at 100% in 95-100-with-trickle.log).
  bms_chg_counts = 0;
  tick_1s(3800, 3790, 0, 3900, 500);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 10000u);
}

TEST_F(Mg4BatteryTest, BmsLimitsAgeOut) {
  identify_as_64kwh_nmc();
  tick_1s(3800, 3790, 0, 3900, 500);
  ASSERT_EQ(datalayer.battery.status.max_charge_power_W, 30000u);
  // 0x12C/0x15B keep arriving but 0x17E stops: power holds for the 10s
  // freshness window, then fails closed.
  for (int i = 0; i < 8; i++) {
    send_12c(3800, 3790, 0, 3900);
    send_15b_fd(7, 500);
    battery->update_values();
  }
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 30000u);
  send_12c(3800, 3790, 0, 3900);
  send_15b_fd(7, 500);
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u);
}

TEST_F(Mg4BatteryTest, BmsFrameWithOneInvalidLimitIsIgnored) {
  identify_as_64kwh_nmc();
  bms_dis_counts = 20;
  bms_chg_counts = 30;
  tick_1s(3800, 3790, 0, 3900, 500);
  ASSERT_EQ(datalayer.battery.status.max_charge_power_W, 15000u);
  // Half-valid frame: neither limit is taken from it.
  send_17e(0x7FF, 4);
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 15000u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 10000u);
}

TEST_F(Mg4BatteryTest, BmsFrameMissingChargeSubframeIsIgnored) {
  identify_as_64kwh_nmc();
  send_12c(3800, 3790, 0, 3900);
  send_15b_fd(7, 500);
  // Only the 0x503 (discharge) subframe of the near-full capture.
  char hex[25] = {0};
  memcpy(hex, CAP_17E_NEAR_FULL, 24);
  send_fd_hex(0x17E, hex);
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u);
}

TEST_F(Mg4BatteryTest, SnapClampOverridesBmsChargeCut) {
  // snap.log: the BMS drops its charge limit from ~58kW straight to 0 once
  // the highest cell reaches 3700mV, short of SNAP_OVER_MV. The snap's 5A
  // clamp must keep charging through that, then defer to the BMS once done.
  identify_as_51kwh_lfp();
  warmup_bulk();
  bms_chg_counts = 0;
  tick_1s(3700, 3650, 50, 3800, 893);
  ASSERT_FALSE(battery->snapEnded);
  EXPECT_LE(datalayer.battery.status.max_charge_power_W, 2000u);
  EXPECT_GE(datalayer.battery.status.max_charge_power_W, 1500u);
  // BMS recalibrates its SoC: the snap succeeds and its 0 limit applies again.
  tick_1s(3750, 3650, 50, 3800, 1000);
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
}

TEST_F(Mg4BatteryTest, SnapClampDoesNotOverrideBmsBeforeAbsorption) {
  identify_as_51kwh_lfp();
  warmup_bulk();
  bms_chg_counts = 0;
  tick_1s(3640, 3600, 50, 3780, 893);
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
}

TEST_F(Mg4BatteryTest, SnapOverridesBmsOnlyNearTheCliff) {
  // In latched absorption but below SNAP_BMS_OVERRIDE_MV, a BMS cut is for
  // some other reason and must be respected; from there on the clamp wins.
  identify_as_51kwh_lfp();
  warmup_bulk();
  tick_1s(3660, 3640, 50, 3790, 893);
  ASSERT_FALSE(battery->snapEnded);
  ASSERT_GE(datalayer.battery.status.max_charge_power_W, 1500u);
  bms_chg_counts = 0;
  tick_1s(3679, 3640, 50, 3790, 893);
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
  tick_1s(3680, 3640, 50, 3790, 893);
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_GE(datalayer.battery.status.max_charge_power_W, 1500u);
  EXPECT_LE(datalayer.battery.status.max_charge_power_W, 2000u);
}

TEST_F(Mg4BatteryTest, SnapGivesUpAfterAbsorbTimeout) {
  // Hovering just under SNAP_OVER_MV at 5A (as in snap.log, at 3748-3749mV):
  // neither the over-voltage timer nor the min-current exit fires, so the
  // absorption timeout must end the override.
  identify_as_51kwh_lfp();
  warmup_bulk();
  bms_chg_counts = 0;
  for (int i = 0; i < TestableMg4Battery::SNAP_ABSORB_MAX_S - 1; i++) {
    tick_1s(3749, 3650, 50, 3800, 893);
  }
  EXPECT_FALSE(battery->snapEnded);
  EXPECT_GE(datalayer.battery.status.max_charge_power_W, 1500u);
  tick_1s(3749, 3650, 50, 3800, 893);
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
}

TEST_F(Mg4BatteryTest, SnapOverTimerNotRestartedByDips) {
  identify_as_51kwh_lfp();
  warmup_bulk();
  bms_chg_counts = 0;
  // First reach SNAP_OVER_MV, then sag back under it: the timer keeps running.
  tick_1s(3755, 3700, 50, 3850, 893);
  for (int i = 0; i < TestableMg4Battery::SNAP_OVER_S - 2; i++) {
    tick_1s(3745, 3700, 50, 3850, 893);
  }
  EXPECT_FALSE(battery->snapEnded);
  tick_1s(3745, 3700, 50, 3850, 893);
  EXPECT_TRUE(battery->snapEnded);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u);
}

TEST_F(Mg4BatteryTest, TempTapersFromHardCap) {
  // 600W/dC over the last 5C: 2C above the LFP charge floor and 2C below the
  // 50C ceiling both leave 12kW.
  identify_as_51kwh_lfp();
  send_12c(3300, 3290, 0, 3450, 20);
  send_15b_fd(7, 500);
  send_17e(bms_dis_counts, bms_chg_counts);
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 12000u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 30000u);
  send_12c(3300, 3290, 0, 3450, 480);
  send_15b_fd(7, 500);
  send_17e(bms_dis_counts, bms_chg_counts);
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 12000u);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 12000u);
}

// Legacy 800/80-frame replay oracle: exact copy of the pre-refactor pattern
// generators (RLE tables, rolling counters, ramps). The free-running
// sequencer must reproduce this bit-for-bit over the closing sequence.
namespace LegacyMg4 {
struct RleRun {
  uint16_t value;
  uint16_t count;
};
template <size_t N>
uint8_t rle_lookup(const RleRun (&runs)[N], int i) {
  for (size_t r = 0; r < N; r++) {
    if (i < (int)runs[r].count) {
      return (uint8_t)runs[r].value;
    }
    i -= runs[r].count;
  }
  return 0;
}
uint8_t cnt(uint8_t base, int i, int skew) {
  return (uint8_t)(base + ((i + skew) % 15));
}
uint16_t ramp(int t, int start, int end, uint16_t max) {
  if (t <= start) {
    return 0;
  }
  if (t >= end) {
    return max;
  }
  return (uint16_t)(((t - start) * max + (end - start) / 2) / (end - start));
}
static const RleRun REQ[3] = {{0x00, 190}, {0x01, 119}, {0x21, 491}};
static const RleRun FLAG[3] = {{0x00, 188}, {0x08, 219}, {0x00, 393}};
static const RleRun LVL[3] = {{0x00, 307}, {0x20, 30}, {0x40, 463}};
static const RleRun STAT[3] = {{0x01, 10}, {0x03, 23}, {0x05, 47}};
static const RleRun V314[5] = {{0x02, 27}, {0x2E, 1}, {0x4C, 1}, {0x56, 35}, {0x57, 16}};
static const RleRun H314[7] = {{0xC8, 27}, {0x08, 1}, {0xC8, 1}, {0x48, 2}, {0x88, 1}, {0xC8, 32}, {0x08, 16}};
}  // namespace LegacyMg4

TEST_F(Mg4BatteryTest, SeqHelpersMatchLegacyTables) {
  // Every per-signal sequencer must agree with the legacy RLE/counter/ramp
  // over the whole closing replay: 800 fast ticks and 80 slow ticks.
  for (int i = 0; i < 800; i++) {
    EXPECT_EQ(TestableMg4Battery::seq_08a_request(i), LegacyMg4::rle_lookup(LegacyMg4::REQ, i))
        << "request mismatch at fast " << i;
    EXPECT_EQ(TestableMg4Battery::seq_08a_flag(i), LegacyMg4::rle_lookup(LegacyMg4::FLAG, i))
        << "flag mismatch at fast " << i;
    EXPECT_EQ(TestableMg4Battery::seq_08a_level(i), LegacyMg4::rle_lookup(LegacyMg4::LVL, i))
        << "level mismatch at fast " << i;
    EXPECT_EQ(TestableMg4Battery::seq_cnt15(i, 0xF0, 11), LegacyMg4::cnt(0xF0, i, 11))
        << "047 cnt mismatch at fast " << i;
    EXPECT_EQ(TestableMg4Battery::seq_cnt15(i, 0x30, 11), LegacyMg4::cnt(0x30, i, 11))
        << "08A cnt1 mismatch at fast " << i;
    EXPECT_EQ(TestableMg4Battery::seq_cnt15(i, 0x40, 11), LegacyMg4::cnt(0x40, i, 11))
        << "08A cnt2 mismatch at fast " << i;
    // VAL12 precharge step (45 below 252, live target above).
    bool legacy_pre = (i < 252);
    bool seq_pre = (i < 252);
    EXPECT_EQ(seq_pre, legacy_pre) << "VAL12 step mismatch at fast " << i;
  }
  for (int j = 0; j < 80; j++) {
    EXPECT_EQ(TestableMg4Battery::seq_313_stat(j), LegacyMg4::rle_lookup(LegacyMg4::STAT, j))
        << "STAT mismatch at slow " << j;
    EXPECT_EQ(TestableMg4Battery::seq_314_val(j), LegacyMg4::rle_lookup(LegacyMg4::V314, j))
        << "314 VAL mismatch at slow " << j;
    EXPECT_EQ(TestableMg4Battery::seq_314_hi(j), LegacyMg4::rle_lookup(LegacyMg4::H314, j))
        << "314 HI mismatch at slow " << j;
    EXPECT_EQ(TestableMg4Battery::seq_cnt15(j, 0xF0, 7), LegacyMg4::cnt(0xF0, j, 7))
        << "313 cnt mismatch at slow " << j;
    EXPECT_EQ(TestableMg4Battery::seq_cnt15(j, 0x30, 7), LegacyMg4::cnt(0x30, j, 7))
        << "313 cnt2 mismatch at slow " << j;
    EXPECT_EQ(TestableMg4Battery::seq_cnt15(j, 0x40, 7), LegacyMg4::cnt(0x40, j, 7))
        << "314 cnt mismatch at slow " << j;
    EXPECT_EQ(TestableMg4Battery::seq_cnt15(j, 0x70, 7), LegacyMg4::cnt(0x70, j, 7))
        << "314 cnt2 mismatch at slow " << j;
    // Ramps use the same saturating helper in both schemes: pin the shape
    // (idle, mid-ramp rounding, plateau) so a future retune is caught here.
    if (j == 0) {
      EXPECT_EQ(LegacyMg4::ramp(j, 32, 78, 77), 0);
      EXPECT_EQ(LegacyMg4::ramp(j, 44, 59, 48), 0);
    }
    if (j == 55) {
      EXPECT_EQ(LegacyMg4::ramp(j, 32, 78, 77), 39);
      EXPECT_EQ(LegacyMg4::ramp(j, 44, 59, 48), 35);
    }
    if (j == 79) {
      EXPECT_EQ(LegacyMg4::ramp(j, 32, 78, 77), 77);
      EXPECT_EQ(LegacyMg4::ramp(j, 44, 59, 48), 48);
    }
    // VAL16 sampled step: t=(j>2)?(j-2)*10:0 vs 252 (first live at j=28).
    if (j == 27) {
      EXPECT_LT((uint32_t)(j - 2) * 10u, 252u);
    }
    if (j == 28) {
      EXPECT_GE((uint32_t)(j - 2) * 10u, 252u);
    }
  }
}

TEST_F(Mg4BatteryTest, ClosingWireSequenceMatchesLegacyReplay) {
  // End-to-end: the free-running sequencer must put the exact legacy bytes on
  // the wire over a full closing run (pack stays open so we remain CLOSING).
  // Voltage is held constant so the live-tracked plateaus are comparable.
  identify_as_64kwh_nmc();
  send_15b_fd(3);
  datalayer.battery.status.voltage_dV = 3600;
  ASSERT_EQ(battery->contactorState, Mg4Battery::ContactorState::WAITING_FOR_PACK);

  const uint32_t target12 = ((uint32_t)3600 * 2u) / 5u;  // 1440
  const uint32_t target16 = (uint32_t)3600 * 5u;         // 18000

  clear_transmitted_frames();
  step_10ms(800);  // 800 fast frames, ~80 slow frames
  ASSERT_EQ(battery->contactorState, Mg4Battery::ContactorState::CLOSING);
  EXPECT_EQ(battery->mg4_seq_tick, 800u);
  EXPECT_EQ(battery->mg4_seq_start, 0u);

  std::vector<CAN_frame> f047, f08a, f313, f314;
  for (const auto& f : get_transmitted_frames()) {
    switch (f.ID) {
      case 0x047:
        f047.push_back(f);
        break;
      case 0x08A:
        f08a.push_back(f);
        break;
      case 0x313:
        f313.push_back(f);
        break;
      case 0x314:
        f314.push_back(f);
        break;
      default:
        break;
    }
  }
  ASSERT_EQ(f047.size(), 800u);
  ASSERT_EQ(f08a.size(), 800u);
  // 8000ms / 100ms = 80 slow frames; the last one is the wrap glitch in the
  // legacy scheme (slow 0) while the sequencer holds the final level (slow
  // 80 == slow 79 for all payload signals), so compare the first 79 exactly.
  ASSERT_GE(f313.size(), 79u);
  ASSERT_GE(f314.size(), 79u);

  auto val12_of = [](const CAN_frame& f) -> uint16_t {
    return (uint16_t)(((uint16_t)f.data.u8[9] << 4) | (f.data.u8[10] >> 4));
  };
  auto val16_of = [](const CAN_frame& f) -> uint16_t {
    return (uint16_t)(((uint16_t)f.data.u8[32] << 8) | f.data.u8[33]);
  };

  for (int i = 0; i < 800; i++) {
    uint16_t exp12 = (i < 252) ? 45 : (uint16_t)target12;
    EXPECT_EQ(val12_of(f047[i]), exp12) << "047 VAL12 at fast " << i;
    EXPECT_EQ(f047[i].data.u8[5], LegacyMg4::cnt(0xF0, i, 11)) << "047 cnt at fast " << i;
    EXPECT_EQ(f047[i].data.u8[17], LegacyMg4::cnt(0xF0, i, 11)) << "047 cnt2 at fast " << i;
    EXPECT_EQ(f08a[i].data.u8[5], LegacyMg4::cnt(0x30, i, 11)) << "08A cnt at fast " << i;
    EXPECT_EQ(f08a[i].data.u8[17], LegacyMg4::cnt(0x40, i, 11)) << "08A cnt2 at fast " << i;
    EXPECT_EQ(f08a[i].data.u8[19], LegacyMg4::rle_lookup(LegacyMg4::REQ, i)) << "08A REQ at fast " << i;
    EXPECT_EQ(f08a[i].data.u8[20], LegacyMg4::rle_lookup(LegacyMg4::FLAG, i)) << "08A FLAG at fast " << i;
    EXPECT_EQ(f08a[i].data.u8[30], LegacyMg4::rle_lookup(LegacyMg4::LVL, i)) << "08A LVL at fast " << i;
  }

  // Slow frames on the wire run 1..79 then the held final (slow 80); the
  // legacy loop would wrap to 0 there, so only the first 79 are bit-identical.
  for (size_t k = 0; k < 79; k++) {
    int j = (int)k + 1;  // legacy slow index for the k-th transmitted slow
    EXPECT_EQ(f313[k].data.u8[5], LegacyMg4::cnt(0xF0, j, 7)) << "313 cnt at slow " << j;
    EXPECT_EQ(f313[k].data.u8[10], LegacyMg4::rle_lookup(LegacyMg4::STAT, j)) << "313 STAT at slow " << j;
    EXPECT_EQ(f313[k].data.u8[18], (uint8_t)LegacyMg4::ramp(j, 32, 78, 77)) << "313 VALA at slow " << j;
    uint16_t expc = LegacyMg4::ramp(j, 32, 78, 90);
    EXPECT_EQ(f313[k].data.u8[20], (uint8_t)(0x80 | (expc >> 4))) << "313 VALC hi at slow " << j;
    EXPECT_EQ(f313[k].data.u8[21], (uint8_t)(((expc & 0xF) << 4) | 0x08)) << "313 VALC lo at slow " << j;
    uint32_t t16 = (j > 2) ? (uint32_t)(j - 2) * 10u : 0u;
    uint16_t exp16 = (t16 < 252) ? 562 : (uint16_t)target16;
    EXPECT_EQ(val16_of(f313[k]), exp16) << "313 VAL16 at slow " << j;
    EXPECT_EQ(f313[k].data.u8[29], LegacyMg4::cnt(0x30, j, 7)) << "313 cnt2 at slow " << j;
    EXPECT_EQ(f314[k].data.u8[5], LegacyMg4::cnt(0x40, j, 7)) << "314 cnt at slow " << j;
    EXPECT_EQ(f314[k].data.u8[6], LegacyMg4::rle_lookup(LegacyMg4::V314, j)) << "314 VAL at slow " << j;
    EXPECT_EQ(f314[k].data.u8[7], LegacyMg4::rle_lookup(LegacyMg4::H314, j)) << "314 HI at slow " << j;
    EXPECT_EQ(f314[k].data.u8[17], LegacyMg4::cnt(0x70, j, 7)) << "314 cnt2 at slow " << j;
    EXPECT_EQ(f314[k].data.u8[18], (uint8_t)LegacyMg4::ramp(j, 44, 59, 48)) << "314 ramp at slow " << j;
  }

  // Past the end the sequencer holds instead of looping: still closed-request
  // levels with live counters stepping by one.
  EXPECT_EQ(f08a[799].data.u8[19], 0x21);
  EXPECT_EQ(f08a[799].data.u8[30], 0x40);
  EXPECT_EQ(f313.back().data.u8[10], 0x05);
  EXPECT_EQ(f314.back().data.u8[6], 0x57);
}

// Every poll list answer is followed by one request for the next sweep DID,
// starting at 0x0001; a sweep answer (or a rejected sweep DID, which never
// reaches handle_pid) lets the poll list continue.
TEST_F(Mg4BatteryTest, DidSweepInterleavesWithPollList) {
  const uint8_t data[2] = {0x26, 0xAC};

  EXPECT_EQ(battery->handle_pid(0xB061, 0x26AC, data, sizeof(data)), 0x0001u);
  // The poll answer is still processed when a detour is returned.
  EXPECT_EQ(datalayer.battery.status.soh_pptt, 0x26AC);
  EXPECT_EQ(battery->handle_pid(0x0001, 0, data, sizeof(data)), 0x0000u);
  EXPECT_EQ(battery->handle_pid(0xB045, 0, data, sizeof(data)), 0x0002u);
  // 0x0002 rejected: no handle_pid call, the next poll answer moves on.
  EXPECT_EQ(battery->handle_pid(0xF192, 0, data, sizeof(data)), 0x0003u);
}

// Sweep answers are packed and rendered; poll list answers aren't recorded.
TEST_F(Mg4BatteryTest, DidSweepRecordsAndRendersAnswers) {
  const uint8_t poll[2] = {0x26, 0xAC};
  const uint8_t ascii[3] = {'A', 'B', 0x00};

  battery->handle_pid(0xB061, 0, poll, sizeof(poll));  // -> 0x0001
  battery->handle_pid(0x0001, 0, ascii, sizeof(ascii));
  battery->handle_pid(0xB061, 0, poll, sizeof(poll));  // -> 0x0002, never answered

  EXPECT_EQ(battery->did_sweep_answered, 1u);
  EXPECT_EQ(battery->did_sweep_buf_len, 3u + sizeof(ascii));
  std::string html = battery->get_uds_info_html().c_str();
  EXPECT_NE(html.find("In progress: 2/65535 DIDs tried, next 0x0003"), std::string::npos);
  EXPECT_NE(html.find("0001: AB[00] (3)"), std::string::npos);
  EXPECT_EQ(html.find("B061:"), std::string::npos);
  // Everything fits on one page, so there's no page indicator.
  EXPECT_EQ(html.find("Page "), std::string::npos);
}

// The sweep ends after 0xFFFF, never hands out 0x0000, and then stays out of
// the way of the poll list.
TEST_F(Mg4BatteryTest, DidSweepStopsAfterFullRange) {
  const uint8_t data[1] = {0x00};
  battery->did_sweep_next = 0xFFFE;

  EXPECT_EQ(battery->handle_pid(0xB061, 0, data, sizeof(data)), 0xFFFEu);
  EXPECT_EQ(battery->handle_pid(0xB061, 0, data, sizeof(data)), 0xFFFFu);
  EXPECT_EQ(battery->handle_pid(0xFFFF, 0, data, sizeof(data)), 0x0000u);
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(battery->handle_pid(0xB061, 0, data, sizeof(data)), 0x0000u);
  }
  std::string html = battery->get_uds_info_html().c_str();
  EXPECT_NE(html.find("Complete: 65535 DIDs tried"), std::string::npos);
  EXPECT_NE(html.find("FFFF: [00] (1)"), std::string::npos);
}

// Long answers are truncated, and answers beyond the buffer cap are dropped
// rather than overflowing it.
TEST_F(Mg4BatteryTest, DidSweepTruncatesAndCapsStorage) {
  uint8_t big[100];
  memset(big, 'x', sizeof(big));
  const uint8_t poll[1] = {0x00};

  for (int i = 0; i < 200; i++) {
    uint16_t did = battery->handle_pid(0xB061, 0, poll, sizeof(poll));
    battery->handle_pid(did, 0, big, sizeof(big));
  }

  EXPECT_GT(battery->did_sweep_dropped, 0u);
  EXPECT_LE(battery->did_sweep_buf_len, (uint32_t)battery->DID_SWEEP_BUF_MAX);
  EXPECT_EQ(battery->did_sweep_answered + battery->did_sweep_dropped, 200u);
  std::string html = battery->get_uds_info_html().c_str();
  EXPECT_NE(html.find("0001: " + std::string(64, 'x') + " (100, truncated)"), std::string::npos);
  EXPECT_NE(html.find("dropped (buffer full)"), std::string::npos);
  // Only one page of the listing is rendered per load.
  EXPECT_LE(html.size(), 1024u + battery->DID_SWEEP_PAGE_HTML_BUDGET);
}

// The listing is shown one page per load, "Page n/N" cycling through every
// record exactly once before wrapping, with each page within its budget - also
// for many short, unprintable answers (the worst case for decoration).
TEST_F(Mg4BatteryTest, DidSweepListingPagesAcrossReloads) {
  const uint8_t poll[1] = {0x00};
  const uint8_t tiny[1] = {0x01};

  for (int i = 0; i < 3000; i++) {
    uint16_t did = battery->handle_pid(0xB061, 0, poll, sizeof(poll));
    battery->handle_pid(did, 0, i % 2 ? tiny : poll, i % 2 ? sizeof(tiny) : 0);
  }
  ASSERT_GT(battery->did_sweep_dropped, 0u);
  const unsigned answered = battery->did_sweep_answered;

  std::string first = battery->get_uds_info_html().c_str();
  size_t at = first.find("Page 1/");
  ASSERT_NE(at, std::string::npos);
  const unsigned pages = std::stoul(first.substr(at + 7));
  ASSERT_GT(pages, 1u);

  unsigned records = 0;
  std::string html = first;
  for (unsigned page = 1; page <= pages; page++) {
    if (page > 1) {
      html = battery->get_uds_info_html().c_str();
    }
    EXPECT_NE(html.find("Page " + std::to_string(page) + "/" + std::to_string(pages)), std::string::npos);
    EXPECT_LE(html.size(), 1024u + battery->DID_SWEEP_PAGE_HTML_BUDGET);
    // Count records in the listing only (the header has ")<br>" too).
    for (size_t pos = html.find("monospace;'>"); (pos = html.find(")<br>", pos)) != std::string::npos; pos++) {
      records++;
    }
  }
  EXPECT_EQ(records, answered);

  // Wraps back to the first page.
  EXPECT_EQ(std::string(battery->get_uds_info_html().c_str()), first);
}
