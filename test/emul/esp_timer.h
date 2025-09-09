#ifndef EMUL_ESP_TIMER_H
#define EMUL_ESP_TIMER_H

#include <chrono>

inline int64_t esp_timer_get_time(void) {
  static const auto start_time = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count();
  return static_cast<int64_t>(elapsed);
}

#endif  // EMUL_ESP_TIMER_H
