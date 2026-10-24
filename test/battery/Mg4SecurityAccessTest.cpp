#include <gtest/gtest.h>

#include <cstring>

#include "../../Software/src/battery/MG-4-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"

namespace {

class SecurityAccessTestableMg4Battery : public Mg4Battery {
 public:
  using Mg4Battery::fd_uds_requests;
  using Mg4Battery::security_access_key;
  using Mg4Battery::security_access_level;
};

class Mg4SecurityAccessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    memset(&datalayer, 0, sizeof(datalayer));
    battery = new SecurityAccessTestableMg4Battery();
    battery->setup();
  }

  void TearDown() override { delete battery; }

  SecurityAccessTestableMg4Battery* battery = nullptr;
};

TEST_F(Mg4SecurityAccessTest, KeyAlgorithmMatchesFirmwareVectors) {
  // Both keys were accepted by the real firmware (see the tricore-emu notes).
  auto key_for = [&](uint32_t seed) {
    const uint8_t s[4] = {(uint8_t)(seed >> 24), (uint8_t)(seed >> 16), (uint8_t)(seed >> 8), (uint8_t)seed};
    uint8_t k[15] = {};
    EXPECT_EQ(battery->security_access_key(1, s, sizeof(s), k), 4);
    return ((uint32_t)k[0] << 24) | ((uint32_t)k[1] << 16) | ((uint32_t)k[2] << 8) | k[3];
  };
  EXPECT_EQ(key_for(0x1637D36C), 0x177A081Fu);
  EXPECT_EQ(key_for(0x1635CE68), 0xDA3EFA2Du);
}

TEST_F(Mg4SecurityAccessTest, SetupEnablesLevelOneAndFdRequests) {
  EXPECT_EQ(battery->security_access_level, 1);
  EXPECT_TRUE(battery->fd_uds_requests);
}

}  // namespace
