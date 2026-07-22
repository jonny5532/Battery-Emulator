#include "mqtt.h"

#include <algorithm>
#include <vector>

#include "../../battery/BATTERIES.h"
#include "../../datalayer/datalayer.h"
#include "../../datalayer/datalayer_extended.h"
#include "../../devboard/hal/hal.h"
#include "../../devboard/safety/safety.h"
#include "../utils/events.h"
#include "../utils/logging.h"
#include "../utils/timer.h"

#include "linearjson.h"

#include <WiFi.h>
#include "mqtt_client.h"

#define MAX_AMOUNT_CELLS 192
#define MQTT_QOS 0  // MQTT Quality of Service (0, 1, or 2) //TODO: Should this be configurable?

static MyTimer publish_global_timer(0);  // Will be configured with mqtt_publish_interval_ms on first use
static MyTimer check_global_timer(
    800);  // check timmer - low-priority MQTT checks, where responsiveness is not critical.

static const int mqtt_port_default = 0;
static const char* mqtt_server_default = "";

int mqtt_port = mqtt_port_default;
std::string mqtt_server = mqtt_server_default;
std::string mqtt_user;
std::string mqtt_password;
bool mqtt_enabled = false;
uint16_t mqtt_timeout_ms = 2000;
bool mqtt_transmit_all_cellvoltages = false;
uint16_t mqtt_publish_interval_ms = 5000;
bool ha_autodiscovery_enabled = false;
std::string ha_autodiscovery_topic = "homeassistant";

char mqtt_msg[MQTT_MSG_BUFFER_SIZE];

static bool ha_cell_voltages_published = false;
static bool ha_common_info_published = false;
static esp_mqtt_client_config_t mqtt_cfg;
static esp_mqtt_client_handle_t client;
static char topic_name[128] = "";
static bool client_started = false;

extern bool ota_active;
extern bool remote_bms_reset;

void hold_pins_across_reset();
void start_bms_reset();

static bool ha_events_published = false;

namespace Var {
static const char* battery_name_suffix = "";
static const char* battery_number_suffix = "";
static uint32_t cell_number;
static uint32_t cell_number0;
static const char* hostname = "";
static const char* button_name = "";
static const char* button_id = "";
static const char* button_icon = nullptr;  // nullptr = omit icon
static const char* event_type = "";
static const char* severity = "";
static const char* count = "";
static const char* data = "";
static const char* message = "";
static const char* millis = "";
}  // namespace Var

static bool mqtt_publish(const char* topic, const char* mqtt_msg, bool retain) {
  logging.printf("MQTT [%s]: %s\n", topic, mqtt_msg);
  return true;
  int msg_id = esp_mqtt_client_publish(client, topic, mqtt_msg, strlen(mqtt_msg), MQTT_QOS, retain);
  return msg_id > -1;
}

auto object_header = Json::MakePayload(Json::ObjStart(), Json::End());

auto object_footer = Json::MakePayload(Json::ObjEnd(), Json::End());

auto common_discovery_attributes = Json::MakePayload(
    Json::KeyVal("device", Json::ObjStart()), Json::KeyVal("identifiers", Json::ListStart()), Json::Val(&Var::hostname),
    Json::ListEnd(), Json::KeyVal("manufacturer", "DalaTech"), Json::KeyVal("model", "Battery Emulator"),
    Json::KeyVal("name", &Var::hostname), Json::ObjEnd(), Json::KeyVal("availability", Json::ListStart()),
    Json::KeyVal("topic", &Var::hostname, "/status"), Json::ListEnd(), Json::KeyVal("payload_available", "online"),
    Json::KeyVal("payload_not_available", "offline"), Json::KeyVal("enabled_by_default", true), Json::End());

static std::vector<EventData> order_events;

// --- publish_events ---
// Publishes event data and HA autodiscovery config for the event sensor.

auto event_discovery_attributes =
    Json::MakePayload(Json::KeyVal("name", "Event"), Json::KeyVal("state_topic", &Var::hostname, "/events"),
                      Json::KeyVal("unique_id", &Var::hostname, "_event"),
                      Json::KeyVal("default_entity_id", "sensor.", &Var::hostname, "_event"),
                      Json::KeyVal("value_template",
                                   "{{ value_json.event_type ~ ' (c:' ~ value_json.count ~ ',m:' ~  value_json.millis "
                                   "~ ') ' ~ value_json.message }}"),
                      Json::KeyVal("json_attributes_topic", &Var::hostname, "/events"),
                      Json::KeyVal("json_attributes_template", "{{ value_json | tojson }}"),
                      Json::KeyVal("icon", "mdi:information-outline"), Json::End());

const JsonItem* event_discovery_segments[] = {object_header.data(), event_discovery_attributes.data(),
                                              common_discovery_attributes.data(), object_footer.data()};

auto event_data_attributes =
    Json::MakePayload(Json::KeyVal("event_type", &Var::event_type), Json::KeyVal("severity", &Var::severity),
                      Json::KeyVal("count", &Var::count), Json::KeyVal("data", &Var::data),
                      Json::KeyVal("message", &Var::message), Json::KeyVal("millis", &Var::millis), Json::End());

const JsonItem* event_data_segments[] = {object_header.data(), event_data_attributes.data(), object_footer.data()};

