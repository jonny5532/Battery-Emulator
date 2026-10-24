#include <gtest/gtest.h>

#include <cstring>
#include <initializer_list>
#include <vector>

#include "../../Software/src/battery/UdsCanBattery.h"
#include "../../Software/src/datalayer/datalayer.h"

// TX frame capture injected by the emulated CAN layer (see test/emul/can.cpp).
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

namespace {

class TestSecurityUdsBattery : public UdsCanBattery {
 public:
  using UdsCanBattery::handle_incoming_uds_can_frame;
  using UdsCanBattery::pause_uds;
  using UdsCanBattery::send_sequence_message;
  using UdsCanBattery::set_pid_scan_list;
  using UdsCanBattery::setup_uds;
  using UdsCanBattery::start_sequence;
  using UdsCanBattery::transmit_uds_can;

  explicit TestSecurityUdsBattery(UdsConfig cfg = {.fd = true, .security_access_level = 1}) : config(cfg) {}

  void setup() override { setup_uds(0x79B, 0x7BB, config); }
  void update_values() override {}
  void handle_incoming_can_frame(CAN_frame rx_frame) override { handle_incoming_uds_can_frame(rx_frame); }
  void transmit_can(unsigned long currentMillis) override { transmit_uds_can(currentMillis); }

  // Key = each seed byte XOR 0xA5, the same length as the seed (or key_len).
  uint8_t security_access_key(uint8_t level, const uint8_t* seed, uint16_t seed_len, uint8_t* key) const override {
    key_calls.push_back({level, std::vector<uint8_t>(seed, seed + seed_len)});
    const uint8_t n = key_len >= 0 ? key_len : seed_len;
    for (uint8_t i = 0; i < n; i++) {
      key[i] = (i < seed_len ? seed[i] : 0) ^ 0xA5;
    }
    return n;
  }

  void on_uds_sequence_step(uint16_t state, uint8_t sid, const uint8_t* data, uint16_t len) override {
    seq_calls.push_back({state, sid, std::vector<uint8_t>(data, data + len)});
  }
  void on_uds_sequence_timeout(uint16_t state) override { seq_timeouts.push_back(state); }

  struct SeqCall {
    uint16_t state;
    uint8_t sid;
    std::vector<uint8_t> data;
  };

  struct KeyCall {
    uint8_t level;
    std::vector<uint8_t> seed;
  };

  std::vector<SeqCall> seq_calls;
  std::vector<uint16_t> seq_timeouts;
  mutable std::vector<KeyCall> key_calls;
  int key_len = -1;  // -1 = same as the seed

 private:
  UdsConfig config;
};

CAN_frame uds_single_frame(uint32_t id, std::initializer_list<uint8_t> uds_message) {
  CAN_frame frame = {};
  frame.ID = id;
  frame.DLC = 8;
  uint8_t i = 0;
  frame.data.u8[i++] = static_cast<uint8_t>(uds_message.size());  // SF PCI byte
  for (uint8_t b : uds_message) {
    frame.data.u8[i++] = b;
  }
  while (i < frame.DLC) {
    frame.data.u8[i++] = 0x55;
  }
  return frame;
}

const CAN_frame& last_frame() {
  return get_transmitted_frames().back();
}

class UdsSecurityAccessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    memset(&datalayer, 0, sizeof(datalayer));
    clear_transmitted_frames();
    battery = new TestSecurityUdsBattery();
    battery->setup();
  }

  void TearDown() override { delete battery; }

  void use_config(UdsConfig cfg) {
    delete battery;
    battery = new TestSecurityUdsBattery(cfg);
    battery->setup();
  }

  void tick(unsigned long ms) { battery->transmit_uds_can(ms); }

  void feed_response(std::initializer_list<uint8_t> uds_message, uint32_t id = 0x7BB) {
    battery->handle_incoming_can_frame(uds_single_frame(id, uds_message));
  }

  // Start an ordinary protected routine step; the BMS answers it with NRC 0x33.
  void start_protected_step() {
    const uint8_t payload[] = {0xAA, 0xBB};
    ASSERT_TRUE(battery->send_sequence_message(0x1001, SID::WriteDataByIdentifier, payload, sizeof(payload), 5, 0));
    ASSERT_FALSE(get_transmitted_frames().empty());
    EXPECT_EQ(last_frame().data.u8[1], 0x2E);
    feed_response({0x7F, 0x2E, 0x33});  // SecurityAccessDenied
  }

  TestSecurityUdsBattery* battery = nullptr;
};

