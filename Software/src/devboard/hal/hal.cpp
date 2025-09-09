#include "hal.h"

#include <Arduino.h>

#if defined(HW_LILYGO)
#include "hw_lilygo.h"
#elif defined(HW_LILYGO2CAN)
#include "hw_lilygo2can.h"
#elif defined(HW_STARK)
#include "hw_stark.h"
#elif defined(HW_3LB)
#include "hw_3LB.h"
#elif defined(HW_DEVKIT)
#include "hw_devkit.h"
#elif defined(HW_PC)
#include "hw_pc.h"
#endif

Esp32Hal* esp32hal = nullptr;

void init_hal() {
#if defined(HW_LILYGO)
  esp32hal = new LilyGoHal();
#elif defined(HW_LILYGO2CAN)
  esp32hal = new LilyGo2CANHal();
#elif defined(HW_STARK)
  esp32hal = new StarkHal();
#elif defined(HW_3LB)
  esp32hal = new ThreeLBHal();
#elif defined(HW_DEVKIT)
  esp32hal = new DevKitHal();
#elif defined(HW_PC)
  esp32hal = new PcHal();
#else
#error "No HW defined."
#endif
}

bool Esp32Hal::system_booted_up() {
  return milliseconds(millis()) > BOOTUP_TIME();
}
