#include "hal.h"

#include <Arduino.h>

// --- wybór hardware ---
#if defined(HW_3LB)
#include "hw_3LB.h"
#elif defined(HW_LILYGO)
#include "hw_lilygo.h"
#elif defined(HW_LILYGO2CAN)
#include "hw_lilygo2can.h"
#elif defined(HW_STARK)
#include "hw_stark.h"
#elif defined(HW_DEVKIT)
#include "hw_devkit.h"
#else
#error "No HW defined."
#endif

Esp32Hal* esp32hal = nullptr;

// 3LB I2C initialization is in hal_3lb_i2c.cpp
#if defined(HW_3LB)
extern void init_3lb_i2c();
#endif

void init_hal() {
  Serial.begin(115200);
  delay(300);

#if defined(HW_3LB)
  Serial.println("🔧 Initializing 3LB hardware...");
  init_3lb_i2c();  // Call 3LB-specific I2C init from separate file
#endif

  // --- tworzenie instancji Twojej klasy HAL jak dotychczas ---
#if defined(HW_3LB)
  esp32hal = new ThreeLBHal();
#elif defined(HW_LILYGO)
  esp32hal = new LilyGoHal();
#elif defined(HW_LILYGO2CAN)
  esp32hal = new LilyGo2CANHal();
#elif defined(HW_STARK)
  esp32hal = new StarkHal();
#elif defined(HW_DEVKIT)
  esp32hal = new DevKitHal();
#endif

  Serial.println("✅ HAL initialization complete.");
}

bool Esp32Hal::system_booted_up() {
  return milliseconds(millis()) > BOOTUP_TIME();
}
#ifdef HW_3LB
bool is_mcp_pin(gpio_num_t pin) {
  // Zakładamy MCP piny 0–15
  return (pin >= 0 && pin < 16);
}
#endif