TEST_F(UdsSecurityAccessTest, UnlocksThenResumesTheDeferredStep) {
  start_protected_step();
  EXPECT_TRUE(battery->seq_calls.empty());
  EXPECT_TRUE(battery->uds_is_busy());

  // The security sequence starts straight away, without waiting for a tick.
  EXPECT_EQ(last_frame().data.u8[0], 0x02);
  EXPECT_EQ(last_frame().data.u8[1], 0x10);  // DiagnosticSessionControl
  EXPECT_EQ(last_frame().data.u8[2], 0x03);  // extended session
  EXPECT_TRUE(last_frame().FD);              // Config::fd applied

  feed_response({0x50, 0x03});
  EXPECT_EQ(last_frame().data.u8[1], 0x27);  // SecurityAccess
  EXPECT_EQ(last_frame().data.u8[2], 0x01);  // requestSeed

  feed_response({0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
  EXPECT_EQ(last_frame().data.u8[0], 0x06);  // SF length
  EXPECT_EQ(last_frame().data.u8[1], 0x27);
  EXPECT_EQ(last_frame().data.u8[2], 0x02);  // sendKey
  EXPECT_EQ(last_frame().data.u8[3], 0xB4);  // 0x11223344 ^ 0xA5A5A5A5
  EXPECT_EQ(last_frame().data.u8[4], 0x87);
  EXPECT_EQ(last_frame().data.u8[5], 0x96);
  EXPECT_EQ(last_frame().data.u8[6], 0xE1);

  // Key accepted: the original protected request is retried unchanged.
  feed_response({0x67, 0x02});
  EXPECT_EQ(last_frame().data.u8[0], 0x03);
  EXPECT_EQ(last_frame().data.u8[1], 0x2E);
  EXPECT_EQ(last_frame().data.u8[2], 0xAA);
  EXPECT_EQ(last_frame().data.u8[3], 0xBB);

  // Now the routine receives the response it originally expected.
  feed_response({0x6E, 0x01});
  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].state, 0x1001);
  EXPECT_EQ(battery->seq_calls[0].sid, 0x6E);
  EXPECT_FALSE(battery->uds_is_busy());
}

TEST_F(UdsSecurityAccessTest, FailedUnlockReportsTheOriginalNrc) {
  start_protected_step();

  feed_response({0x7F, 0x10, 0x22});  // session control rejected

  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].state, 0x1001);
  EXPECT_EQ(battery->seq_calls[0].sid, kNegativeResponseSid);
  EXPECT_EQ(battery->seq_calls[0].data,
            (std::vector<uint8_t>{kNegativeResponseSid, 0x2E, NegativeResponseCode::SecurityAccessDenied}));
  EXPECT_TRUE(battery->seq_timeouts.empty());
}

TEST_F(UdsSecurityAccessTest, TimedOutUnlockReportsTheOriginalNrc) {
  start_protected_step();

  // The security sequence has already sent 10 03. Session-control timeout is
  // 10 ticks with 2 retries, so it gives up after three 10-tick windows.
  for (unsigned long ms = 1100; ms <= 4000; ms += 100) {
    tick(ms);
  }

  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].state, 0x1001);
  EXPECT_EQ(battery->seq_calls[0].sid, kNegativeResponseSid);
  EXPECT_EQ(battery->seq_calls[0].data,
            (std::vector<uint8_t>{kNegativeResponseSid, 0x2E, NegativeResponseCode::SecurityAccessDenied}));
  // The unlock's own timeout is reported as the denial, not as a step timeout.
  EXPECT_TRUE(battery->seq_timeouts.empty());
}

TEST_F(UdsSecurityAccessTest, ASecondNrcAfterRetryDoesNotUnlockAgain) {
  start_protected_step();
  feed_response({0x50, 0x03});
  feed_response({0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
  feed_response({0x67, 0x02});

  const size_t frames_before = get_transmitted_frames().size();
  feed_response({0x7F, 0x2E, 0x33});

  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].state, 0x1001);
  EXPECT_EQ(battery->seq_calls[0].sid, kNegativeResponseSid);
  EXPECT_EQ(get_transmitted_frames().size(), frames_before);
  EXPECT_FALSE(battery->uds_is_busy());
}

