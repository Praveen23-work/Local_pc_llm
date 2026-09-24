#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebSocketsClient.h>

// 1. Include BLE FIRST with the struct renaming trick
#define KeyReport BleKeyReport 
#include <BleCombo.h>
#undef KeyReport

// 2. Include Native USB SECOND
#include "USB.h"
#include "USBHIDMouse.h"
#include "USBHIDKeyboard.h"

// --- Global Objects ---
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;
WebSocketsClient webSocket;

USBHIDMouse usbMouse;
USBHIDKeyboard usbKeyboard;
volatile bool nativeUsbActive = false;

// --- accumulator state (file scope) ---
static int pendingDx = 0, pendingDy = 0, pendingDz = 0;
static unsigned long lastFlushMs = 0;
unsigned long flushIntervalMs = 8;   // now mutable, loaded from NVS below
unsigned int typeDelayMs = 15;
unsigned long wifiReconnectMs = 5000;
unsigned long ledBlinkMs = 1000;
uint8_t ledR = 0, ledG = 64, ledB = 0;  // default green (CONNECTED-state blink color)


// --- State Variables ---
bool setupMode = false;
String savedSSID = "";
String savedPass = "";
String savedWsHost = "";
String savedWsKey = "";
bool bleWasConnected = false;

// --- Physical Button Override ---
const int BOOT_BUTTON = 0; 
unsigned long buttonPressTime = 0;
bool buttonIsPressed = false;

// --- RGB LED State Machine ---
enum SystemState { STATE_SETUP, STATE_DISCONNECTED, STATE_CONNECTED, STATE_SAVING };
SystemState currentState = STATE_DISCONNECTED;
unsigned long lastLedTime = 0;
bool ledIsOn = false;

void updateLED() {
  if (currentState == STATE_SETUP) {
    neopixelWrite(RGB_BUILTIN, 0, 0, 64);
  } else if (currentState == STATE_SAVING) {
    neopixelWrite(RGB_BUILTIN, 64, 32, 0);
  } else {
    if (millis() - lastLedTime > ledBlinkMs) {
      lastLedTime = millis();
      ledIsOn = !ledIsOn;
      if (ledIsOn) {
        if (currentState == STATE_CONNECTED) neopixelWrite(RGB_BUILTIN, ledR, ledG, ledB);
        else neopixelWrite(RGB_BUILTIN, 64, 0, 0);
      } else {
        neopixelWrite(RGB_BUILTIN, 0, 0, 0);
      }
    }
  }
}

