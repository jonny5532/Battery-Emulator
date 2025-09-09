cmake_minimum_required(VERSION 3.28)
enable_testing()

# set the project name
project(UnitTests)

# Enable ExternalProject CMake module
include(ExternalProject)
include(GoogleTest)

# specify the C++ standard
# At least 17 is required by Gtest
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Download and install GoogleTest
ExternalProject_Add(
    gtest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    PREFIX ${CMAKE_CURRENT_BINARY_DIR}/gtest
    # Disable install step
    INSTALL_COMMAND ""
)

# Get GTest source and binary directories from CMake project
ExternalProject_Get_Property(gtest source_dir binary_dir)

# Create a libgtest target to be used as a dependency by test programs
add_library(libgtest IMPORTED STATIC GLOBAL)
add_dependencies(libgtest gtest)
add_definitions(-DUNIT_TEST)

# Set libgtest properties
if(WIN32)
    set_target_properties(libgtest PROPERTIES
        "IMPORTED_LOCATION" "${binary_dir}/lib/Debug/gtest.lib"
        "IMPORTED_LINK_INTERFACE_LIBRARIES" "${CMAKE_THREAD_LIBS_INIT}")
else()
    set_target_properties(libgtest PROPERTIES
        "IMPORTED_LOCATION" "${binary_dir}/lib/libgtest.a"
        "IMPORTED_LINK_INTERFACE_LIBRARIES" "${CMAKE_THREAD_LIBS_INIT}")
endif(WIN32)

# Create a libgmock target to be used as a dependency by test programs
add_library(libgmock IMPORTED STATIC GLOBAL)
add_dependencies(libgmock gtest)

# Set libgmock properties
if(WIN32)
    set_target_properties(libgmock PROPERTIES
        "IMPORTED_LOCATION" "${binary_dir}/lib/Debug/gmock.lib"
        "IMPORTED_LINK_INTERFACE_LIBRARIES" "${CMAKE_THREAD_LIBS_INIT}")
else()
    set_target_properties(libgmock PROPERTIES
        "IMPORTED_LOCATION" "${binary_dir}/lib/libgmock.a"
        "IMPORTED_LINK_INTERFACE_LIBRARIES" "${CMAKE_THREAD_LIBS_INIT}")
endif(WIN32)

# I couldn't make it work with INTERFACE_INCLUDE_DIRECTORIES
include_directories("${source_dir}/googletest/include"
                    "${source_dir}/googlemock/include")

include_directories(emul)

# For eModBus
add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NO_POSIX_ERROR_CODES ESP32 ARDUINO HW_LILYGO COMMON_IMAGE ASYNCTCP_ALLOW_ANY_ERRNOS_VALUES)