TEST_F(UdsSecurityAccessTest, UnlockRunsWhileAUserSequenceIsQueued) {
  // Arrange: a protected step is in flight and a user sequence is queued
  // behind it, occupying the start_sequence() slot.
  const uint8_t payload[] = {0xAA, 0xBB};
  ASSERT_TRUE(battery->send_sequence_message(0x1001, SID::WriteDataByIdentifier, payload, sizeof(payload), 5, 0));
  ASSERT_TRUE(battery->start_sequence(0x2001));

  // Act: the step is denied, which must still start the unlock.
  feed_response({0x7F, 0x2E, 0x33});
  EXPECT_EQ(last_frame().data.u8[1], 0x10);  // DiagnosticSessionControl
  feed_response({0x50, 0x03});
  feed_response({0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
  feed_response({0x67, 0x02});
  EXPECT_EQ(last_frame().data.u8[1], 0x2E);  // the deferred step, retried
  feed_response({0x6E, 0x01});

  // Assert: the deferred step completed, and the queued user sequence is
  // still intact and starts on the next tick.
  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].state, 0x1001);
  EXPECT_EQ(battery->seq_calls[0].sid, 0x6E);

  tick(1000);
  ASSERT_EQ(battery->seq_calls.size(), 2u);
  EXPECT_EQ(battery->seq_calls[1].state, 0x2001);
  EXPECT_EQ(battery->seq_calls[1].sid, 0);
  EXPECT_FALSE(battery->uds_is_busy());
}

TEST_F(UdsSecurityAccessTest, UnlockBlockedByAPauseReportsTheOriginalNrc) {
  const uint8_t payload[] = {0xAA, 0xBB};
  ASSERT_TRUE(battery->send_sequence_message(0x1001, SID::WriteDataByIdentifier, payload, sizeof(payload), 5, 0));
  // An external tool appears while the step is in flight.
  battery->pause_uds(50, TestSecurityUdsBattery::UdsPriority::Custom);

  const size_t frames_before = get_transmitted_frames().size();
  feed_response({0x7F, 0x2E, 0x33});

  // No unlock traffic while paused; the routine sees the denial immediately.
  EXPECT_EQ(get_transmitted_frames().size(), frames_before);
  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].state, 0x1001);
  EXPECT_EQ(battery->seq_calls[0].sid, kNegativeResponseSid);

  // The external-tool pause is left as it was: it isn't stretched to the
  // 10 s unlock backoff, so a sequence can start once it expires.
  ASSERT_TRUE(battery->start_sequence(0x2001));
  for (int k = 0; k < 52; k++) {
    tick(1000 + 100 * k);
  }
  ASSERT_EQ(battery->seq_calls.size(), 2u);
  EXPECT_EQ(battery->seq_calls[1].state, 0x2001);
}

TEST_F(UdsSecurityAccessTest, TwoByteSeedGetsATwoByteKey) {
  // BYD Atto 3 style: 67 01 <2-byte seed> -> 27 02 <2-byte key>.
  start_protected_step();
  feed_response({0x50, 0x03});
  feed_response({0x67, 0x01, 0x12, 0x34});

  ASSERT_EQ(battery->key_calls.size(), 1u);
  EXPECT_EQ(battery->key_calls[0].level, 1);
  EXPECT_EQ(battery->key_calls[0].seed, (std::vector<uint8_t>{0x12, 0x34}));
  EXPECT_EQ(last_frame().data.u8[0], 0x04);  // SF length
  EXPECT_EQ(last_frame().data.u8[1], 0x27);
  EXPECT_EQ(last_frame().data.u8[2], 0x02);
  EXPECT_EQ(last_frame().data.u8[3], 0xB7);  // 0x12 ^ 0xA5
  EXPECT_EQ(last_frame().data.u8[4], 0x91);  // 0x34 ^ 0xA5

  feed_response({0x67, 0x02});
  EXPECT_EQ(last_frame().data.u8[1], 0x2E);  // the deferred step, retried
}

TEST_F(UdsSecurityAccessTest, KeyLengthIsIndependentOfSeedLength) {
  battery->key_len = 3;
  start_protected_step();
  feed_response({0x50, 0x03});
  feed_response({0x67, 0x01, 0x12, 0x34});

  EXPECT_EQ(last_frame().data.u8[0], 0x05);  // SF length: 27 02 + 3 key bytes
  EXPECT_EQ(last_frame().data.u8[1], 0x27);
  EXPECT_EQ(last_frame().data.u8[2], 0x02);
  EXPECT_EQ(last_frame().data.u8[3], 0xB7);
  EXPECT_EQ(last_frame().data.u8[4], 0x91);
  EXPECT_EQ(last_frame().data.u8[5], 0xA5);  // key byte beyond the seed
}

