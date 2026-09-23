idf_component_register(SRCS "main.cpp"
                        INCLUDE_DIRS "."
                        REQUIRES arduino-esp32 WebSockets ESP32_BLE_Combo_Keyboard_Mouse arduino_tinyusb)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ARDUINO_USB_MODE=1
    ARDUINO_USB_CDC_ON_BOOT=1
)