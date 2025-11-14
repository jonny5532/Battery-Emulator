#ifndef _TYPES_H_
#define _TYPES_H_

#include <chrono>
#include <string>

using milliseconds = std::chrono::milliseconds;
using duration = std::chrono::duration<unsigned long, std::ratio<1, 1000>>;

enum bms_status_enum { STANDBY = 0, INACTIVE = 1, DARKSTART = 2, ACTIVE = 3, FAULT = 4, UPDATING = 5 };
enum real_bms_status_enum { BMS_DISCONNECTED = 0, BMS_STANDBY = 1, BMS_ACTIVE = 2, BMS_FAULT = 3 };
enum battery_chemistry_enum { Autodetect = 0, NCA = 1, NMC = 2, LFP = 3, Highest };

enum class comm_interface {
  Modbus = 1,
  RS485 = 2,
  CanNative = 3,
  CanFdNative = 4,
  CanAddonMcp2515 = 5,
  CanFdAddonMcp2518 = 6,
#if defined(HW_3LB)
  CanFdAddonMcp2517_1 = 7,  // MCP2517FD U5 (CAN2)
  CanFdAddonMcp2517_2 = 8,  // MCP2517FD U3 (CAN3)
  CanFdNativeAsCan1 = 9,    // Select this to treat MCP2517 U5 as native CAN1 (stored as selection)
#endif
  Highest
};

enum led_mode_enum { CLASSIC, FLOW, HEARTBEAT };
enum PrechargeState {
  AUTO_PRECHARGE_IDLE,
  AUTO_PRECHARGE_START,
  AUTO_PRECHARGE_PRECHARGING,
  AUTO_PRECHARGE_OFF,
  AUTO_PRECHARGE_COMPLETED,
  AUTO_PRECHARGE_FAILURE
};

#define DISCHARGING 1
#define CHARGING 2

#define INTERVAL_10_MS 10
#define INTERVAL_20_MS 20
#define INTERVAL_30_MS 30
#define INTERVAL_40_MS 40
#define INTERVAL_50_MS 50
#define INTERVAL_70_MS 70
#define INTERVAL_100_MS 100
#define INTERVAL_200_MS 200
#define INTERVAL_250_MS 250
#define INTERVAL_500_MS 500
#define INTERVAL_640_MS 640
#define INTERVAL_1_S 1000
#define INTERVAL_2_S 2000
#define INTERVAL_5_S 5000
#define INTERVAL_10_S 10000
#define INTERVAL_30_S 30000
#define INTERVAL_60_S 60000

#define INTERVAL_10_MS_DELAYED 15

#define CAN_STILL_ALIVE 60
// Set by battery each time we get a CAN message. Decrements every second. When reaching 0, sets event

enum CAN_Interface {
  // Native CAN port on the LilyGo & Stark hardware
  CAN_NATIVE = 0,

  // Native CANFD port on the Stark CMR hardware
  CANFD_NATIVE = 1,

  // Add-on CAN MCP2515 connected to GPIO pins
  CAN_ADDON_MCP2515 = 2,

  // Add-on CAN-FD MCP2518 connected to GPIO pins
  CANFD_ADDON_MCP2518 = 3,

  // No CAN interface
  NO_CAN_INTERFACE = 4,
  // MCP2517FD (3LB specific)
  CANFD_ADDON_MCP2517_1 = 5,
  CANFD_ADDON_MCP2517_2 = 6
};

// Convenience alias: point virtual CANFD_NATIVE_CAN1 to MCP2517 U5 on this board
#ifndef CANFD_NATIVE_CAN1
#define CANFD_NATIVE_CAN1 CANFD_ADDON_MCP2517_1
#endif

extern const char* getCANInterfaceName(CAN_Interface interface);

/* CAN Frame structure */
typedef struct {
  bool FD;
  bool ext_ID;
  uint8_t DLC;
  uint32_t ID;
  union {
    uint8_t u8[64];
    uint32_t u32[2];
    uint64_t u64;
  } data;
} CAN_frame;

enum frameDirection { MSG_RX, MSG_TX };  //RX = 0, TX = 1

typedef struct {
  CAN_frame frame;
  frameDirection direction;
} CAN_log_frame;

std::string getBMSStatus(bms_status_enum status);

#endif
