#include <gtest/gtest.h>
#include <sys/mman.h>
#include "../../Software/src/battery/BATTERIES.h"
#include "../../Software/src/battery/MG-4-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "soc/soc.h"

class Mg4BatteryTest : public ::testing::Test {
 protected:
  Mg4Battery* battery;

  void SetUp() override {
    // Map the RTC slow memory region the MG-4 uses as NVRAM for its discharge
    // counter (see MG-4-BATTERY.cpp setup()). Without this the host build would
    // segfault on the first write to SOC_RTC_DATA_LOW + 400.
    mmap((void*)SOC_RTC_DATA_LOW, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    battery = new Mg4Battery();
    // Reset datalayer to a known state
    memset(&datalayer, 0, sizeof(datalayer));
    user_selected_battery_chemistry = battery_chemistry_enum::NMC;
    // This test exercises the coulomb-counting SoC path, so enable it before setup()
    user_selected_use_estimated_SOC = true;
    battery->setup();
    // Set some default info since setup sets some but not all
    datalayer.battery.info.total_capacity_Wh = 51000;  // 51kWh pack
    datalayer.battery.info.number_of_cells = 104;
  }

  void TearDown() override { delete battery; }

  void send_can_12c_fd(uint16_t voltage_dV, int16_t current_dA, uint16_t min_mV, uint16_t max_mV) {
    CAN_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.ID = 0x12C;
    frame.FD = true;
    frame.DLC = 64;  // MG4 FD frame is long

    // voltage_dV = (((rx_frame.data.u8[8] << 4) | (rx_frame.data.u8[9] >> 4)) * 5) / 2;
    // inverse: (voltage_dV * 2 / 5) = (u8[8] << 4) | (u8[9] >> 4)
    uint16_t v_raw = (voltage_dV * 2) / 5;
    frame.data.u8[8] = (v_raw >> 4) & 0xFF;
    frame.data.u8[9] = (v_raw & 0x0F) << 4;

    // current_dA = -(((rx_frame.data.u8[6] << 8) | rx_frame.data.u8[7]) - 20000) / 2;
    // inverse: -2 * current_dA + 20000 = (u8[6] << 8) | u8[7]
    uint16_t c_raw = (uint16_t)(-2 * current_dA + 20000);
    frame.data.u8[6] = (c_raw >> 8) & 0xFF;
    frame.data.u8[7] = c_raw & 0xFF;

    // cell_min_voltage_mV = ((rx_frame.data.u8[30] << 8) | (rx_frame.data.u8[31])) / 8;
    uint16_t min_raw = min_mV * 8;
    frame.data.u8[30] = (min_raw >> 8) & 0xFF;
    frame.data.u8[31] = min_raw & 0xFF;

    // cell_max_voltage_mV = ((rx_frame.data.u8[32] << 8) | (rx_frame.data.u8[33])) / 8;
    uint16_t max_raw = max_mV * 8;
    frame.data.u8[32] = (max_raw >> 8) & 0xFF;
    frame.data.u8[33] = max_raw & 0xFF;

    battery->handle_incoming_can_frame(frame);
  }
};

TEST_F(Mg4BatteryTest, CoulombCountAndLimitsTest) {
  // Initial state: 3.7V per cell (approx 50% SoC for NMC)
  uint16_t cell_mV = 3700;
  uint16_t pack_voltage_dV = (cell_mV * 104) / 100;

  // First frame to initialize
  send_can_12c_fd(pack_voltage_dV, 0, cell_mV, cell_mV);
  battery->update_values();

  uint16_t initial_soc = datalayer.battery.status.real_soc;
  // Check SoC is in a believable range
  EXPECT_GE(initial_soc, 4000);
  EXPECT_LE(initial_soc, 6000);

  // Test Discharge Limit Observation
  // MG-4-BATTERY.cpp setup(): min_cell_voltage_mV for NMC is 2700, so
  // working_min is 3000mV. The cell-voltage limiter tapers to zero over the
  // last 50mV and latches at zero below working_min.

  // Simulate hitting working min
  send_can_12c_fd(300 * 104 / 10, 0, 2999, 3100);
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0);
  EXPECT_EQ(datalayer.battery.status.real_soc, 0);

  // Test Charge Limit Observation
  // setup(): max_cell_voltage_mV for NMC is 4250, so the default working max is
  // 4240mV (no user-selected max is set, working_cell_max_mV = 4250 - 10).