// --- Unified Setup Page HTML (No required tags) ---
const char* SETUP_HTML = R"HTMLPAGE(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Remote Setup</title>
  <style>
    body { font-family: sans-serif; background: #111; color: #eee; padding: 20px; }
    .container { max-width: 400px; margin: auto; background: #222; padding: 20px; border-radius: 10px; }
    h2 { text-align: center; color: #4CAF50; }
    p { font-size: 12px; color: #888; text-align: center; }
    label { font-size: 14px; color: #aaa; margin-top: 10px; display: block; }
    input[type="text"], input[type="password"] { width: 100%; padding: 10px; margin: 5px 0 15px 0; border: none; border-radius: 5px; background: #333; color: white; box-sizing: border-box;}
    button { width: 100%; padding: 12px; background: #4CAF50; color: white; border: none; border-radius: 5px; font-size: 16px; cursor: pointer; }
    button:active { background: #45a049; }
  </style>
</head>
<body>
  <div class="container">
    <h2>Device Setup</h2>
    <p>Leave a field blank to keep its current value.</p>
    <form action="/save" method="POST">
      <label>Wi-Fi Name (SSID)</label>
      <input type="text" name="ssid" value="{SSID}">
      
      <label>Wi-Fi Password</label>
      <input type="password" name="pass" placeholder="••••••••">
      
      <label>Cloudflare URL</label>
      <input type="text" name="wshost" value="{HOST}">
      
      <label>Secret Key</label>
      <input type="password" name="wskey" placeholder="••••••••">
      
      <button type="submit">Save & Reboot</button>
    </form>
  </div>
</body>
</html>
)HTMLPAGE";

// --------------------------------------------------------
// Dual-Routing HID Functions 
// --------------------------------------------------------


void routeMouseMove(int dx, int dy) {
  pendingDx = constrain(pendingDx + dx, -127, 127);
  pendingDy = constrain(pendingDy + dy, -127, 127);
}

void routeMouseScroll(int dz) {
  pendingDz = constrain(pendingDz + dz, -127, 127);
}

void flushMouseIfDue() {
  if (millis() - lastFlushMs < flushIntervalMs) return;
  lastFlushMs = millis();
  if (pendingDx || pendingDy || pendingDz) {
    if (nativeUsbActive) {
      usbMouse.move(pendingDx, pendingDy, pendingDz);
    } else if (Keyboard.isConnected()) {
      Mouse.move(pendingDx, pendingDy, pendingDz);
    }
    pendingDx = pendingDy = pendingDz = 0;
  }
}

void routeMouseClick(char btn) {
  uint8_t b = (btn == 'R') ? MOUSE_RIGHT : MOUSE_LEFT;
  if (nativeUsbActive) usbMouse.click(b);
  else if (Keyboard.isConnected()) Mouse.click(b);
}

void routeKeyboardWrite(uint8_t c) {
  if (nativeUsbActive) usbKeyboard.write(c);
  else if (Keyboard.isConnected()) Keyboard.write(c);
}

void routeKeyboardReleaseAll() {
  if (nativeUsbActive) usbKeyboard.releaseAll();
  else if (Keyboard.isConnected()) Keyboard.releaseAll();
}

void typeStringSlowly(const String& text, unsigned int delayMs) {
  for (size_t i = 0; i < text.length(); i++) {
    routeKeyboardWrite(text[i]);
    if (!setupMode) webSocket.loop(); 
    updateLED(); 
    delay(delayMs);
  }
}

void onUsbEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == ARDUINO_USB_STARTED_EVENT || event_id == ARDUINO_USB_RESUME_EVENT) {
    nativeUsbActive = true;
  } else if (event_id == ARDUINO_USB_STOPPED_EVENT || event_id == ARDUINO_USB_SUSPEND_EVENT) {
    nativeUsbActive = false;
  }
}

// --------------------------------------------------------
// WebSocket Event Handler
// --------------------------------------------------------
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.println("[WEBSOCKET] Connected to Cloudflare Edge!");
    currentState = STATE_CONNECTED;
  } 
  else if (type == WStype_DISCONNECTED) {
    Serial.println("[WEBSOCKET] Disconnected from Cloudflare.");
    currentState = STATE_DISCONNECTED;
  }
  else if (type == WStype_TEXT) {
    String data = (char*)payload;
    if (data.startsWith("M:")) {
      int firstColon = data.indexOf(':');
      int secondColon = data.indexOf(':', firstColon + 1);
      if (secondColon != -1) {
        int dx = data.substring(firstColon + 1, secondColon).toInt();
        int dy = data.substring(secondColon + 1).toInt();
        routeMouseMove(constrain(dx, -127, 127), constrain(dy, -127, 127));
      }
    } 
    else if (data.startsWith("S:")) {
      int dz = data.substring(2).toInt();
      routeMouseScroll(constrain(dz, -127, 127));
    }
    else if (data.startsWith("C:")) routeMouseClick(data.charAt(2));
    else if (data.startsWith("T:")) {
      String text = data.substring(2);
      typeStringSlowly(text, typeDelayMs);
      routeKeyboardReleaseAll();
    } 
    else if (data == "B:") routeKeyboardWrite(8); // Backspace
    else if (data == "E:") routeKeyboardWrite('\n'); // Enter key
  }
}

// --------------------------------------------------------
// Captive Portal Handlers
// --------------------------------------------------------
void handleRoot() {
  Serial.println("[PORTAL] Client connected to setup page.");
  String page = SETUP_HTML;
  page.replace("{SSID}", savedSSID);
  page.replace("{HOST}", savedWsHost);
  server.send(200, "text/html", page);
}

void handleSave() {
  Serial.println("[PORTAL] New data received. Processing...");
  currentState = STATE_SAVING;
  updateLED();

  String newSSID = server.arg("ssid");
  String newPass = server.arg("pass");
  String newHost = server.arg("wshost");
  String newKey = server.arg("wskey");

  if (newSSID != "") savedSSID = newSSID;
  if (newPass != "") savedPass = newPass;
  if (newKey != "") savedWsKey = newKey;
  
  if (newHost != "") {
    newHost.replace("https://", "");
    newHost.replace("wss://", "");
    if (newHost.indexOf('/') != -1) newHost = newHost.substring(0, newHost.indexOf('/'));
    savedWsHost = newHost;
  }

  Serial.println("[NVS] Saving credentials to memory...");
  preferences.begin("remote", false);
  preferences.putString("ssid", savedSSID);
  preferences.putString("pass", savedPass);
  preferences.putString("host", savedWsHost);
  preferences.putString("key", savedWsKey);
  preferences.end();

  Serial.println("[PORTAL] Save complete. Rebooting...");
  server.send(200, "text/html", "<html><body style='background:#111; color:#4CAF50; font-family:sans-serif; text-align:center; padding-top:50px;'><h2>Credentials Saved!</h2><p>Device is rebooting...</p></body></html>");
  
  delay(1500);
  ESP.restart();
}

// --------------------------------------------------------
// Setup
// --------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  
  Serial.println("\n[BOOT] Initializing hardware...");
  neopixelWrite(RGB_BUILTIN, 0, 0, 0); 

  usbMouse.begin();
  usbKeyboard.begin();
  USB.PID(0x1001);
  USB.VID(0x303A);
  usbMouse.begin();
  usbKeyboard.begin();
  USB.onEvent(onUsbEvent);
  USB.begin(); 
  Serial.println("[USB] USB.begin() called");
  Keyboard.begin(); 
  Mouse.begin();    

  Serial.println("[NVS] Loading saved credentials...");
  preferences.begin("remote", false); 
  
  bool forceSetup = preferences.getBool("force_setup", false);
  flushIntervalMs = preferences.getULong("flush_interval", 8);
  typeDelayMs = preferences.getUInt("type_delay", 15);
  wifiReconnectMs = preferences.getULong("wifi_reconnect", 5000);
  ledBlinkMs = preferences.getULong("led_ms", 1000);
  ledR = preferences.getUChar("led_r", 0);
  ledG = preferences.getUChar("led_g", 64);
  ledB = preferences.getUChar("led_b", 0);
  if (forceSetup) {
    Serial.println("[BOOT] Force setup flag detected. Clearing flag for next reset.");
    preferences.putBool("force_setup", false);
  }

  savedSSID = preferences.getString("ssid", "");
  savedPass = preferences.getString("pass", "");
  savedWsHost = preferences.getString("host", "");
  savedWsKey = preferences.getString("key", "");
  preferences.end();

  if (forceSetup || savedSSID == "" || savedWsHost == "") {
    setupMode = true;
    currentState = STATE_SETUP;
    updateLED();
    
    Serial.println("[WIFI] Starting Access Point: ESP32-Setup");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-Setup");
    dnsServer.start(53, "*", WiFi.softAPIP());
    
    server.on("/", handleRoot);
    server.on("/generate_204", handleRoot); 
    server.on("/hotspot-detect.html", handleRoot); 
    server.on("/fwlink", handleRoot); 
    server.on("/connecttest.txt", handleRoot); 
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleRoot); 
    server.begin();
    
    Serial.println("[SETUP] Captive portal active. Waiting for user input.");
  } 
  else {
    setupMode = false;
    currentState = STATE_DISCONNECTED;
    updateLED();
    
    Serial.print("[WIFI] Connecting to SSID: ");
    Serial.println(savedSSID);
    
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { 
      updateLED();
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());
      Serial.println("[WEBSOCKET] Connecting to Cloudflare...");
      String wsPath = String("/ws?key=") + savedWsKey;
      webSocket.beginSSL(savedWsHost.c_str(), 443, wsPath.c_str());
      webSocket.onEvent(webSocketEvent);
      webSocket.setReconnectInterval(5000);
      webSocket.enableHeartbeat(25000, 3000, 2);
    } else {
      Serial.println("\n[WIFI] Failed to connect. Will retry in background.");
    }
  }
}

