#include "esp_timer.h"

uint64_t millis64() {
    /* Return ESP32's high-resolution timer in milliseconds, as a 64 bit value. */
    return esp_timer_get_time() / 1000ULL;
}
