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
    neopixelWrite(RGB_BUILTIN, 0, 0, 64); // Solid Blue
  } else if (currentState == STATE_SAVING) {
    neopixelWrite(RGB_BUILTIN, 64, 32, 0); // Solid Orange
  } else {
    // 1-second interval blinker
    if (millis() - lastLedTime > 1000) {
      lastLedTime = millis();
      ledIsOn = !ledIsOn;
      if (ledIsOn) {
        if (currentState == STATE_CONNECTED) neopixelWrite(RGB_BUILTIN, 0, 64, 0); // Green
        else neopixelWrite(RGB_BUILTIN, 64, 0, 0); // Red
      } else {
        neopixelWrite(RGB_BUILTIN, 0, 0, 0); // Off
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
  usbMouse.move(dx, dy);
  if (Keyboard.isConnected()) Mouse.move(dx, dy, 0);
}

void routeMouseScroll(int dz) {
  // Move the scroll wheel (3rd parameter) on both drivers
  usbMouse.move(0, 0, dz);
  if (Keyboard.isConnected()) Mouse.move(0, 0, dz);
}

void routeMouseClick(char btn) {
  if (btn == 'R') {
    usbMouse.click(MOUSE_RIGHT);
    if (Keyboard.isConnected()) Mouse.click(MOUSE_RIGHT);
  } else {
    usbMouse.click(MOUSE_LEFT);
    if (Keyboard.isConnected()) Mouse.click(MOUSE_LEFT);
  }
}

void routeKeyboardWrite(uint8_t c) {
  usbKeyboard.write(c);
  if (Keyboard.isConnected()) Keyboard.write(c);
}

void routeKeyboardReleaseAll() {
  usbKeyboard.releaseAll();
  if (Keyboard.isConnected()) Keyboard.releaseAll();
}

void typeStringSlowly(const String& text, unsigned int delayMs) {
  for (size_t i = 0; i < text.length(); i++) {
    routeKeyboardWrite(text[i]);
    if (!setupMode) webSocket.loop(); 
    updateLED(); 
    delay(delayMs);
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
      typeStringSlowly(text, 15);
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
  USB.begin(); 
  Keyboard.begin(); 
  Mouse.begin();    

  Serial.println("[NVS] Loading saved credentials...");
  preferences.begin("remote", false); 
  
  bool forceSetup = preferences.getBool("force_setup", false);
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
// Loop
// --------------------------------------------------------
void loop() {
  updateLED(); 

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
      if (millis() - lastWifiCheck > 5000) {
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
extern "C" void app_main() {
    initArduino();   // sets up Arduino core internals
    setup();         // your existing setup()
    while (true) {
        loop();       // your existing loop()
        vTaskDelay(1); 
    }
}