// --------------------------------------------------------
// Serial Command Listener (prompt-driven variable updates)
// --------------------------------------------------------
static String serialBuf = "";

bool isKnownStringKey(const String& k) {
  return k == "ssid" || k == "pass" || k == "host" || k == "key";
}

bool isKnownIntKey(const String& k) {
  return k == "flush_ms" || k == "type_ms" || k == "recon_ms"
      || k == "led_ms" || k == "led_r" || k == "led_g" || k == "led_b";
}

void applyIntKey(const String& k, long v) {
  preferences.begin("remote", false);
  preferences.putULong(k.c_str(), v);
  preferences.end();
  if (k == "flush_ms") flushIntervalMs = v;
  else if (k == "type_ms") typeDelayMs = (unsigned int)v;
  else if (k == "recon_ms") wifiReconnectMs = v;
  else if (k == "led_ms") ledBlinkMs = v;
  else if (k == "led_r") ledR = (uint8_t)v;
  else if (k == "led_g") ledG = (uint8_t)v;
  else if (k == "led_b") ledB = (uint8_t)v;
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      serialBuf.trim();
      if (serialBuf.startsWith("SET ")) {
        String rest = serialBuf.substring(4);
        int sp = rest.indexOf(' ');
        if (sp != -1) {
          String key = rest.substring(0, sp);
          String val = rest.substring(sp + 1);

          if (isKnownStringKey(key)) {
            preferences.begin("remote", false);
            preferences.putString(key.c_str(), val);
            preferences.end();
            Serial.println("OK " + key + " saved (reboot required to apply)");
          } else if (isKnownIntKey(key)) {
            applyIntKey(key, val.toInt());
            Serial.println("OK " + key + "=" + val + " applied immediately");
          } else {
            Serial.println("ERR unknown key: " + key);
          }
        } else {
          Serial.println("ERR malformed command");
        }
      } else {
        Serial.println("ERR expected 'SET <key> <value>'");
      }
      serialBuf = "";
    } else if (c != '\r') {
      serialBuf += c;
    }
  }
}


