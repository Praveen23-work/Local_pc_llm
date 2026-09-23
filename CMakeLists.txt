cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS "C:/prvn/esp32-arduino-lib-builder/components/arduino_tinyusb")
add_compile_definitions(ARDUINO_USB_MODE=1 ARDUINO_USB_CDC_ON_BOOT=1)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Local_pc_llm)