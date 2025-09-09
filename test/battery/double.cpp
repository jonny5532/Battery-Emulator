#include <gtest/gtest.h>

#include "../utils/utils.h"

#include "../../Software/src/battery/BATTERIES.h"
#include "../../Software/src/devboard/utils/events.h"

class DoubleBatteryTestFixture : public testing::Test {
 public:
  DoubleBatteryTestFixture(BatteryType type) : type(type) {}
  // Optional:
  //   static void SetUpTestSuite() { ... }
  //   static void TearDownTestSuite() { ... }

  void SetUp() override {
    // Reset the datalayer and events before each test
    datalayer = DataLayer();
    reset_all_events();
    if (battery) {
      delete battery;
      battery = nullptr;
    }
    init_hal();

    user_selected_battery_type = type;
    user_selected_second_battery = true;

    setup_battery();
  }

  void TearDown() override {
    if (battery) {
      delete battery;
      battery = nullptr;
    }
    if (battery2) {
      delete battery2;
      battery2 = nullptr;
    }
  }

 private:
  BatteryType type;
};

// Check that the CAN aliveness timeout isn't being renewed by bogus CAN frames
class TestSetup : public DoubleBatteryTestFixture {
 public:
  explicit TestSetup(BatteryType type) : DoubleBatteryTestFixture(type) {}
  void TestBody() override {
    EXPECT_NE(battery, nullptr);
    EXPECT_NE(battery2, nullptr);
  }
};

bool IsValidDoubleBattery(BatteryType type) {
  if (type == BatteryType::TestFake) {
    return false;
  }

  // We need some minimal setup or the battery constructors may segfault
  datalayer = DataLayer();
  // This leaks memory (but not much...)
  init_hal();

  auto* tmp_battery = create_battery(type);
  if (tmp_battery == nullptr) {
    // Not a valid battery type
    return false;
  }

  //   auto* as_can_battery = dynamic_cast<CanBattery*>(tmp_battery);
  //   if (as_can_battery == nullptr) {
  //     // Failed to cast to CanBattery, so it's not a CAN battery
  //     delete tmp_battery;
  //     return false;
  //   }
  delete tmp_battery;

  return true;
}

void RegisterDoubleBatteryTests() {
  for (int i = 0; i < (int)BatteryType::Highest; i++) {
    if (!IsValidBattery((BatteryType)i)) {
      continue;
    }

    std::string test_name = ("TestSetup" + snake_case_to_camel_case(name_for_battery_type((BatteryType)i)));
    testing::RegisterTest("DoubleBatteryTests", test_name.c_str(), nullptr, nullptr, __FILE__, __LINE__,
                          [=]() -> DoubleBatteryTestFixture* { return new TestSetup((BatteryType)i); });
  }
}
