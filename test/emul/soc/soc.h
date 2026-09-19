#pragma once

// Start of RTC data (slow) memory on the ESP32. The MG-4 battery driver
// allocates its OTA-persistent discharge counter inside this region and the
// host tests mmap() a page there, so the value must match the real ESP32.
#define SOC_RTC_DATA_LOW 0x3FF80000UL
#define SOC_RTC_DATA_HIGH 0x3FFC0000UL

// End of regular data RAM
#define SOC_EXTRAM_DATA_LOW 0x3FF80000UL