bool publish_events() {
  char hostname_buf[64];
  snprintf(hostname_buf, sizeof(hostname_buf), "%s", WiFi.getHostname());
  Var::hostname = hostname_buf;

  // --- Autodiscovery phase ---
  if (ha_autodiscovery_enabled && !ha_events_published) {
    JsonState json_state = {.segments = event_discovery_segments, .seg_count = 4};

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/sensor/%s/event/config", ha_autodiscovery_topic.c_str(), hostname_buf);
    logging.printf("MQTT [%s]: ", topic);

    json_serialize(&json_state, mqtt_msg, sizeof(mqtt_msg));

    if (mqtt_publish(topic, mqtt_msg, true)) {
      ha_events_published = true;
    } else {
      return false;
    }
  } else {
    // --- Event data phase ---
    const EVENTS_STRUCT_TYPE* event_pointer;

    order_events.clear();
    for (int i = 0; i < EVENT_NOF_EVENTS; i++) {
      event_pointer = get_event_pointer((EVENTS_ENUM_TYPE)i);
      if (event_pointer->occurences > 0 && !event_pointer->MQTTpublished) {
        order_events.push_back({static_cast<EVENTS_ENUM_TYPE>(i), event_pointer});
      }
    }
    std::sort(order_events.begin(), order_events.end(), compareEventsByTimestampAsc);

    char event_type_buf[64];
    char severity_buf[16];
    char count_buf[12];
    char data_buf[12];
    char millis_buf[24];

    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/events", hostname_buf);

    for (const auto& event : order_events) {
      snprintf(event_type_buf, sizeof(event_type_buf), "%s", get_event_enum_string(event.event_handle));
      snprintf(severity_buf, sizeof(severity_buf), "%s", get_event_level_string(event.event_handle));
      snprintf(count_buf, sizeof(count_buf), "%u", event.event_pointer->occurences);
      snprintf(data_buf, sizeof(data_buf), "%u", event.event_pointer->data);
      snprintf(millis_buf, sizeof(millis_buf), "%llu", event.event_pointer->timestamp);

      Var::event_type = event_type_buf;
      Var::severity = severity_buf;
      Var::count = count_buf;
      Var::data = data_buf;
      Var::message = get_event_message_string(event.event_handle).c_str();
      Var::millis = millis_buf;

      JsonState json_state = {.segments = event_data_segments, .seg_count = 3};
      json_serialize(&json_state, mqtt_msg, sizeof(mqtt_msg));

      if (!mqtt_publish(state_topic, mqtt_msg, false)) {
        logging.println("Event MQTT msg could not be sent");
        return false;
      } else {
        set_event_MQTTpublished(event.event_handle);
      }
    }
    order_events.clear();
  }

  return true;
}

auto button_discovery_attributes = Json::MakePayload(
    Json::KeyVal("name", &Var::button_name), Json::KeyVal("unique_id", &Var::hostname, "_", &Var::button_id),
    Json::KeyVal("command_topic", &Var::hostname, "/command/", &Var::button_id),
    Json::KeyVal("icon", &Var::button_icon),  // null ptr skips the field
    Json::End());

auto battery_voltage_attribues = Json::MakePayload(
    Json::KeyVal("name", "Battery", &Var::battery_name_suffix, " Cell Voltage ", &Var::cell_number),
    Json::KeyVal("default_entity_id", "sensor.battery_voltage_cell", &Var::cell_number),
    Json::KeyVal("unique_id", &Var::hostname, "_", &Var::hostname, "_battery_voltage_cell", &Var::cell_number),
    Json::KeyVal("device_class", "voltage"), Json::KeyVal("state_class", "measurement"),
    Json::KeyVal("state_topic", &Var::hostname, "/spec_data", &Var::battery_number_suffix),
    Json::KeyVal("unit_of_measurement", "V"), Json::KeyVal("suggested_display_precision", 3),
    Json::KeyVal("icon", "mdi:current-dc"),
    Json::KeyVal("value_template", "{{ value_json.cell_voltages[", &Var::cell_number0, "] }}"), Json::End());

const JsonItem* cell_voltage_segments[] = {object_header.data(), battery_voltage_attribues.data(),
                                           common_discovery_attributes.data(), object_footer.data()};

// --- publish_common_info ---
// Sensor config struct (stored in flash)
struct SensorConfig {
  const char* entity_id;
  const char* name;
  const char* unit;
  const char* device_class;
  uint8_t flags;
};

// Condition flags for sensor configs
static constexpr uint8_t COND_ALWAYS = 0;
static constexpr uint8_t COND_CHARGED_ENERGY = (1 << 0);
static constexpr uint8_t COND_TESLA_DCDC = (1 << 1);
static constexpr uint8_t COND_BYD_AUTOCAL = (1 << 2);

static constexpr SensorConfig batterySensorConfigTemplate[] = {
    {"SOC", "SOC (Scaled)", "%", "battery", COND_ALWAYS},
    {"SOC_real", "SOC (real)", "%", "battery", COND_ALWAYS},
    {"state_of_health", "State of Health", "%", "battery", COND_ALWAYS},
    {"temperature_min", "Temperature Min", "°C", "temperature", COND_ALWAYS},
    {"temperature_max", "Temperature Max", "°C", "temperature", COND_ALWAYS},
    {"stat_batt_power", "Battery Power", "W", "power", COND_ALWAYS},
    {"battery_current", "Battery Current", "A", "current", COND_ALWAYS},
    {"cell_max_voltage", "Cell Max Voltage", "V", "voltage", COND_ALWAYS},
    {"cell_min_voltage", "Cell Min Voltage", "V", "voltage", COND_ALWAYS},
    {"cell_voltage_delta", "Cell Voltage Delta", "mV", "voltage", COND_ALWAYS},
    {"battery_voltage", "Battery Voltage", "V", "voltage", COND_ALWAYS},
    {"total_capacity", "Total Capacity", "Wh", "energy", COND_ALWAYS},
    {"remaining_capacity", "Remaining Capacity (scaled)", "Wh", "energy", COND_ALWAYS},
    {"remaining_capacity_real", "Remaining Capacity (real)", "Wh", "energy", COND_ALWAYS},
    {"max_discharge_power", "Max Discharge Power", "W", "power", COND_ALWAYS},
    {"max_charge_power", "Max Charge Power", "W", "power", COND_ALWAYS},
    {"charged_energy", "Battery Charged Energy", "Wh", "energy", COND_CHARGED_ENERGY},
    {"discharged_energy", "Battery Discharged Energy", "Wh", "energy", COND_CHARGED_ENERGY},
    {"balancing_active_cells", "Balancing Active Cells", "", "", COND_ALWAYS},
    {"balancing_status", "Balancing Status", "", "", COND_ALWAYS},
    {"charging_state", "Charging State", "", "", COND_ALWAYS},
    {"limiting_factor", "Limiting Factor", "", "", COND_ALWAYS},
    {"dc_dc_current", "DC-DC Current", "A", "current", COND_TESLA_DCDC},
    {"dc_dc_voltage", "DC-DC Voltage", "V", "voltage", COND_TESLA_DCDC},
    {"autocal_taper", "BYD Auto-cal: In Taper", "", "", COND_BYD_AUTOCAL},
    {"autocal_dwell_s", "BYD Auto-cal: Dwell Time", "s", "duration", COND_BYD_AUTOCAL},
    {"autocal_cooldown_ready", "BYD Auto-cal: Cooldown Ready", "", "", COND_BYD_AUTOCAL},
    {"autocal_soc_drift", "BYD Auto-cal: SOC Drift", "%", "battery", COND_BYD_AUTOCAL},
};

