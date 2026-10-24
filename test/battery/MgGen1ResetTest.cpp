#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "../../Software/src/battery/MG-GEN1-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"

// TX frame capture injected by the emulated CAN layer (see test/emul/can.cpp).
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();
void set_millis64(uint64_t);

namespace {

// MG Gen1 re-runs setup_uds() once it has found the BMS address, so the reset
// settings must survive that switch.
TEST(MgGen1ResetTest, ResetUsesSessionThenHardResetAfterAddressSwitch) {
  memset(&datalayer, 0, sizeof(datalayer));
  clear_transmitted_frames();
  MgGen1Battery battery;
  battery.setup();
  EXPECT_TRUE(battery.supports_reset_BMS());

  // A reply from 0x7ED switches UDS to the BMS on 0x7E5.
  CAN_frame reply = {};
  reply.ID = 0x7ED;
  reply.DLC = 8;
  battery.handle_incoming_can_frame(reply);

  battery.reset_BMS();
  bool sent_session = false;
  for (unsigned long t = 1000; t < 3000 && !sent_session; t += 10) {
    set_millis64(t);
    battery.transmit_can(t);
    for (const auto& f : get_transmitted_frames()) {
      sent_session |= f.ID == 0x7E5 && f.data.u8[1] == 0x10 && f.data.u8[2] == 0x03;
    }
  }
  ASSERT_TRUE(sent_session);

  CAN_frame ok = {};
  ok.ID = 0x7ED;
  ok.DLC = 8;
  const uint8_t positive[] = {0x02, 0x50, 0x03, 0x55, 0x55, 0x55, 0x55, 0x55};
  memcpy(ok.data.u8, positive, sizeof(positive));
  battery.handle_incoming_can_frame(ok);

  const CAN_frame& last = get_transmitted_frames().back();
  EXPECT_EQ(last.ID, 0x7E5u);
  EXPECT_EQ(last.data.u8[1], 0x11);
  EXPECT_EQ(last.data.u8[2], 0x01);  // hard reset
}

// A BMS that has stopped answering PID reads is exactly when a reset is wanted.
// The reset must still go out while the PID scan is busy timing out.
TEST(MgGen1ResetTest, ResetIsSentWhileThePidScanIsTimingOut) {
  memset(&datalayer, 0, sizeof(datalayer));
  clear_transmitted_frames();
  MgGen1Battery battery;
  battery.setup();
  CAN_frame reply = {};
  reply.ID = 0x7ED;
  reply.DLC = 8;
  battery.handle_incoming_can_frame(reply);

  // Several seconds of unanswered PID reads.
  unsigned long t = 1000;
  for (; t < 6000; t += 10) {
    set_millis64(t);
    battery.transmit_can(t);
  }

  for (int attempt = 0; attempt < 10; attempt++) {
    clear_transmitted_frames();
    battery.reset_BMS();
    bool sent_session = false;
    for (unsigned long end = t + 500; t < end; t += 10) {
      set_millis64(t);
      battery.transmit_can(t);
    }
    for (const auto& f : get_transmitted_frames()) {
      sent_session |= f.ID == 0x7E5 && f.data.u8[1] == 0x10 && f.data.u8[2] == 0x03;
    }
    ASSERT_TRUE(sent_session) << "attempt " << attempt;
    // Let the unanswered session request time out, then wait out the holdoff
    // (10 s) at a different phase of the PID retry cycle.
    for (unsigned long end = t + 11000 + attempt * 30; t < end; t += 10) {
      set_millis64(t);
      battery.transmit_can(t);
    }
  }
}

}  // namespace
