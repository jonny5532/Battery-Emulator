#ifndef PRINT_H
#define PRINT_H

class Print {
 public:
  virtual void flush() {}
  size_t vprintf(const char* format, va_list arg);
  size_t printf(const char* format, ...) {}
  size_t println(const char[]);
  virtual size_t write(uint8_t) { return 0; }
  virtual size_t write(const char* s) { return 0; }
  virtual size_t write(const uint8_t* buffer, size_t size) { return 0; }
};

#endif