static constexpr SensorConfig globalSensorConfigTemplate[] = {
    {"bms_status", "BMS Status", "", "", COND_ALWAYS},
    {"pause_status", "Pause Status", "", "", COND_ALWAYS},
    {"event_level", "Event Level", "", "", COND_ALWAYS},
    {"emulator_status", "Emulator Status", "", "", COND_ALWAYS},
    {"emulator_uptime", "Emulator Uptime", "s", "duration", COND_ALWAYS},
    {"cpu_temp", "CPU Temperature", "°C", "temperature", COND_ALWAYS},
};

static const char* sensor_discovery_icon(const char* entity_id, const char* device_class) {
  if (entity_id != nullptr) {
    if (strncmp(entity_id, "balancing_active_cells", strlen("balancing_active_cells")) == 0 ||
        strncmp(entity_id, "balancing_status", strlen("balancing_status")) == 0) {
      return "mdi:fuel-cell";
    }
    if (strncmp(entity_id, "bms_status", strlen("bms_status")) == 0) {
      return "mdi:information-box-outline";
    }
    if (strncmp(entity_id, "charging_state", strlen("charging_state")) == 0) {
      return "mdi:home-battery";
    }
    if (strncmp(entity_id, "limiting_factor", strlen("limiting_factor")) == 0) {
      return "mdi:home-battery-outline";
    }
    if (strncmp(entity_id, "emulator_status", strlen("emulator_status")) == 0 ||
        strncmp(entity_id, "event_level", strlen("event_level")) == 0) {
      return "mdi:information-outline";
    }
    if (strncmp(entity_id, "pause_status", strlen("pause_status")) == 0) {
      return "mdi:battery-outline";
    }
  }
  if (device_class != nullptr) {
    if (strcmp(device_class, "voltage") == 0)
      return "mdi:current-dc";
    if (strcmp(device_class, "current") == 0)
      return "mdi:equal";
  }
  return nullptr;
}

// Discovery payload template for common info sensors (flash-resident)
namespace Var {
static const char* name = "";
static const char* value_template = "";
static const char* unit_of_measurement = nullptr;          // nullptr = omit
static const char* device_class = nullptr;                 // nullptr = omit
static const char* state_class = nullptr;                  // nullptr = omit
static const char* suggested_display_precision = nullptr;  // nullptr = omit
static const char* icon = nullptr;                         // nullptr = omit
static const char* default_entity_id = "";
}  // namespace Var

auto common_info_attributes =
    Json::MakePayload(Json::KeyVal("name", &Var::name), Json::KeyVal("state_topic", &Var::hostname, "/info"),
                      Json::KeyVal("unique_id", &Var::hostname, "_", &Var::default_entity_id),
                      Json::KeyVal("default_entity_id", "sensor.", &Var::default_entity_id),
                      Json::KeyVal("value_template", &Var::value_template),
                      Json::KeyVal("unit_of_measurement", &Var::unit_of_measurement),
                      Json::KeyVal("device_class", &Var::device_class), Json::KeyVal("state_class", &Var::state_class),
                      Json::KeyVal("suggested_display_precision", &Var::suggested_display_precision),
                      Json::KeyVal("icon", &Var::icon), Json::End());

const JsonItem* common_info_segments[] = {object_header.data(), common_info_attributes.data(),
                                          common_discovery_attributes.data(), object_footer.data()};

