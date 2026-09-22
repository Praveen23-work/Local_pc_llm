idf_component_register(SRCS "main.cpp"
                        INCLUDE_DIRS "."
                        REQUIRES arduino-esp32 WebSockets ESP32_BLE_Combo_Keyboard_Mouse arduino_tinyusb)