#include "comm_contactorcontrol.h"
#include "../../battery/BATTERIES.h"
#include "../../communication/can/CanReceiver.h"
#include "../../devboard/hal/hal.h"
#include "../../devboard/mqtt/mqtt.h"
#include "../../devboard/safety/safety.h"
#include "../../inverter/INVERTERS.h"

#ifdef HW_3LB
#include <Adafruit_MCP23X17.h>
extern Adafruit_MCP23X17 mcp;            // ekspander I²C MCP23017
extern bool is_mcp_pin(gpio_num_t pin);  // deklaracja z hal.cpp
#else
// Provide lightweight stubs so non-3LB boards can compile this file without changes.
struct DummyMCP {
  void pinMode(uint8_t, uint8_t) {}
  void digitalWrite(uint8_t, uint8_t) {}
};
static DummyMCP mcp;
static inline bool is_mcp_pin(gpio_num_t) {
  return false;
}
#endif

// TODO: Ensure valid values at run-time
// User can update all these values via Settings page
bool contactor_control_enabled = false;  //Should GPIO contactor control be performed?
uint16_t precharge_time_ms = 100;        //Precharge time in ms. Adjust depending on capacitance in inverter
bool pwm_contactor_control = false;      //Should the contactors be economized via PWM after they are engaged?
bool contactor_control_enabled_double_battery = false;  //Should a contactor for the secondary battery be operated?
bool remote_bms_reset = false;                          //Is it possible to actuate BMS reset via MQTT?
bool periodic_bms_reset = false;                        //Should periodic BMS reset be performed each 24h?

// Parameters
enum State { DISCONNECTED, START_PRECHARGE, PRECHARGE, POSITIVE, PRECHARGE_OFF, COMPLETED, SHUTDOWN_REQUESTED };
State contactorStatus = DISCONNECTED;

const int ON = 1;
const int OFF = 0;

#ifdef NC_CONTACTORS  //Normally closed contactors use inverted logic
#undef ON
#define ON 0
#undef OFF
#define OFF 1
#endif  //NC_CONTACTORS

#define MAX_ALLOWED_FAULT_TICKS 1000  //1000 = 10 seconds
#define NEGATIVE_CONTACTOR_TIME_MS \
  500  // Time after negative contactor is turned on, to start precharge (not actual precharge time!)
#define PRECHARGE_COMPLETED_TIME_MS \
  1000  // After successful precharge, resistor is turned off after this delay (and contactors are economized if PWM enabled)
uint16_t pwm_frequency = 20000;
uint16_t pwm_hold_duty = 250;
#define PWM_ON_DUTY 1023
#define PWM_RESOLUTION 10
#define PWM_OFF_DUTY 0  //No need to have this userconfigurable
#define PWM_Positive_Channel 0
#define PWM_Negative_Channel 1
static unsigned long prechargeStartTime = 0;
unsigned long negativeStartTime = 0;
unsigned long prechargeCompletedTime = 0;
unsigned long timeSpentInFaultedMode = 0;
unsigned long currentTime = 0;
unsigned long lastPowerRemovalTime = 0;
unsigned long bmsPowerOnTime = 0;
const unsigned long powerRemovalInterval = 24 * 60 * 60 * 1000;  // 24 hours in milliseconds
const unsigned long bmsWarmupDuration = 3000;

void set(uint8_t pin, bool direction, uint32_t pwm_freq = 0xFFFF) {
#ifdef HW_3LB
  // 3LB – obsługa MCP23017
  if (is_mcp_pin((gpio_num_t)pin)) {
    // MCP nie ma PWM, więc traktujemy to binarnie: HIGH/LOW
    if (pwm_contactor_control && pwm_freq != 0xFFFF) {
      // symulacja PWM: dla uproszczenia używamy HIGH, można dodać softPWM jeśli potrzebne
      DEBUG_PRINTF("mcp.digitalWrite (PWM-sim) pin=%d val=%d\n", pin, (pwm_freq > 0) ? HIGH : LOW);
      mcp.digitalWrite(pin, (pwm_freq > 0) ? HIGH : LOW);
    } else {
      DEBUG_PRINTF("mcp.digitalWrite pin=%d val=%d\n", pin, direction ? HIGH : LOW);
      mcp.digitalWrite(pin, direction ? HIGH : LOW);
    }
    return;
  }
#endif

  // Standardowa obsługa GPIO (dla wszystkich innych płytek)
  if (pwm_contactor_control) {
    if (pwm_freq != 0xFFFF) {
      ledcWrite(pin, pwm_freq);
      return;
    }
  }

  digitalWrite(pin, direction ? HIGH : LOW);
}