bool publish_common_info(void) {
  char hostname_buf[64];
  snprintf(hostname_buf, sizeof(hostname_buf), "%s", WiFi.getHostname());
  Var::hostname = hostname_buf;

  // char lwt_topic_buf[128];
  // snprintf(lwt_topic_buf, sizeof(lwt_topic_buf), "%s/status", hostname_buf);
  // Var::lwt_topic = lwt_topic_buf;

  //Var::default_entity_id_prefix = hostname_buf;  // not used in common_info_segments but available

  // --- Autodiscovery phase ---
  if (ha_autodiscovery_enabled && !ha_common_info_published) {
    // Publish battery sensor configs
    for (size_t i = 0; i < sizeof(batterySensorConfigTemplate) / sizeof(batterySensorConfigTemplate[0]); i++) {
      const auto& config = batterySensorConfigTemplate[i];

      // Check condition
      if (config.flags & COND_CHARGED_ENERGY) {
        if (!battery || !battery->supports_charged_energy())
          continue;
      }
      if (config.flags & COND_TESLA_DCDC) {
        if (!battery || (user_selected_battery_type != BatteryType::TeslaModel3Y &&
                         user_selected_battery_type != BatteryType::TeslaModelSX))
          continue;
      }
      if (config.flags & COND_BYD_AUTOCAL) {
        if (!battery || user_selected_battery_type != BatteryType::BydAtto3)
          continue;
      }

      String value_template_str = "{{ value_json." + String(config.entity_id) + " }}";

      Var::name = config.name;
      Var::value_template = value_template_str.c_str();
      Var::unit_of_measurement = (config.unit[0] != '\0') ? config.unit : nullptr;
      Var::device_class = (config.device_class[0] != '\0') ? config.device_class : nullptr;
      Var::state_class = nullptr;
      Var::suggested_display_precision = nullptr;
      Var::default_entity_id = config.entity_id;

      // state_class logic
      if (Var::device_class != nullptr && strlen(Var::device_class) > 0) {
        Var::state_class = "measurement";
      }
      if (strncmp(config.entity_id, "balancing_active_cells", strlen("balancing_active_cells")) == 0) {
        Var::state_class = "measurement";
      }
      // Energy class adjustments
      if (strncmp(config.entity_id, "total_capacity", strlen("total_capacity")) == 0 ||
          strncmp(config.entity_id, "remaining_capacity", strlen("remaining_capacity")) == 0) {
        Var::device_class = "energy_storage";
      } else if (strncmp(config.entity_id, "charged_energy", strlen("charged_energy")) == 0 ||
                 strncmp(config.entity_id, "discharged_energy", strlen("discharged_energy")) == 0) {
        Var::state_class = "total_increasing";
      }
      // Display precision
      if (strncmp(config.entity_id, "cell_max_voltage", strlen("cell_max_voltage")) == 0 ||
          strncmp(config.entity_id, "cell_min_voltage", strlen("cell_min_voltage")) == 0) {
        Var::suggested_display_precision = "3";
      }
      if (strncmp(config.entity_id, "battery_current", strlen("battery_current")) == 0 ||
          strncmp(config.entity_id, "SOC", strlen("SOC")) == 0) {
        Var::suggested_display_precision = "1";
      }
      // Icon
      Var::icon = sensor_discovery_icon(config.entity_id, config.device_class);

      JsonState json_state = {.segments = common_info_segments, .seg_count = 4};

      char topic[128];
      snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", hostname_buf, config.entity_id);
      logging.printf("MQTT [%s]: ", topic);

      char chunk[128];
      int n;
      while ((n = json_serialize(&json_state, chunk, sizeof(chunk))) > 0) {
        logging.printf("%.*s", n, chunk);
      }
      logging.println();
    }

    // Publish global sensor configs
    for (size_t i = 0; i < sizeof(globalSensorConfigTemplate) / sizeof(globalSensorConfigTemplate[0]); i++) {
      const auto& config = globalSensorConfigTemplate[i];

      String value_template_str = "{{ value_json." + String(config.entity_id) + " }}";

      Var::name = config.name;
      Var::value_template = value_template_str.c_str();
      Var::unit_of_measurement = (config.unit[0] != '\0') ? config.unit : nullptr;
      Var::device_class = (config.device_class[0] != '\0') ? config.device_class : nullptr;
      Var::state_class = nullptr;
      Var::suggested_display_precision = nullptr;
      Var::default_entity_id = config.entity_id;
      Var::icon = sensor_discovery_icon(config.entity_id, config.device_class);

      if (Var::device_class != nullptr && strlen(Var::device_class) > 0) {
        Var::state_class = "measurement";
      }

      JsonState json_state = {.segments = common_info_segments, .seg_count = 4};

      char topic[128];
      snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", hostname_buf, config.entity_id);
      logging.printf("MQTT [%s]: ", topic);

      char chunk[128];
      int n;
      while ((n = json_serialize(&json_state, chunk, sizeof(chunk))) > 0) {
        logging.printf("%.*s", n, chunk);
      }
      logging.println();
    }

    ha_common_info_published = true;
  }

  // --- State data phase ---
  // Pre-compute all values into a struct so pointers remain stable during serialization
  struct CommonInfoData {
    float SOC;
    float SOC_real;
    float state_of_health;
    float temperature_min;
    float temperature_max;
    float stat_batt_power;
    float battery_current;
    float battery_voltage;
    float cell_max_voltage;
    float cell_min_voltage;
    float cell_voltage_delta;
    float total_capacity;
    float remaining_capacity;
    float remaining_capacity_real;
    float max_discharge_power;
    float max_charge_power;
    float charged_energy;
    float discharged_energy;
    uint32_t balancing_active_cells;
    const char* balancing_status;
    const char* charging_state;
    const char* limiting_factor;
    float dc_dc_current;
    float dc_dc_voltage;
    bool autocal_taper;
    uint32_t autocal_dwell_s;
    bool autocal_cooldown_ready;
    float autocal_soc_drift;
  };

  // Helper lambda: populate CommonInfoData from a battery instance
  auto populate_battery = [](CommonInfoData& d, const DATALAYER_BATTERY_TYPE& b, bool supports_charged,
                             bool is_primary) {
    d.SOC = ((float)b.status.reported_soc) / 100.0f;
    d.SOC_real = ((float)b.status.real_soc) / 100.0f;
    d.state_of_health = ((float)b.status.soh_pptt) / 100.0f;
    d.temperature_min = ((float)((int16_t)b.status.temperature_min_dC)) / 10.0f;
    d.temperature_max = ((float)((int16_t)b.status.temperature_max_dC)) / 10.0f;
    d.stat_batt_power = ((float)((int32_t)b.status.active_power_W));
    d.battery_current = ((float)((int16_t)b.status.current_dA)) / 10.0f;
    d.battery_voltage = ((float)b.status.voltage_dV) / 10.0f;
    if (b.info.number_of_cells != 0u && b.status.cell_voltages_mV[b.info.number_of_cells - 1] != 0u) {
      d.cell_max_voltage = ((float)b.status.cell_max_voltage_mV) / 1000.0f;
      d.cell_min_voltage = ((float)b.status.cell_min_voltage_mV) / 1000.0f;
      d.cell_voltage_delta = ((float)b.status.cell_max_voltage_mV) - ((float)b.status.cell_min_voltage_mV);
    } else {
      d.cell_max_voltage = 0;
      d.cell_min_voltage = 0;
      d.cell_voltage_delta = 0;
    }
    d.total_capacity = ((float)b.info.total_capacity_Wh);
    d.remaining_capacity_real = ((float)b.status.remaining_capacity_Wh);
    d.remaining_capacity = ((float)b.status.reported_remaining_capacity_Wh);
    d.max_discharge_power = ((float)b.status.max_discharge_power_W);
    d.max_charge_power = ((float)b.status.max_charge_power_W);

    if (supports_charged) {
      if (datalayer.battery.status.total_charged_battery_Wh != 0 &&
          datalayer.battery.status.total_discharged_battery_Wh != 0) {
        d.charged_energy = ((float)datalayer.battery.status.total_charged_battery_Wh);
        d.discharged_energy = ((float)datalayer.battery.status.total_discharged_battery_Wh);
      } else {
        d.charged_energy = 0;
        d.discharged_energy = 0;
      }
    } else {
      d.charged_energy = 0;
      d.discharged_energy = 0;
    }

    uint16_t active_cells = 0;
    if (b.info.number_of_cells != 0u) {
      for (size_t j = 0; j < b.info.number_of_cells; ++j) {
        if (b.status.cell_balancing_status[j])
          active_cells++;
      }
    }
    d.balancing_active_cells = active_cells;

    switch (b.status.balancing_status) {
      case BALANCING_STATUS_ACTIVE:
        d.balancing_status = "Active";
        break;
      case BALANCING_STATUS_READY:
        d.balancing_status = "Ready";
        break;
      case BALANCING_STATUS_BLOCKED:
        d.balancing_status = "Blocked";
        break;
      case BALANCING_STATUS_ERROR:
        d.balancing_status = "Error";
        break;
      default:
        d.balancing_status = "Unknown";
        break;
    }

    ChargingState cs = get_charging_state(b.status.current_dA);
    d.charging_state = charging_state_to_text(cs);
    d.limiting_factor = limiting_factor_to_text(
        get_limiting_factor(cs, b.settings.inverter_limits_charge, b.settings.inverter_limits_discharge,
                            b.settings.user_settings_limit_charge, b.settings.user_settings_limit_discharge));

    // Tesla DC-DC: only for primary battery (matches original mqtt.cpp)
    if (is_primary && (user_selected_battery_type == BatteryType::TeslaModel3Y ||
                       user_selected_battery_type == BatteryType::TeslaModelSX)) {
      d.dc_dc_current = static_cast<float>(datalayer_extended.tesla.battery_dcdcLvOutputCurrent) * 0.1f;
      d.dc_dc_voltage = static_cast<float>(datalayer_extended.tesla.battery_dcdcLvBusVolt) * 0.0390625f;
    } else {
      d.dc_dc_current = 0;
      d.dc_dc_voltage = 0;
    }

    // BYD autocal: only for primary battery (matches original mqtt.cpp)
    if (is_primary && user_selected_battery_type == BatteryType::BydAtto3) {
      const DATALAYER_INFO_BYDATTO3& byd = datalayer_extended.bydAtto3;
      d.autocal_taper = byd.autocal_crit_taper;
      d.autocal_dwell_s = byd.autocal_dwell_accumulated_ms / 1000u;
      d.autocal_cooldown_ready = byd.autocal_crit_cooldown_ready;
      d.autocal_soc_drift = byd.autocal_drift_percent;
    } else {
      d.autocal_taper = false;
      d.autocal_dwell_s = 0;
      d.autocal_cooldown_ready = false;
      d.autocal_soc_drift = 0;
    }
  };

  // Global values (computed once)
  std::string bms_status_str = getBMSStatus(datalayer.system.status.system_status);
  std::string pause_status_str = get_emulator_pause_status();

  uint32_t cpu_temp = (uint32_t)(datalayer.system.info.CPU_temperature + 0.5f);
  uint32_t emulator_uptime = (uint32_t)(millis64() / 1000);
  const char* event_level = get_event_level_string(get_event_level());
  const char* emulator_status = get_emulator_status_string(get_emulator_status());

  // Battery 1 data
  CommonInfoData d1 = {};
  if (datalayer.battery.status.CAN_battery_still_alive && allowed_to_send_CAN && esp32hal->system_booted_up()) {
    populate_battery(d1, datalayer.battery, battery->supports_charged_energy(), true);
  }

  // Build the state payload using pointers to the pre-computed struct fields
  auto doc = Json::MakePayload(
      Json::ObjStart(), Json::KeyVal("bms_status", bms_status_str.c_str()),
      Json::KeyVal("pause_status", pause_status_str.c_str()), Json::KeyVal("SOC", &d1.SOC),
      Json::KeyVal("SOC_real", &d1.SOC_real), Json::KeyVal("state_of_health", &d1.state_of_health),
      Json::KeyVal("temperature_min", &d1.temperature_min), Json::KeyVal("temperature_max", &d1.temperature_max),
      Json::KeyVal("stat_batt_power", &d1.stat_batt_power), Json::KeyVal("battery_current", &d1.battery_current),
      Json::KeyVal("cell_max_voltage", &d1.cell_max_voltage), Json::KeyVal("cell_min_voltage", &d1.cell_min_voltage),
      Json::KeyVal("cell_voltage_delta", &d1.cell_voltage_delta), Json::KeyVal("battery_voltage", &d1.battery_voltage),
      Json::KeyVal("total_capacity", &d1.total_capacity), Json::KeyVal("remaining_capacity", &d1.remaining_capacity),
      Json::KeyVal("remaining_capacity_real", &d1.remaining_capacity_real),
      Json::KeyVal("max_discharge_power", &d1.max_discharge_power),
      Json::KeyVal("max_charge_power", &d1.max_charge_power), Json::KeyVal("charged_energy", &d1.charged_energy),
      Json::KeyVal("discharged_energy", &d1.discharged_energy),
      Json::KeyVal("balancing_active_cells", &d1.balancing_active_cells),
      Json::KeyVal("balancing_status", d1.balancing_status), Json::KeyVal("charging_state", d1.charging_state),
      Json::KeyVal("limiting_factor", d1.limiting_factor), Json::KeyVal("dc_dc_current", &d1.dc_dc_current),
      Json::KeyVal("dc_dc_voltage", &d1.dc_dc_voltage), Json::KeyVal("autocal_taper", &d1.autocal_taper),
      Json::KeyVal("autocal_dwell_s", &d1.autocal_dwell_s),
      Json::KeyVal("autocal_cooldown_ready", &d1.autocal_cooldown_ready),
      Json::KeyVal("autocal_soc_drift", &d1.autocal_soc_drift), Json::KeyVal("event_level", event_level),
      Json::KeyVal("emulator_status", emulator_status), Json::KeyVal("cpu_temp", cpu_temp),
      Json::KeyVal("emulator_uptime", emulator_uptime), Json::ObjEnd(), Json::End());

  const JsonItem* segments[] = {doc.data()};
  JsonState json_state = {.segments = segments, .seg_count = 1};

  logging.printf("MQTT [%s/info]: ", hostname_buf);
  char chunk[32];
  int n;
  while ((n = json_serialize(&json_state, chunk, sizeof(chunk))) > 0) {
    logging.printf("%.*s", n, chunk);
  }
  logging.println();

  return true;
}