TEST_F(UdsSecurityAccessTest, LevelIsPassedToTheKeyCallback) {
  // MEB style: level 3 -> 27 03 / 27 04.
  use_config({.security_access_level = 3});
  start_protected_step();
  feed_response({0x50, 0x03});
  EXPECT_EQ(last_frame().data.u8[2], 0x03);  // requestSeed level 3
  feed_response({0x67, 0x03, 0x11, 0x22, 0x33, 0x44});

  ASSERT_EQ(battery->key_calls.size(), 1u);
  EXPECT_EQ(battery->key_calls[0].level, 3);
  EXPECT_EQ(last_frame().data.u8[1], 0x27);
  EXPECT_EQ(last_frame().data.u8[2], 0x04);  // sendKey for level 3
}

TEST_F(UdsSecurityAccessTest, AllZeroSeedSkipsTheKey) {
  start_protected_step();
  feed_response({0x50, 0x03});
  feed_response({0x67, 0x01, 0x00, 0x00});  // already unlocked

  EXPECT_TRUE(battery->key_calls.empty());
  EXPECT_EQ(last_frame().data.u8[1], 0x2E);  // straight to the deferred step
}

TEST_F(UdsSecurityAccessTest, EmptySeedFailsTheUnlock) {
  start_protected_step();
  feed_response({0x50, 0x03});
  feed_response({0x67, 0x01});

  EXPECT_TRUE(battery->key_calls.empty());
  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].sid, kNegativeResponseSid);
}

TEST_F(UdsSecurityAccessTest, KeyCallbackCanAbortTheUnlock) {
  battery->key_len = 0;
  start_protected_step();
  feed_response({0x50, 0x03});
  const size_t frames_before = get_transmitted_frames().size();
  feed_response({0x67, 0x01, 0x11, 0x22});

  EXPECT_EQ(get_transmitted_frames().size(), frames_before);  // no key sent
  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].state, 0x1001);
  EXPECT_EQ(battery->seq_calls[0].sid, kNegativeResponseSid);
}

TEST_F(UdsSecurityAccessTest, OversizedKeyFailsTheUnlock) {
  battery->key_len = 16;  // one more than fits
  start_protected_step();
  feed_response({0x50, 0x03});
  const size_t frames_before = get_transmitted_frames().size();
  feed_response({0x67, 0x01, 0x11, 0x22});

  EXPECT_EQ(get_transmitted_frames().size(), frames_before);
  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].sid, kNegativeResponseSid);
}

TEST_F(UdsSecurityAccessTest, SessionZeroRequestsTheSeedDirectly) {
  use_config({.security_access_level = 1, .security_access_session = 0});
  start_protected_step();

  // No 10 xx: the seed request is the first unlock frame.
  EXPECT_EQ(last_frame().data.u8[1], 0x27);
  EXPECT_EQ(last_frame().data.u8[2], 0x01);
  feed_response({0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
  feed_response({0x67, 0x02});
  EXPECT_EQ(last_frame().data.u8[1], 0x2E);
  feed_response({0x6E, 0x01});

  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].sid, 0x6E);
  for (const auto& f : get_transmitted_frames()) {
    EXPECT_NE(f.data.u8[1], 0x10);
  }
}

TEST_F(UdsSecurityAccessTest, FailedUnlockHoldsOffSequencesButNotThePidScan) {
  const uint16_t pids[] = {0x9001};
  battery->set_pid_scan_list(pids, 1);
  start_protected_step();
  feed_response({0x50, 0x03});
  feed_response({0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
  feed_response({0x7F, 0x27, 0x35});  // invalidKey

  ASSERT_EQ(battery->seq_calls.size(), 1u);
  EXPECT_EQ(battery->seq_calls[0].sid, kNegativeResponseSid);
  EXPECT_TRUE(battery->uds_is_busy());

  // A routine retrying straight away is refused...
  const uint8_t payload[] = {0xAA, 0xBB};
  EXPECT_FALSE(battery->send_sequence_message(0x1001, SID::WriteDataByIdentifier, payload, sizeof(payload), 5, 0));
  // ...and a queued sequence waits out the backoff, while PIDs keep flowing.
  ASSERT_TRUE(battery->start_sequence(0x2001));
  clear_transmitted_frames();
  unsigned long t = 1000;
  for (int k = 0; k < 99; k++, t += 100) {
    tick(t);
  }
  EXPECT_EQ(battery->seq_calls.size(), 1u);
  ASSERT_FALSE(get_transmitted_frames().empty());
  EXPECT_EQ(last_frame().data.u8[1], 0x22);  // PID scan request

  for (int k = 0; k < 5; k++, t += 100) {
    tick(t);
  }
  ASSERT_EQ(battery->seq_calls.size(), 2u);
  EXPECT_EQ(battery->seq_calls[1].state, 0x2001);
}

}  // namespace
