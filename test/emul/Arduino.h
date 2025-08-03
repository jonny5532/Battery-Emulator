#ifndef ARDUINO_H
#define ARDUINO_H

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

#include "HardwareSerial.h"
#include "Print.h"
#include "Printable.h"
#include "esp-hal-gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_num.h"

#include "WString.h"

//typedef	uint64_t	time_t;

// Arduino base constants for print formatting
constexpr int BIN = 2;
constexpr int OCT = 8;
constexpr int DEC = 10;
constexpr int HEX = 16;
// Arduino type aliases
using byte = uint8_t;
#define boolean bool
// Arduino random functions
inline long random(long max) {
  (void)max;
  return 0;  // Return a predictable value for testing
}

inline long random(long min, long max) {
  (void)min;
  (void)max;
  return min;  // Return the minimum value for predictability
}

inline void randomSeed(unsigned long seed) {
  (void)seed;
}

inline uint16_t word(uint8_t highByte, uint8_t lowByte) {
  return (static_cast<uint16_t>(highByte) << 8) | lowByte;
}

inline uint16_t word(uint16_t w) {
  return w;
}

// Bit manipulation functions
inline uint8_t bitRead(uint8_t value, uint8_t bit) {
  return (value >> bit) & 0x01;
}

inline void bitSet(uint8_t& value, uint8_t bit) {
  value |= (1UL << bit);
}

inline void bitClear(uint8_t& value, uint8_t bit) {
  value &= ~(1UL << bit);
}

inline void bitWrite(uint8_t& value, uint8_t bit, uint8_t bitvalue) {
  if (bitvalue) {
    bitSet(value, bit);
  } else {
    bitClear(value, bit);
  }
}

// Byte extraction functions
inline uint8_t lowByte(uint16_t w) {
  return static_cast<uint8_t>(w & 0xFF);
}

inline uint8_t highByte(uint16_t w) {
  return static_cast<uint8_t>(w >> 8);
}

template <typename T>
inline const T& min(const T& a, const T& b) {
  return (a < b) ? a : b;
}

template <typename T>
inline const T& max(const T& a, const T& b) {
  return (a > b) ? a : b;
}
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
inline int analogRead(uint8_t pin) {
  (void)pin;
  return 0;  // Return 0 for predictable tests
}

// Mock WiFi types
typedef int WiFiEvent_t;
typedef int WiFiEventInfo_t;

// Mock WiFi functions
inline void onWifiConnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  (void)event;
  (void)info;
}

inline void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  (void)event;
  (void)info;
}

unsigned long micros();
// Can be previously declared as a macro in stupid eModbus
#undef millis
unsigned long millis();

void delay(unsigned long ms);
void delayMicroseconds(unsigned long us);

int max(int a, int b);
int min(int a, int b);

bool ledcAttachChannel(uint8_t pin, uint32_t freq, uint8_t resolution, int8_t channel);
bool ledcWrite(uint8_t pin, uint32_t duty);

class ESPClass {
 public:
  size_t getFlashChipSize() {
    // This is a placeholder for the actual implementation
    // that retrieves the flash chip size.
    return 4 * 1024 * 1024;  // Example: returning 4MB
  }
};

extern ESPClass ESP;

typedef int esp_err_t;

esp_err_t esp_task_wdt_add(TaskHandle_t task_handle);
esp_err_t esp_task_wdt_reset(void);

typedef struct {
  uint32_t timeout_ms; /**< TWDT timeout duration in milliseconds */
  uint32_t
      idle_core_mask; /**< Bitmask of the core whose idle task should be subscribed on initialization where 1 << i means that core i's idle task will be monitored by the TWDT */
  bool trigger_panic; /**< Trigger panic when timeout occurs */
} esp_task_wdt_config_t;

float temperatureRead();

uint32_t ledcWriteTone(uint8_t pin, uint32_t freq);

/*size_t strlen_P(const char *s);
int strncmp_P (const char *, const char *, size_t);
int	strcmp_P (const char *, const char *);
int	memcmp_P (const void *, const void *, size_t);
void *memcpy_P (void *, const void *, size_t);
*/

#define pgm_read_byte(addr) (*(const unsigned char*)(addr))

#define PROGMEM
#define PGM_P const char*
#define PSTR(s) (s)
#define __unused

#define snprintf_P snprintf
#define strlen_P strlen
#define memcpy_P memcpy
#define sprintf_P sprintf

char* strndup(const char* str, size_t size);

class EspClass {
 public:
  void restart();
};

extern EspClass ESP;

void log_printf(const char* format, ...);

#define log_d(format, ...) log_printf(format, ##__VA_ARGS__)
#define log_e(format, ...) log_printf(format, ##__VA_ARGS__)
#define log_i(format, ...) log_printf(format, ##__VA_ARGS__)
#define log_w(format, ...) log_printf(format, ##__VA_ARGS__)
#define log_v(format, ...) log_printf(format, ##__VA_ARGS__)

struct tm* localtime_r(const time_t*, struct tm*);

int strcasecmp(const char*, const char*);

#endif