const JsonItem* button_discovery_segments[] = {object_header.data(), button_discovery_attributes.data(),
                                               common_discovery_attributes.data(), object_footer.data()};

static bool publish_cell_voltage_autodiscovery_for_battery(DATALAYER_BATTERY_TYPE& datalayer_battery,
                                                           const char* battery_name_suffix_,
                                                           const char* battery_number_suffix_) {
  char topic_buf[128];

  if (datalayer_battery.info.number_of_cells != 0u) {
    Var::battery_name_suffix = battery_name_suffix_;
    Var::battery_number_suffix = battery_number_suffix_;

    //Var::lwt_topic = "lwt_topic";

    for (int i = 0; i < datalayer_battery.info.number_of_cells; i++) {
      Var::cell_number0 = i;
      Var::cell_number = i + 1;

      JsonState json_state = {.segments = cell_voltage_segments, .seg_count = 4};

      snprintf(topic_buf, sizeof(topic_buf), "homeassistant/sensor/%s/cell_voltage%s%d/config", Var::hostname,
               Var::battery_number_suffix, Var::cell_number);
      logging.printf("MQTT [%s]: ", topic_buf);

      //set_battery_voltage_attributes(doc, i, cellNumber, state_topic, default_entity_id_prefix, "");
      //set_common_discovery_attributes(doc);

      json_serialize(&json_state, mqtt_msg, sizeof(mqtt_msg));

      if (mqtt_publish(topic_buf, mqtt_msg, true) == false) {
        return false;
      }
    }
  }
  return true;
}