// Initialization functions

const char* contactors = "Contactors";

bool init_contactors() {
  // Init contactor pins
  if (contactor_control_enabled) {
    auto posPin = esp32hal->POSITIVE_CONTACTOR_PIN();
    auto negPin = esp32hal->NEGATIVE_CONTACTOR_PIN();
    auto precPin = esp32hal->PRECHARGE_PIN();

    if (!esp32hal->alloc_pins(contactors, posPin, negPin, precPin)) {
      DEBUG_PRINTF("GPIO controlled contactor setup failed\n");
      return false;
    }

    if (pwm_contactor_control) {
      // Setup PWM only for MCU pins. MCP expander doesn't support PWM.
      if (!is_mcp_pin(posPin)) {
        ledcAttachChannel(posPin, pwm_frequency, PWM_RESOLUTION, PWM_Positive_Channel);
        ledcWrite(posPin, PWM_OFF_DUTY);
      } else {
        // Ensure pin is set as output on MCP
        DEBUG_PRINTF("init_contactors: MCP posPin=%d set OUTPUT, LOW\n", (int)posPin);
        mcp.pinMode((uint8_t)posPin, OUTPUT);
        mcp.digitalWrite((uint8_t)posPin, LOW);
      }

      if (!is_mcp_pin(negPin)) {
        ledcAttachChannel(negPin, pwm_frequency, PWM_RESOLUTION, PWM_Negative_Channel);
        ledcWrite(negPin, PWM_OFF_DUTY);
      } else {
        DEBUG_PRINTF("init_contactors: MCP negPin=%d set OUTPUT, LOW\n", (int)negPin);
        mcp.pinMode((uint8_t)negPin, OUTPUT);
        mcp.digitalWrite((uint8_t)negPin, LOW);
      }
    } else {  // Normal CONTACTOR_CONTROL
      if (is_mcp_pin(posPin)) {
        mcp.pinMode((uint8_t)posPin, OUTPUT);
        mcp.digitalWrite((uint8_t)posPin, LOW);
      } else {
        pinMode(posPin, OUTPUT);
        set(posPin, OFF);
      }

      if (is_mcp_pin(negPin)) {
        mcp.pinMode((uint8_t)negPin, OUTPUT);
        mcp.digitalWrite((uint8_t)negPin, LOW);
      } else {
        pinMode(negPin, OUTPUT);
        set(negPin, OFF);
      }
    }

    // Precharge never has PWM regardless of setting
    if (is_mcp_pin(precPin)) {
      DEBUG_PRINTF("init_contactors: MCP precPin=%d set OUTPUT, LOW\n", (int)precPin);
      mcp.pinMode((uint8_t)precPin, OUTPUT);
      mcp.digitalWrite((uint8_t)precPin, LOW);
    } else {
      pinMode(precPin, OUTPUT);
      set(precPin, OFF);
    }
  }

  if (contactor_control_enabled_double_battery) {
    auto second_contactors = esp32hal->SECOND_BATTERY_CONTACTORS_PIN();
    if (!esp32hal->alloc_pins(contactors, second_contactors)) {
      DEBUG_PRINTF("Secondary battery contactor control setup failed\n");
      return false;
    }

    if (is_mcp_pin(second_contactors)) {
      DEBUG_PRINTF("init_contactors: MCP second_contactors=%d set OUTPUT, LOW\n", (int)second_contactors);
      mcp.pinMode((uint8_t)second_contactors, OUTPUT);
      mcp.digitalWrite((uint8_t)second_contactors, LOW);
    } else {
      pinMode(second_contactors, OUTPUT);
      set(second_contactors, OFF);
    }
  }

  // Init BMS contactor
  if (periodic_bms_reset || remote_bms_reset || esp32hal->always_enable_bms_power()) {
    auto pin = esp32hal->BMS_POWER();
    if (!esp32hal->alloc_pins("BMS power", pin)) {
      DEBUG_PRINTF("BMS power setup failed\n");
      return false;
    }
    if (is_mcp_pin(pin)) {
      DEBUG_PRINTF("init_contactors: MCP BMS pin=%d set OUTPUT, HIGH\n", (int)pin);
      mcp.pinMode((uint8_t)pin, OUTPUT);
      mcp.digitalWrite((uint8_t)pin, HIGH);
    } else {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, HIGH);
    }
  }

  return true;
}

