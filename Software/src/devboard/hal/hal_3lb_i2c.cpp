// 3LB-specific I2C initialization and MCP23017 setup
// This file is ONLY compiled when HW_3LB is defined
// Wire includes are here to prevent PlatformIO LDF from detecting them in other builds

#if defined(HW_3LB)

#include <Adafruit_MCP23X17.h>
#include <Wire.h>
#include "hal.h"

Adafruit_MCP23X17 mcp;

// Zgodnie ze schematem
#define MCP23017_ADDR 0x20   // A0..A2 = GND
#define MCP23017_INT_PIN 39  // INTA -> GPIO39 (input-only, bez wewn. pull-up)
#define MCP23017_RST_PIN 17  // RESET -> GPIO17 (aktywny niski)

static void i2cScanRange(uint8_t a0 = 0x20, uint8_t a1 = 0x27) {
  Serial.println("🔍 I2C scanning...");
  for (uint8_t addr = a0; addr <= a1; addr++) {
    Wire.beginTransmission(addr);
    // Retry a couple of times to mitigate transient I2C hardware timeouts on some boards/wiring
    const int maxRetries = 3;
    int result = -1;
    for (int r = 0; r < maxRetries; ++r) {
      result = Wire.endTransmission();
      if (result == 0)
        break;
      delay(5);
    }
    if (result == 0) {
      Serial.printf("✅ Found I2C device at 0x%02X\n", addr);
    } else if (result != 0 && result != 4) {
      // result == 4 is "other error" from Wire; log non-found addresses only when suspicious
      // Keep output minimal: only log devices found to avoid noisy logs.
    }
    delay(2);
  }
  Serial.println("🔎 Scan done.");
}

void init_3lb_i2c() {
  // I2C start (wg schematu)
  Wire.begin(21, 22);
  Wire.setClock(100000);  // 100 kHz na początek – stabilniej
  delay(10);

  // Podnieś RESET MCP23017
  pinMode(MCP23017_RST_PIN, OUTPUT);
  digitalWrite(MCP23017_RST_PIN, HIGH);
  delay(5);

  // Skan adresów 0x20..0x27
  i2cScanRange(0x20, 0x27);

  // Inicjalizacja MCP23017
  if (!mcp.begin_I2C(MCP23017_ADDR)) {
    Serial.println("❌ MCP23017 not detected at 0x20 (0x40/0x41 8-bit)!");
  } else {
    Serial.println("✅ MCP23017 initialized at 0x20.");

    // Przykład – GPB0 (pin 8) jako wyjście
    mcp.pinMode(8, OUTPUT);
    mcp.digitalWrite(8, HIGH);
  }

  // Przerwania włącz dopiero, gdy na INTA jest zewnętrzny pull-up 3.3V
  // pinMode(MCP23017_INT_PIN, INPUT);
  // attachInterrupt(MCP23017_INT_PIN, [](){ /* ISR */ }, FALLING);
}

#endif  // HW_3LB