bool publish_cell_voltages(void) {
  //static JsonDocument doc;
  //   static String state_topic = topic_name + "/spec_data";
  //   static String state_topic_2 = topic_name + "/spec_data_2";
  //   static String state_topic_3 = topic_name + "/spec_data_3";
  char hostname_buf[64];
  snprintf(hostname_buf, sizeof(hostname_buf), "%s", WiFi.getHostname());
  Var::hostname = hostname_buf;

  if (ha_autodiscovery_enabled) {
    bool successfully_published = false;
    if (ha_cell_voltages_published == false) {

      successfully_published = publish_cell_voltage_autodiscovery_for_battery(datalayer.battery, "", "");
      if (battery2 && successfully_published) {
        successfully_published = publish_cell_voltage_autodiscovery_for_battery(datalayer.battery2, " 2", "_2");
      }
      if (battery3 && successfully_published) {
        successfully_published = publish_cell_voltage_autodiscovery_for_battery(datalayer.battery3, " 3", "_3");
      }

      // // If the cell voltage number isn't initialized...
      // if (datalayer.battery.info.number_of_cells != 0u) {

      //   battery_name_suffix = " 2";
      //   battery_number_suffix = "_2";
      //   lwt_topic = "lwt_topic";

      //   for (int i = 0; i < datalayer.battery.info.number_of_cells; i++) {
      //     cell_number0 = i;
      //     cell_number = i + 1;

      //     JsonState json_state = { .segments = cell_voltage_segments, .seg_count = 4 };

      //   //set_battery_voltage_attributes(doc, i, cellNumber, state_topic, default_entity_id_prefix, "");
      //   //set_common_discovery_attributes(doc);

      //     char chunk[128];
      //     int n;
      //     while ((n = json_serialize(&json_state, chunk, sizeof(chunk))) > 0) {
      //       logging.printf("%.*s", n, chunk);
      //         //printf("%.*s", n, chunk);
      //     }
      //     logging.println();
      //     //printf("x\n");

      //   //   serializeJson(doc, mqtt_msg, sizeof(mqtt_msg));
      //   //   if (mqtt_publish(generateCellVoltageAutoConfigTopic(cellNumber, "").c_str(), mqtt_msg, true) == false) {
      //   //     return false;
      //   //   }
      //   }
      //   successfully_published = true;
      //   //doc.clear();  // clear after sending autoconfig
      // }

      /*
      if (battery2) {
        successfully_published = false;
        // TODO: Combine this identical block with the previous one.
        // If the cell voltage number isn't initialized...
        if (datalayer.battery2.info.number_of_cells != 0u) {

          for (int i = 0; i < datalayer.battery2.info.number_of_cells; i++) {
            int cellNumber = i + 1;
            set_battery_voltage_attributes(doc, i, cellNumber, state_topic_2, default_entity_id_prefix + "2_", " 2");
            set_common_discovery_attributes(doc);

            serializeJson(doc, mqtt_msg, sizeof(mqtt_msg));
            if (mqtt_publish(generateCellVoltageAutoConfigTopic(cellNumber, "_2_").c_str(), mqtt_msg, true) == false) {
              return false;
            }
          }
          successfully_published = true;
          doc.clear();  // clear after sending autoconfig
        }
      }

      if (battery3) {
        successfully_published = false;
        // If the cell voltage number isn't initialized...
        if (datalayer.battery3.info.number_of_cells != 0u) {

          for (int i = 0; i < datalayer.battery3.info.number_of_cells; i++) {
            int cellNumber = i + 1;
            set_battery_voltage_attributes(doc, i, cellNumber, state_topic_3, default_entity_id_prefix + "3_", " 3");
            set_common_discovery_attributes(doc);

            serializeJson(doc, mqtt_msg, sizeof(mqtt_msg));
            if (mqtt_publish(generateCellVoltageAutoConfigTopic(cellNumber, "_3_").c_str(), mqtt_msg, true) == false) {
              return false;
            }
          }
          successfully_published = true;
          doc.clear();  // clear after sending autoconfig
        }
      }*/
    }
    if (successfully_published) {
      ha_cell_voltages_published = true;
    }
  }

  // If cell voltages have been populated...
  if (datalayer.battery.info.number_of_cells != 0u &&
      datalayer.battery.status.cell_voltages_mV[datalayer.battery.info.number_of_cells - 1] != 0u) {

    float cell_voltages[196];
    for (size_t i = 0; i < datalayer.battery.info.number_of_cells; i++) {
      cell_voltages[i] = ((float)datalayer.battery.status.cell_voltages_mV[i]) / 1000.0f;
    }

    auto doc = Json::MakePayload(Json::ObjStart(), Json::KeyVal("cell_voltages", Json::ListStart()),
                                 Json::Array(cell_voltages, datalayer.battery.info.number_of_cells), Json::ListEnd(),
                                 Json::ObjEnd(), Json::End());

    const JsonItem* segments[] = {doc.data()};
    JsonState json_state = {.segments = segments, .seg_count = 1};

    // char chunk[32];
    // int n;
    // while ((n = json_serialize(&json_state, chunk, sizeof(chunk))) > 0) {
    //     printf("%.*s", n, chunk);
    // }
    // printf("\n");

    json_serialize(&json_state, mqtt_msg, sizeof(mqtt_msg));

    //serializeJson(doc, mqtt_msg, sizeof(mqtt_msg));

    char state_topic_buf[128];
    snprintf(state_topic_buf, sizeof(state_topic_buf), "%s/spec_data", hostname_buf);

    if (!mqtt_publish(state_topic_buf, mqtt_msg, false)) {
      logging.println("Cell voltage MQTT msg could not be sent");
      return false;
    }
    //doc.clear();
  }

  Var::hostname = nullptr;

  return true;
}