static void dbg_contactors(const char* state) {
  logging.print("[");
  logging.print(millis());
  logging.print(" ms] contactors control: ");
  logging.println(state);
}

// Main functions of the handle_contactors include checking if inverter allows for closing, checking battery 2, checking BMS power output, and actual contactor closing/precharge via GPIO
void handle_contactors() {
  if (inverter && inverter->controls_contactor()) {
    datalayer.system.status.inverter_allows_contactor_closing = inverter->allows_contactor_closing();
  }

  auto posPin = esp32hal->POSITIVE_CONTACTOR_PIN();
  auto negPin = esp32hal->NEGATIVE_CONTACTOR_PIN();
  auto prechargePin = esp32hal->PRECHARGE_PIN();
  auto bms_power_pin = esp32hal->BMS_POWER();

  if (bms_power_pin != GPIO_NUM_NC) {
    handle_BMSpower();  // Some batteries need to be periodically power cycled
  }

  if (contactor_control_enabled_double_battery) {
    handle_contactors_battery2();
  }

  if (contactor_control_enabled) {
    // First check if we have any active errors, incase we do, turn off the battery
    if (datalayer.battery.status.bms_status == FAULT) {
      timeSpentInFaultedMode++;
    } else {
      timeSpentInFaultedMode = 0;
    }

    //handle contactor control SHUTDOWN_REQUESTED
    if (timeSpentInFaultedMode > MAX_ALLOWED_FAULT_TICKS) {
      contactorStatus = SHUTDOWN_REQUESTED;
    }

    if (contactorStatus == SHUTDOWN_REQUESTED) {
      set(prechargePin, OFF);
      set(negPin, OFF, PWM_OFF_DUTY);
      set(posPin, OFF, PWM_OFF_DUTY);
      set_event(EVENT_ERROR_OPEN_CONTACTOR, 0);
      datalayer.system.status.contactors_engaged = 2;
      return;  // A fault scenario latches the contactor control. It is not possible to recover without a powercycle (and investigation why fault occured)
    }

    // After that, check if we are OK to start turning on the battery
    if (contactorStatus == DISCONNECTED) {
      set(prechargePin, OFF);
      set(negPin, OFF, PWM_OFF_DUTY);
      set(posPin, OFF, PWM_OFF_DUTY);
      datalayer.system.status.contactors_engaged = 0;

      if (datalayer.system.status.inverter_allows_contactor_closing && !datalayer.system.info.equipment_stop_active) {
        contactorStatus = START_PRECHARGE;
      }
    }

    // In case the inverter requests contactors to open, set the state accordingly
    if (contactorStatus == COMPLETED) {
      //Incase inverter (or estop) requests contactors to open, make state machine jump to Disconnected state (recoverable)
      if (!datalayer.system.status.inverter_allows_contactor_closing || datalayer.system.info.equipment_stop_active) {
        contactorStatus = DISCONNECTED;
      }
      // Skip running the state machine below if it has already completed
      return;
    }

    currentTime = millis();

    if (currentTime < INTERVAL_10_S) {
      // Skip running the state machine before system has started up.
      // Gives the system some time to detect any faults from battery before blindly just engaging the contactors
      return;
    }

    // Handle actual state machine. This first turns on Negative, then Precharge, then Positive, and finally turns OFF precharge
    switch (contactorStatus) {
      case START_PRECHARGE:
        set(negPin, ON, PWM_ON_DUTY);
        dbg_contactors("NEGATIVE");
        prechargeStartTime = currentTime;
        contactorStatus = PRECHARGE;
        break;

      case PRECHARGE:
        if (currentTime - prechargeStartTime >= NEGATIVE_CONTACTOR_TIME_MS) {
          set(prechargePin, ON);
          dbg_contactors("PRECHARGE");
          negativeStartTime = currentTime;
          contactorStatus = POSITIVE;
        }
        break;

      case POSITIVE:
        if (currentTime - negativeStartTime >= precharge_time_ms) {
          set(posPin, ON, PWM_ON_DUTY);
          dbg_contactors("POSITIVE");
          prechargeCompletedTime = currentTime;
          contactorStatus = PRECHARGE_OFF;
        }
        break;

      case PRECHARGE_OFF:
        if (currentTime - prechargeCompletedTime >= PRECHARGE_COMPLETED_TIME_MS) {
          set(prechargePin, OFF);
          set(negPin, ON, pwm_hold_duty);
          set(posPin, ON, pwm_hold_duty);
          dbg_contactors("PRECHARGE_OFF");
          contactorStatus = COMPLETED;
          datalayer.system.status.contactors_engaged = 1;
        }
        break;
      default:
        break;
    }
  }
}