  // Simulate hitting working max
  send_can_12c_fd(425 * 104 / 10, 0, 4250, 4250);
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0);
  EXPECT_EQ(datalayer.battery.status.real_soc, 10000);

  // Test Coulomb Counting

  // Simulate discharge: 100A (1000dA) for some "ticks"
  // update_values is called "every second" in real life.
  // total_discharge_dC -= current_dA; (since discharge is negative current)
  // Actually MG-4 current_dA is calculated as:
  // current_dA = -(((rx_frame.data.u8[6] << 8) | rx_frame.data.u8[7]) - 20000) / 2;
  // If we want 100A discharge, current_dA should be -1000.

  // Note: the previous step reset the discharge counter to zero while at
  // 100% SoC, so the drift-correction in update_values() bumps it to
  // one_percent_dC (cell voltage is below the recharge threshold) before
  // counting resumes. 4500 ticks of 100A therefore lands at ~4.5% rather
  // than the ~10% a naive count would suggest.

  for (int i = 0; i < 4500; i++) {  // 100A discharge
    send_can_12c_fd(pack_voltage_dV, -1000, 3700, 3700);
    battery->update_values();
  }

  uint16_t soc_after_discharge = datalayer.battery.status.real_soc;
  // Should be ~4.5% now
  EXPECT_LE(soc_after_discharge, 1500);
  EXPECT_GE(soc_after_discharge, 400);

  for (int i = 0; i < 500; i++) {  // 100A discharge
    send_can_12c_fd(pack_voltage_dV, -1000, 3700, 3700);
    battery->update_values();
  }

  soc_after_discharge = datalayer.battery.status.real_soc;
  // Should be floored at 1% (since min cell voltage is still above working min)
  EXPECT_EQ(soc_after_discharge, 100);
}

// handle_pid alternates between returning 0 (runtime poll list continues) and
// the next untried DID (one background sweep request), starting at 0xB000.
TEST_F(Mg4BatteryTest, HandlePidAlternatesDetourAndZero) {
  const uint8_t data[] = {0x00};

  // First response: detour for the first sweep DID.
  EXPECT_EQ(battery->handle_pid(0xB061, 0, data, sizeof(data)), 0xB000u);
  // Next response: 0, the runtime poll list continues.
  EXPECT_EQ(battery->handle_pid(0xB061, 0, data, sizeof(data)), 0x0000u);
  // And so on, one DID at a time.
  EXPECT_EQ(battery->handle_pid(0xB061, 0, data, sizeof(data)), 0xB001u);
  EXPECT_EQ(battery->handle_pid(0xB061, 0, data, sizeof(data)), 0x0000u);
  EXPECT_EQ(battery->handle_pid(0xB061, 0, data, sizeof(data)), 0xB002u);
}

// DIDs that answer are recorded and rendered in the alpha-or-hex format, and a
// re-answered DID updates its entry instead of duplicating it.
TEST_F(Mg4BatteryTest, SweepRecordsAndRendersResponses) {
  const uint8_t data1[] = {'A', 'B', 0x00};
  const uint8_t data2[] = {'O', 'K'};
  const uint8_t data3[] = {'X'};

  // Drive some sweep traffic through handle_pid (detour returns ignored) and
  // pretend 0xB042 and 0xB061 answered.
  battery->handle_pid(0xB042, 0x414200, data1, sizeof(data1));  // -> detour 0xB000
  battery->handle_pid(0xB000, 0, data1, sizeof(data1));         // -> 0
  battery->handle_pid(0xB061, 0x4F4B, data2, sizeof(data2));    // -> detour 0xB001

  String html = battery->get_uds_info_html();
  EXPECT_NE(std::string(html.c_str()).find("Background DID sweep: 2/512"), std::string::npos);
  EXPECT_NE(std::string(html.c_str()).find("DID B042: AB[00]"), std::string::npos);
  EXPECT_NE(std::string(html.c_str()).find("DID B061: OK"), std::string::npos);

  // Re-answer of an already-recorded DID updates in place.
  battery->handle_pid(0xB042, 0x58, data3, sizeof(data3));
  html = battery->get_uds_info_html();
  EXPECT_NE(std::string(html.c_str()).find("DID B042: X"), std::string::npos);
  EXPECT_EQ(std::string(html.c_str()).find("DID B042: AB[00]"), std::string::npos);
}

// Once 0xF1FF has been handed out every DID in the two swept ranges has been
// tried exactly once (the gap between the ranges is skipped); handle_pid then
// always returns 0 (no DID is re-added) and the page reports the sweep as
// complete.
TEST_F(Mg4BatteryTest, SweepStopsAfterBothRanges) {
  const uint8_t data[] = {0x00};

  uint16_t expected = 0xB000;
  uint32_t detour_count = 0;
  bool done = false;
  // Alternation means roughly two calls per DID; run until 0xF1FF has been
  // handed out, then keep going to prove the sweep has stopped.
  for (uint32_t i = 0; i < 2u * 512u + 100; i++) {
    uint16_t ret = battery->handle_pid(0xB061, 0, data, sizeof(data));
    if (done) {
      // Full range handed out: no detours may be returned anymore.
      EXPECT_EQ(ret, 0x0000u);
    } else if (ret != 0) {
      detour_count++;
      // Detours must be sequential within each range, hopping the gap.
      EXPECT_EQ(ret, expected);
      if (ret == 0xB0FF) {
        expected = 0xF100;  // Skip the gap between the ranges.
      } else if (ret == 0xF1FF) {
        done = true;  // Last DID of the second range: sweep complete.
      } else {
        expected++;
      }
    }
  }

  // Exactly one detour per DID in 0xB000-0xB0FF and 0xF100-0xF1FF.
  EXPECT_TRUE(done);
  EXPECT_EQ(detour_count, 512u);

  String html = battery->get_uds_info_html();
  EXPECT_NE(std::string(html.c_str()).find("Background DID sweep complete"), std::string::npos);
  EXPECT_EQ(std::string(html.c_str()).find("Background DID sweep: "), std::string::npos);
}