// --- publish_cell_balancing ---
// Publishes cell balancing status as a boolean array for each battery.
static void publish_cell_balancing_for_battery(DATALAYER_BATTERY_TYPE& datalayer_battery, const char* topic_suffix) {
  if (datalayer_battery.info.number_of_cells == 0u) {
    return;
  }

  bool cell_balancing[MAX_AMOUNT_CELLS];
  for (size_t i = 0; i < datalayer_battery.info.number_of_cells; i++) {
    cell_balancing[i] = datalayer_battery.status.cell_balancing_status[i];
  }

  auto doc = Json::MakePayload(Json::ObjStart(), Json::KeyVal("cell_balancing", Json::ListStart()),
                               Json::Array(cell_balancing, datalayer_battery.info.number_of_cells), Json::ListEnd(),
                               Json::ObjEnd(), Json::End());

  const JsonItem* segments[] = {doc.data()};
  JsonState json_state = {.segments = segments, .seg_count = 1};

  char topic[64];
  snprintf(topic, sizeof(topic), "%s/balancing_data%s", WiFi.getHostname(), topic_suffix);
  logging.printf("MQTT [%s]: ", topic);

  char chunk[32];
  int n;
  while ((n = json_serialize(&json_state, chunk, sizeof(chunk))) > 0) {
    logging.printf("%.*s", n, chunk);
  }
  logging.println();
}

bool publish_cell_balancing(void) {
  publish_cell_balancing_for_battery(datalayer.battery, "");
  if (battery2) {
    publish_cell_balancing_for_battery(datalayer.battery2, "_2");
  }
  if (battery3) {
    publish_cell_balancing_for_battery(datalayer.battery3, "_3");
  }
  return true;
}

// --- publish_buttons_discovery ---
// HA autodiscovery for command buttons (BMSRESET, PAUSE, RESUME, RESTART, STOP).

struct ButtonConfig {
  const char* entity_id;
  const char* name;
  const char* icon;  // MDI icon, or nullptr to omit
};

static ButtonConfig buttonConfigs[] = {
    {"BMSRESET", "Reset BMS", "mdi:star-four-points-box-outline"},
    {"PAUSE", "Pause charge/discharge", "mdi:battery-minus-outline"},
    {"RESUME", "Resume charge/discharge", "mdi:battery-sync-outline"},
    {"RESTART", "Restart Battery Emulator", "mdi:restart"},
    {"STOP", "Open Contactors", "mdi:battery-remove-outline"},
};

static bool ha_buttons_published = false;

bool publish_buttons_discovery(void) {
  if (!ha_autodiscovery_enabled || ha_buttons_published) {
    return true;
  }

  logging.println("Publishing buttons discovery");

  char hostname_buf[64];
  snprintf(hostname_buf, sizeof(hostname_buf), "%s", WiFi.getHostname());
  Var::hostname = hostname_buf;

  // char lwt_topic_buf[128];
  // snprintf(lwt_topic_buf, sizeof(lwt_topic_buf), "%s/status", hostname_buf);
  // Var::lwt_topic = lwt_topic_buf;

  for (size_t i = 0; i < sizeof(buttonConfigs) / sizeof(buttonConfigs[0]); i++) {
    Var::button_name = buttonConfigs[i].name;
    Var::button_id = buttonConfigs[i].entity_id;
    Var::button_icon = buttonConfigs[i].icon;

    JsonState json_state = {.segments = button_discovery_segments, .seg_count = 4};

    char topic[128];
    snprintf(topic, sizeof(topic), "homeassistant/button/%s/%s/config", hostname_buf, buttonConfigs[i].entity_id);
    logging.printf("MQTT [%s]: ", topic);

    char chunk[128];
    int n;
    while ((n = json_serialize(&json_state, chunk, sizeof(chunk))) > 0) {
      logging.printf("%.*s", n, chunk);
    }
    logging.println();
  }

  ha_buttons_published = true;
  return true;
}

static void publish_values(void) {

  char buf[128];
  snprintf(buf, sizeof(buf), "%s/status", topic_name);
  if (mqtt_publish(buf, "online", false) == false) {
    return;
  }

  if (publish_events() == false) {
    return;
  }

  if (publish_common_info() == false) {
    return;
  }

  if (mqtt_transmit_all_cellvoltages) {
    if (publish_cell_voltages() == false) {
      return;
    }
  }

  if (mqtt_transmit_all_cellvoltages) {
    if (publish_cell_balancing() == false) {
      return;
    }
  }
}

static void subscribe() {
  char topic_buf[128];
  snprintf(topic_buf, sizeof(topic_buf), "%s/command/+", topic_name);
  esp_mqtt_client_subscribe(client, topic_buf, 1);
}