// Handle contactor logic for the second battery (if present)
void handle_contactors_battery2() {
  // Ensure we actually have a second battery configured
  if (!battery2) {
    return;
  }

  auto secondPin = esp32hal->SECOND_BATTERY_CONTACTORS_PIN();
  if (secondPin == GPIO_NUM_NC) {
    return;  // pin not configured for this HW
  }

  // Only allow closing if system/inverter/battery permit it
  bool allowed = datalayer.system.status.battery2_allowed_contactor_closing &&
                 datalayer.system.status.inverter_allows_contactor_closing &&
                 !datalayer.system.info.equipment_stop_active && (datalayer.battery2.status.bms_status != FAULT);

  if (allowed) {
    set(secondPin, ON, pwm_hold_duty);
    datalayer.system.status.contactors_battery2_engaged = true;
  } else {
    set(secondPin, OFF, PWM_OFF_DUTY);
    datalayer.system.status.contactors_battery2_engaged = false;
  }
}

/* PERIODIC_BMS_RESET - Once every 24 hours we remove power from the BMS_power pin for 30 seconds.
REMOTE_BMS_RESET - Allows the user to remotely powercycle the BMS by sending a command to the emulator via MQTT.

This makes the BMS recalculate all SOC% and avoid memory leaks
During that time we also set the emulator state to paused in order to not try and send CAN messages towards the battery
Feature is only used if user has enabled PERIODIC_BMS_RESET */

void handle_BMSpower() {
  if (periodic_bms_reset || remote_bms_reset) {
    auto bms_power_pin = esp32hal->BMS_POWER();

    // Get current time
    currentTime = millis();

    if (periodic_bms_reset) {
      // Check if 24 hours have passed since the last power removal
      if (currentTime - lastPowerRemovalTime >= powerRemovalInterval) {
        start_bms_reset();
      }
    }

    // If power has been removed for user configured interval (1-59 seconds), restore the power
    if (datalayer.system.status.BMS_reset_in_progress &&
        currentTime - lastPowerRemovalTime >= datalayer.battery.settings.user_set_bms_reset_duration_ms) {
      // Reapply power to the BMS
      digitalWrite(bms_power_pin, HIGH);
      bmsPowerOnTime = currentTime;
      datalayer.system.status.BMS_reset_in_progress = false;   // Reset the power removal flag
      datalayer.system.status.BMS_startup_in_progress = true;  // Set the BMS warmup flag
    }
    //if power has been restored we need to wait a couple of seconds to unpause the battery
    if (datalayer.system.status.BMS_startup_in_progress && currentTime - bmsPowerOnTime >= bmsWarmupDuration) {

      setBatteryPause(false, false, false, false);

      datalayer.system.status.BMS_startup_in_progress = false;  // Reset the BMS warmup removal flag
      set_event(EVENT_PERIODIC_BMS_RESET, 0);
      clear_event(EVENT_PERIODIC_BMS_RESET);
    }
  }
}

void start_bms_reset() {
  if (periodic_bms_reset || remote_bms_reset) {
    auto bms_power_pin = esp32hal->BMS_POWER();

    if (!datalayer.system.status.BMS_reset_in_progress) {
      lastPowerRemovalTime = currentTime;  // Record the time when BMS reset was started
                                           // we are now resetting at the correct time. We don't need to offset anymore
      // Set a flag to let the rest of the system know we are cutting power to the BMS.
      // The battery CAN sending routine will then know not to try guto send anything towards battery while active
      datalayer.system.status.BMS_reset_in_progress = true;

      // Set emulator state to paused (Max Charge/Discharge = 0 & CAN = stop)
      // We try to keep contactors engaged during this pause, and just ramp power down to 0.
      setBatteryPause(true, false, false, false);

      digitalWrite(bms_power_pin, LOW);  // Remove power by setting the BMS power pin to LOW
    }
  }
}
