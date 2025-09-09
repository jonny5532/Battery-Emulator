#ifndef __LOGGING_H__
#define __LOGGING_H__

#include <inttypes.h>
#include <mutex>
#include "../../datalayer/datalayer.h"
#include "Print.h"
#include "types.h"

class Logging : public Print {
  void add_timestamp(size_t size);
  std::mutex writeMutex;

 public:
  virtual size_t write(const uint8_t* buffer, size_t size);
  virtual size_t write(uint8_t) { return 0; }
  void printf(const char* fmt, ...);
  Logging() {}
};

// Production macros
#define DEBUG_PRINTF(fmt, ...)                                                                  \
  do {                                                                                          \
    if (datalayer.system.info.web_logging_active || datalayer.system.info.usb_logging_active) { \
      logging.printf(fmt, ##__VA_ARGS__);                                                       \
    }                                                                                           \
  } while (0)

#define DEBUG_PRINTLN(str)                                                                      \
  do {                                                                                          \
    if (datalayer.system.info.web_logging_active || datalayer.system.info.usb_logging_active) { \
      logging.println(str);                                                                     \
    }                                                                                           \
  } while (0)

extern Logging logging;

#endif  // __LOGGING_H__
