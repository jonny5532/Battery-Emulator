#include <gtest/gtest.h>
#include <stdio.h>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

void RegisterCanLogTests(void);
void RegisterStillAliveTests(void);
void RegisterDoubleBatteryTests(void);

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  RegisterCanLogTests();
  RegisterStillAliveTests();
  RegisterDoubleBatteryTests();
  return RUN_ALL_TESTS();
}

void store_settings_equipment_stop(void) {}