# add the executable
add_executable(tests 
    tests.cpp 
    #main_tests.cpp 
    safety_tests.cpp 
    battery/NissanLeafTest.cpp 
    battery/still_alive_tests.cpp
    can_log_based/canlog_safety_tests.cpp
    utils/utils.cpp
    ../Software/src/communication/can/obd.cpp
    ../Software/src/communication/contactorcontrol/comm_contactorcontrol.cpp
    ../Software/src/communication/rs485/comm_rs485.cpp
    ../Software/src/communication/precharge_control/precharge_control.cpp
    ../Software/src/communication/equipmentstopbutton/comm_equipmentstopbutton.cpp
    ../Software/src/communication/nvm/comm_nvm.cpp
    ../Software/src/communication/rs485/comm_rs485.cpp
    ../Software/src/devboard/wifi/wifi.cpp
    ../Software/src/devboard/mqtt/mqtt.cpp
    ../Software/src/devboard/safety/safety.cpp
    ../Software/src/devboard/hal/hal.cpp
    ../Software/src/devboard/utils/events.cpp
    ../Software/src/devboard/utils/logging.cpp
    ../Software/src/devboard/utils/debounce_button.cpp
    ../Software/src/datalayer/datalayer.cpp
    ../Software/src/datalayer/datalayer_extended.cpp
    ../Software/src/lib/eModbus-eModbus/Logging.cpp
    ../Software/src/lib/eModbus-eModbus/ModbusMessage.cpp
    ../Software/src/lib/eModbus-eModbus/ModbusServer.cpp
    ../Software/src/lib/eModbus-eModbus/ModbusServerRTU.cpp
    ../Software/src/lib/eModbus-eModbus/ModbusTypeDefs.cpp
    ../Software/src/lib/eModbus-eModbus/RTUutils.cpp
    ../Software/src/lib/ESP32Async-ESPAsyncWebServer/src/AsyncWebHeader.cpp
    ../Software/src/lib/ESP32Async-ESPAsyncWebServer/src/AsyncWebServerRequest.cpp
    ../Software/src/lib/ESP32Async-ESPAsyncWebServer/src/WebServer.cpp
    ../Software/src/lib/ESP32Async-ESPAsyncWebServer/src/WebRequest.cpp
    ../Software/src/lib/ESP32Async-ESPAsyncWebServer/src/WebHandlers.cpp
    ../Software/src/lib/ESP32Async-ESPAsyncWebServer/src/WebResponses.cpp
    ../Software/src/lib/ESP32Async-ESPAsyncWebServer/src/WebAuthentication.cpp
    ../Software/src/lib/ESP32Async-ESPAsyncWebServer/src/Middleware.cpp
    ../Software/src/lib/mathieucarbou-AsyncTCPSock/src/AsyncTCP.cpp
    ../Software/src/lib/adafruit-Adafruit_NeoPixel/Adafruit_NeoPixel.cpp
    ../Software/src/lib/ayushsharma82-ElegantOTA/src/elop.cpp
    ../Software/src/lib/ayushsharma82-ElegantOTA/src/ElegantOTA.cpp
    ../Software/src/battery/BATTERIES.cpp
    ../Software/src/battery/Battery.cpp
    ../Software/src/battery/BMW-I3-BATTERY.cpp
    ../Software/src/battery/BMW-I3-HTML.cpp
    ../Software/src/battery/BMW-IX-BATTERY.cpp
    ../Software/src/battery/BMW-IX-HTML.cpp
    ../Software/src/battery/BMW-PHEV-BATTERY.cpp
    ../Software/src/battery/BMW-SBOX.cpp
    ../Software/src/battery/BOLT-AMPERA-BATTERY.cpp
    ../Software/src/battery/BYD-ATTO-3-BATTERY.cpp
    ../Software/src/battery/CanBattery.cpp
    ../Software/src/battery/CELLPOWER-BMS.cpp
    ../Software/src/battery/CHADEMO-BATTERY.cpp
    ../Software/src/battery/CHADEMO-SHUNTS.cpp
    ../Software/src/battery/CMFA-EV-BATTERY.cpp
    ../Software/src/battery/DALY-BMS.cpp
    ../Software/src/battery/ECMP-BATTERY.cpp
    ../Software/src/battery/FOXESS-BATTERY.cpp
    ../Software/src/battery/GEELY-GEOMETRY-C-BATTERY.cpp
    ../Software/src/battery/HYUNDAI-IONIQ-28-BATTERY-HTML.cpp
    ../Software/src/battery/HYUNDAI-IONIQ-28-BATTERY.cpp
    ../Software/src/battery/IMIEV-CZERO-ION-BATTERY.cpp
    ../Software/src/battery/JAGUAR-IPACE-BATTERY.cpp
    ../Software/src/battery/KIA-E-GMP-BATTERY.cpp
    ../Software/src/battery/KIA-E-GMP-HTML.cpp
    ../Software/src/battery/KIA-64FD-BATTERY.cpp
    ../Software/src/battery/KIA-HYUNDAI-64-BATTERY.cpp
    ../Software/src/battery/KIA-HYUNDAI-HYBRID-BATTERY.cpp
    ../Software/src/battery/MEB-BATTERY.cpp
    ../Software/src/battery/MG-5-BATTERY.cpp
    ../Software/src/battery/MG-HS-PHEV-BATTERY.cpp
    ../Software/src/battery/NISSAN-LEAF-BATTERY.cpp
    ../Software/src/battery/ORION-BMS.cpp
    ../Software/src/battery/PYLON-BATTERY.cpp
    ../Software/src/battery/RANGE-ROVER-PHEV-BATTERY.cpp
    ../Software/src/battery/RELION-LV-BATTERY.cpp
    ../Software/src/battery/RENAULT-KANGOO-BATTERY.cpp
    ../Software/src/battery/RENAULT-TWIZY.cpp
    ../Software/src/battery/RENAULT-ZOE-GEN1-BATTERY.cpp
    ../Software/src/battery/RENAULT-ZOE-GEN2-BATTERY.cpp
    ../Software/src/battery/RJXZS-BMS.cpp
    ../Software/src/battery/SAMSUNG-SDI-LV-BATTERY.cpp
    ../Software/src/battery/SANTA-FE-PHEV-BATTERY.cpp
    ../Software/src/battery/Shunts.cpp
    ../Software/src/battery/SIMPBMS-BATTERY.cpp
    ../Software/src/battery/SONO-BATTERY.cpp
    ../Software/src/battery/TESLA-BATTERY.cpp
    ../Software/src/battery/TEST-FAKE-BATTERY.cpp
    ../Software/src/battery/VOLVO-SPA-BATTERY.cpp
    ../Software/src/battery/VOLVO-SPA-HYBRID-BATTERY.cpp
    ../Software/src/inverter/AFORE-CAN.cpp
    ../Software/src/inverter/BYD-CAN.cpp
    ../Software/src/inverter/BYD-MODBUS.cpp
    ../Software/src/inverter/FERROAMP-CAN.cpp
    ../Software/src/inverter/FOXESS-CAN.cpp
    ../Software/src/inverter/GROWATT-HV-CAN.cpp
    ../Software/src/inverter/GROWATT-LV-CAN.cpp
    ../Software/src/inverter/GROWATT-WIT-CAN.cpp
    ../Software/src/inverter/INVERTERS.cpp
    ../Software/src/inverter/KOSTAL-RS485.cpp
    ../Software/src/inverter/ModbusInverterProtocol.cpp
    ../Software/src/inverter/PYLON-CAN.cpp
    ../Software/src/inverter/PYLON-LV-CAN.cpp
    ../Software/src/inverter/SCHNEIDER-CAN.cpp
    ../Software/src/inverter/SMA-BYD-H-CAN.cpp
    ../Software/src/inverter/SMA-BYD-HVS-CAN.cpp
    ../Software/src/inverter/SMA-LV-CAN.cpp
    ../Software/src/inverter/SMA-TRIPOWER-CAN.cpp
    ../Software/src/inverter/SOFAR-CAN.cpp
    ../Software/src/inverter/SOL-ARK-LV-CAN.cpp
    ../Software/src/inverter/SOLAX-CAN.cpp
    ../Software/src/inverter/SOLXPOW-CAN.cpp
    ../Software/src/inverter/SUNGROW-CAN.cpp
    ../Software/src/charger/CHARGERS.cpp
    ../Software/src/charger/CHEVY-VOLT-CHARGER.cpp
    ../Software/src/charger/NISSAN-LEAF-CHARGER.cpp
    ../Software/src/devboard/webserver/advanced_battery_html.cpp
    ../Software/src/devboard/webserver/can_logging_html.cpp
    ../Software/src/devboard/webserver/can_replay_html.cpp
    ../Software/src/devboard/webserver/cellmonitor_html.cpp
    ../Software/src/devboard/webserver/debug_logging_html.cpp
    ../Software/src/devboard/webserver/events_html.cpp
    ../Software/src/devboard/webserver/index_html.cpp
    ../Software/src/devboard/webserver/settings_html.cpp
    ../Software/src/devboard/webserver/webserver.cpp
    ../Software/src/devboard/utils/types.cpp
    ../Software/src/devboard/utils/timer.cpp
    ../Software/src/devboard/utils/led_handler.cpp
    ../Software/Software.cpp
    emul/can.cpp
    emul/time.cpp
    emul/serial.cpp
    emul/WString.cpp
    emul/cbuf.cpp
    emul/WiFi.cpp
    emul/Arduino.cpp
    emul/freertos/FreeRTOS.cpp
    emul/Update.cpp
    emul/MD5Builder.cpp
    emul/HardwareSerial.cpp
    emul/Preferences.cpp
    emul/print.cpp
    emul/mqtt.cpp
    emul/SD_MMC.cpp
    emul/sdcard.cpp
    emul/freertos/task.cpp
    emul/freertos/semphr.cpp
    emul/freertos/ringbuf.cpp
    emul/lwip/sockets.cpp
    emul/lwip/dns.cpp
    emul/libb64/cencode.cpp
    emul/hacks.cpp
    )

target_link_libraries(tests
    libgtest
    libgmock
)

gtest_discover_tests(tests)