void mqtt_message_received(char* topic_raw, int topic_len, char* data, int data_len) {
  char* topic = strndup(topic_raw, topic_len);

  logging.printf("MQTT message arrived: [%.*s]\n", topic_len, topic);

  char button_topic_buf[128];
  auto generateButtonTopic = [&button_topic_buf](const char* button_id) -> char* {
    snprintf(button_topic_buf, sizeof(button_topic_buf), "%s/command/%s", topic_name, button_id);
    return button_topic_buf;
  };

  if (remote_bms_reset) {
    if (strcmp(topic, generateButtonTopic("BMSRESET")) == 0) {
      logging.println("Triggering BMS reset");
      start_bms_reset();
    }
  }

  if (strcmp(topic, generateButtonTopic("PAUSE")) == 0) {
    setBatteryPause(true, false);
  }

  if (strcmp(topic, generateButtonTopic("RESUME")) == 0) {
    setBatteryPause(false, false, EquipmentStop::RESUME);
  }

  if (strcmp(topic, generateButtonTopic("RESTART")) == 0) {
    hold_pins_across_reset();
    graceful_restart();
  }

  if (strcmp(topic, generateButtonTopic("STOP")) == 0) {
    setBatteryPause(true, false, EquipmentStop::STOP);
  }

  if (strcmp(topic, generateButtonTopic("SET_LIMITS")) == 0) {
    /*
    JsonDocument doc;
    char* data_str = strndup(data, data_len);
    deserializeJson(doc, data_str);

    if (doc["max_charge"].is<int>()) {
      datalayer.battery.settings.max_remote_set_charge_dA = doc["max_charge"];
      datalayer.battery.settings.remote_settings_limit_charge = true;
    } else {
      datalayer.battery.settings.max_remote_set_charge_dA = 0;
      datalayer.battery.settings.remote_settings_limit_charge = false;
    }

    if (doc["max_discharge"].is<int>()) {
      datalayer.battery.settings.max_remote_set_discharge_dA = doc["max_discharge"];
      datalayer.battery.settings.remote_settings_limit_discharge = true;
    } else {
      datalayer.battery.settings.max_remote_set_discharge_dA = 0;
      datalayer.battery.settings.remote_settings_limit_discharge = false;
    }

    if (doc["timeout"].is<int>()) {
      datalayer.battery.settings.remote_set_timeout = doc["timeout"].as<int>() * 1000;
    } else {
      datalayer.battery.settings.remote_set_timeout = 30000;
    }

    datalayer.battery.settings.remote_set_timestamp = millis();

    free(data_str);
    */
  }

  free(topic);
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
      clear_event(EVENT_MQTT_DISCONNECT);
      set_event(EVENT_MQTT_CONNECT, 0);

      publish_buttons_discovery();
      subscribe();
      logging.println("MQTT connected");
      break;
    case MQTT_EVENT_DISCONNECTED:
      set_event(EVENT_MQTT_DISCONNECT, 0);
      logging.println("MQTT disconnected!");
      break;
    case MQTT_EVENT_DATA:
      mqtt_message_received(event->topic, event->topic_len, event->data, event->data_len);
      break;
    case MQTT_EVENT_ERROR:
      logging.println("MQTT_ERROR");
      logging.print("reported from esp-tls");
      logging.println(event->error_handle->esp_tls_last_esp_err);
      logging.print("reported from tls stack");
      logging.println(event->error_handle->esp_tls_stack_err);
      logging.print("captured as transport's socket errno");
      logging.println(strerror(event->error_handle->esp_transport_sock_errno));
      break;
    case MQTT_EVENT_SUBSCRIBED:
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      break;
    case MQTT_EVENT_PUBLISHED:
      break;
    case MQTT_EVENT_BEFORE_CONNECT:
      break;
    case MQTT_EVENT_DELETED:
      break;
    case MQTT_USER_EVENT:
      break;
    case MQTT_EVENT_ANY:
      break;
  }
}

bool init_mqtt(void) {

  if (battery == nullptr) {
    logging.println("ERROR: No battery selected. Aborting MQTT initialization");
    return false;
  }

  if (ha_autodiscovery_enabled) {
    //create_battery_sensor_configs();
    //create_global_sensor_configs();
  }

  snprintf(topic_name, sizeof(topic_name), "%s", WiFi.getHostname());

  // default_entity_id_prefix = hostname + "_";
  // device_name = hostname;
  // device_id = hostname;

  //String clientId = String("BatteryEmulatorClient-") + WiFi.getHostname();
  char client_id[128];
  snprintf(client_id, sizeof(client_id), "BatteryEmulatorClient-%s", topic_name);

  char lwt_topic[128];
  snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", topic_name);

  mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  mqtt_cfg.broker.address.hostname = mqtt_server.c_str();
  mqtt_cfg.broker.address.port = mqtt_port;
  mqtt_cfg.credentials.client_id = client_id;
  mqtt_cfg.credentials.username = mqtt_user.c_str();
  mqtt_cfg.credentials.authentication.password = mqtt_password.c_str();
  mqtt_cfg.session.last_will.topic = lwt_topic;
  mqtt_cfg.session.last_will.qos = 1;
  mqtt_cfg.session.last_will.retain = true;
  mqtt_cfg.session.last_will.msg = "offline";
  mqtt_cfg.session.last_will.msg_len = strlen(mqtt_cfg.session.last_will.msg);
  mqtt_cfg.network.timeout_ms = mqtt_timeout_ms;
  client = esp_mqtt_client_init(&mqtt_cfg);

  if (client == nullptr) {
    return false;
  }

  if (esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, client) != ESP_OK) {
    return false;
  }

  return true;
}

void mqtt_client_loop(void) {
  // Only attempt to publish/reconnect MQTT if Wi-Fi is connected and checkTimmer is elapsed
  if (check_global_timer.elapsed() && WiFi.status() == WL_CONNECTED) {

    if (client_started == false) {
      // Configure timer with the loaded interval on first use
      publish_global_timer = MyTimer(mqtt_publish_interval_ms);
      esp_mqtt_client_start(client);
      client_started = true;
      logging.println("MQTT initialized");
      return;
    }

    // Skip publishing if OTA update is in progress to avoid interference
    if (publish_global_timer.elapsed() && !ota_active) {
      publish_values();
    }
  }
}