// --------------------------------------------------------
// Loop
// --------------------------------------------------------
void loop() {
  updateLED(); 
  flushMouseIfDue();
  handleSerialCommands();

  if (digitalRead(BOOT_BUTTON) == LOW) {
    if (!buttonIsPressed) {
      buttonPressTime = millis();
      buttonIsPressed = true;
    } else if (millis() - buttonPressTime > 10000) {
      currentState = STATE_SAVING; 
      updateLED();
      Serial.println("\n[BUTTON] 10-second hold detected. Setting force_setup flag...");
      preferences.begin("remote", false);
      preferences.putBool("force_setup", true);
      preferences.end();
      Serial.println("[BUTTON] Rebooting into Setup Mode...");
      delay(500);
      ESP.restart();
    }
  } else {
    buttonIsPressed = false;
  }

  if (setupMode) {
    dnsServer.processNextRequest();
    server.handleClient();
  } 
  else {
    static unsigned long lastWifiCheck = 0;
    if (WiFi.status() != WL_CONNECTED) {
      currentState = STATE_DISCONNECTED;
      if (millis() - lastWifiCheck > wifiReconnectMs) {
        WiFi.reconnect();
        lastWifiCheck = millis();
      }
    } else {
      webSocket.loop();
    }

    if (Keyboard.isConnected()) {
      if (!bleWasConnected) {
        Serial.println("[BLE] Connected to PC.");
        bleWasConnected = true; 
      }
    } else if (bleWasConnected) {
      Serial.println("[BLE] Disconnected! Soft-resetting to clear stack...");
      delay(500);
      ESP.restart(); 
    }
  }
}
static void loopTask(void *pvParameters) {
    setup();
    for (;;) {
        loop();
        vTaskDelay(1);
    }
}

extern "C" void app_main() {
    initArduino();
    xTaskCreatePinnedToCore(loopTask, "loopTask", 8192, NULL, 1, NULL, 1);  // Core 1, matches Arduino IDE
}