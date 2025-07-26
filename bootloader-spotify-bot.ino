#include <WiFiManager.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HTTPUpdate.h>
#include <HTTPClient.h>

// instantiate the OTA updater
HTTPUpdate httpUpdater;

// OLED setup
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// font metrics for size=1
const int CHAR_WIDTH   = 6;
const int LINE_HEIGHT  = 8;

// hardware pins
const int LED_PIN      = 2;
#define BUTTON_PIN      0

// Spotify / OAuth servers
const char* SPOTIFY_HOST = "api.spotify.com";
const int   SPOTIFY_PORT = 443;
const char* OAUTH_SERVER = "oauth.thespacewaffles.com";
const int   OAUTH_PORT   = 443;

// OTA metadata URL
const char* OTA_JSON_URL = "https://oauth.thespacewaffles.com/updates/ota.json";

const char* LOCAL_FIRMWARE_VERSION = "0.0.0";

// Current firmware version for self-check after OTA

// user greeting
#define USER_NAME "Sam"

// button state
bool deviceOn        = false;
bool lastButtonState = HIGH;

// tokens & HTML
String DEVICE_ID;
String ACCESS_TOKEN  = "";
String REFRESH_TOKEN = "";
String latestHtml    = "";

// Wi‑Fi state
enum WifiState { CONNECTING, CONNECTED, DISCONNECTED };
WifiState wifiState = CONNECTING;

// timing
unsigned long lastBlink     = 0;
unsigned long lastPollTime  = 0;
int           blinkInterval = 100;
const unsigned long POLL_INTERVAL = 5000;

// web server & prefs
WebServer server(80);
Preferences prefs;

// ————————— Helpers —————————

void debugWifiState() {
  Serial.print("💡 WiFi State: ");
  switch (wifiState) {
    case CONNECTING:   Serial.println("CONNECTING");   break;
    case CONNECTED:    Serial.println("CONNECTED");    break;
    case DISCONNECTED: Serial.println("DISCONNECTED"); break;
  }
}

void updateBlinkInterval() {
  switch (wifiState) {
    case CONNECTING:   blinkInterval = 100;  break;
    case CONNECTED:    blinkInterval = 500;  break;
    case DISCONNECTED: blinkInterval = 1000; break;
  }
  debugWifiState();
  Serial.print("💡 blinkInterval set to ");
  Serial.println(blinkInterval);
}

void displayWrappedText(int x, int &y, const String &text) {
  int maxChars = SCREEN_WIDTH / CHAR_WIDTH;
  String s = text;
  while (s.length()) {
    int len = s.length() > maxChars ? maxChars : s.length();
    int split = s.length() > maxChars ? s.lastIndexOf(' ', len) : len;
    if (split < 0) split = len;
    String line = s.substring(0, split);
    display.setCursor(x, y);
    display.println(line);
    y += LINE_HEIGHT;
    if (split < (int)s.length() && s.charAt(split) == ' ') split++;
    s = s.substring(split);
  }
}

// ————————— OAuth helpers —————————

void unlinkFromServer() {
  Serial.println("🔗 unlinkFromServer()");
  WiFiClientSecure c; c.setInsecure();
  if (!c.connect(OAUTH_SERVER, OAUTH_PORT)) {
    Serial.println("❌ unlink: cannot connect");
    return;
  }
  String url = "/logout?device_id=" + DEVICE_ID;
  c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
           url.c_str(), OAUTH_SERVER);
  while (c.connected() && !c.available()) delay(1);
  while (c.available()) c.readString();
  Serial.println("✅ unlinked");
}

bool pollForTokens() {
  Serial.println("🔃 pollForTokens()");
  WiFiClientSecure c; c.setInsecure();
  if (!c.connect(OAUTH_SERVER, OAUTH_PORT)) {
    Serial.println("❌ poll: cannot connect");
    return false;
  }
  String url = "/poll?device_id=" + DEVICE_ID;
  c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
           url.c_str(), OAUTH_SERVER);
  while (c.connected() && !c.available()) delay(1);
  String resp;
  while (c.available()) resp += c.readString();
  Serial.print("📨 poll response: ");
  Serial.println(resp);
  int idx = resp.indexOf("{\"success\":");
  if (idx < 0) return false;
  String j = resp.substring(idx);
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, j)) {
    Serial.println("❌ poll JSON parse error");
    return false;
  }
  if (!doc["success"].as<bool>()) return false;
  JsonObject tok = doc["tokens"].as<JsonObject>();
  ACCESS_TOKEN  = tok["access_token"].as<String>();
  REFRESH_TOKEN = tok["refresh_token"].as<String>();
  Serial.println("✅ tokens received");
  return true;
}

bool refreshAccessToken() {
  Serial.println("🔄 refreshAccessToken()");
  if (REFRESH_TOKEN == "") {
    Serial.println("⚠️ no refresh token");
    return false;
  }
  WiFiClientSecure c; c.setInsecure();
  if (!c.connect(OAUTH_SERVER, OAUTH_PORT)) {
    Serial.println("❌ refresh: cannot connect");
    return false;
  }
  String url = "/token?device_id=" + DEVICE_ID;
  c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
           url.c_str(), OAUTH_SERVER);
  while (c.connected() && !c.available()) delay(1);
  String resp;
  while (c.available()) resp += c.readString();
  Serial.print("📨 refresh response: ");
  Serial.println(resp);
  int idx = resp.indexOf('{');
  if (idx < 0) return false;
  String j = resp.substring(idx);
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, j)) {
    Serial.println("❌ refresh JSON parse error");
    return false;
  }
  if (!doc["success"].as<bool>()) return false;
  JsonObject tok = doc["tokens"].as<JsonObject>();
  ACCESS_TOKEN = tok["access_token"].as<String>();
  if (tok.containsKey("refresh_token"))
    REFRESH_TOKEN = tok["refresh_token"].as<String>();
  Serial.println("✅ token refreshed");
  return true;
}

// ————————— OTA update check —————————

bool checkForOTA() {
  Serial.println("🔍 checkForOTA()");
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(600000);

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Checking OTA...");
  display.display();

  if (!http.begin(client, OTA_JSON_URL)) {
    Serial.println("❌ OTA: begin() failed");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA Error");
    display.display();
    delay(1000);
    return false;
  }

  int code = http.GET();
  Serial.printf("📡 OTA HTTP GET code: %d\n", code);
  if (code != HTTP_CODE_OK) {
    Serial.printf("❌ OTA HTTP error: %s\n", http.errorToString(code).c_str());
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA Error");
    display.display();
    delay(1000);
    http.end();
    return false;
  }

  String payload = http.getString();
  Serial.print("📨 OTA payload: ");
  Serial.println(payload);

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, payload)) {
    Serial.println("❌ OTA JSON parse error");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA Error");
    display.display();
    delay(1000);
    http.end();
    return false;
  }
  http.end();

  String remoteVersion = doc["version"].as<String>();
  String otaUrl        = doc["url"].as<String>();
  Serial.printf("🔍 Remote version: %s\n", remoteVersion.c_str());

  String localVersion = LOCAL_FIRMWARE_VERSION;
  Serial.printf("🔍 Local version: %s\n", localVersion.c_str());

  if (remoteVersion == localVersion) {
    Serial.println("✅ Firmware up to date");
    return false;
  }

  // perform update
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("Ver ");
  display.println(localVersion);
  display.display();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Updating...");
  display.display();

  client.setTimeout(600000);
  httpUpdater.onProgress([](int cur, int total) {
    display.fillRect(0, SCREEN_HEIGHT - 10, SCREEN_WIDTH, 10, SSD1306_BLACK);
    display.drawRect(0, SCREEN_HEIGHT - 10, SCREEN_WIDTH, 10, SSD1306_WHITE);
    int w = map(cur, 0, total, 0, SCREEN_WIDTH);
    display.fillRect(0, SCREEN_HEIGHT - 10, w, 10, SSD1306_WHITE);
    display.display();
  });

  httpUpdater.onEnd([remoteVersion]() {
    Serial.printf("⚙️ Image v%s written, rebooting\n", remoteVersion.c_str());
  });

  // actually perform the OTA update (this will reboot on success)
  t_httpUpdate_return ret = httpUpdater.update(client, otaUrl.c_str());
  if (ret != HTTP_UPDATE_OK) {
    Serial.printf("❌ OTA Failed (%d): %s\n",
                  httpUpdater.getLastError(),
                  httpUpdater.getLastErrorString().c_str());
    return false;
  }

  // on HTTP_UPDATE_OK the ESP will reboot automatically
  return true;
}

// ————————— Fetch & Draw song —————————

String fetchCurrentSpotifySong() {
  Serial.println("🎵 fetchCurrentSpotifySong()");
  if (ACCESS_TOKEN == "") {
    Serial.println("⚠️ no access token");
    String link = "https://oauth.thespacewaffles.com/login?device_id=" + DEVICE_ID;
    return "<h1>Pair Spotify</h1><p>Visit <a href='" + link + "'>" + link + "</a></p>";
  }

  WiFiClientSecure c; c.setInsecure();
  if (!c.connect(SPOTIFY_HOST, SPOTIFY_PORT)) {
    Serial.println("❌ Spotify connect failed");
    return "<p>Spotify API unreachable</p>";
  }

  c.printf(
    "GET /v1/me/player/currently-playing HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Authorization: Bearer %s\r\n"
    "Connection: close\r\n\r\n",
    SPOTIFY_HOST, ACCESS_TOKEN.c_str()
  );

  String status = c.readStringUntil('\n');
  Serial.print("📡 Spotify HTTP status: ");
  Serial.println(status);

  if (status.indexOf("401") != -1) {
    Serial.println("⚠️ Spotify 401, refreshing token");
    if (refreshAccessToken()) return fetchCurrentSpotifySong();
    return "<p>Session expired. <a href='/pair'>Re-pair</a></p>";
  }
  if (status.indexOf("204") != -1) {
    Serial.println("ℹ️ Nothing playing");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Nothing playing");
    display.display();
    return "<p>Nothing playing</p>";
  }

  while (c.available() && c.readStringUntil('\n') != "\r");
  String body;
  while (c.available()) body += c.readString();
  Serial.print("📨 Spotify body: ");
  Serial.println(body);

  int idx = body.indexOf('{');
  if (idx < 0) {
    Serial.println("❌ Invalid Spotify response");
    return "<p>Invalid response</p>";
  }

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, body.substring(idx))) {
    Serial.println("❌ Spotify JSON parse error");
    return "<p>JSON parse error</p>";
  }

  const char* song   = doc["item"]["name"]              | "Unknown";
  const char* artist = doc["item"]["artists"][0]["name"] | "Unknown";
  int progressMs     = doc["progress_ms"]               | 0;
  Serial.printf("▶️ Now Playing: %s by %s\n", song, artist);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(true);

  int y = 0;
  display.setCursor(0, y);
  display.println("Now Playing:");
  y += LINE_HEIGHT;

  displayWrappedText(0, y, String(song));
  displayWrappedText(0, y, String("by ") + artist);

  display.setCursor(0, y);
  display.println(String("Hello ") + USER_NAME);
  y += LINE_HEIGHT;

  display.display();

  String html = "<h1>Now Playing</h1>";
  html += "<p><b>" + String(song) + "</b> by " + artist + "</p>";
  html += "<p><i>Progress: </i>" + String(progressMs/1000) + "s</p>";
  return html;
}

// ————————— Setup & Loop —————————

void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(500);

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("❌ SSD1306 init failed");
    for (;;);
  }

  // one‑time firmware version
  prefs.begin("ota", false);
  prefs.end();

  // boot splash
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Booting...");
  display.display();
  delay(500);
  display.clearDisplay();
  display.display();

  // device ID
  prefs.begin("ota", false);
  if (!prefs.isKey("device_id")) {
    uint8_t buf[16];
    esp_fill_random(buf, sizeof(buf));
    char tmp[33];
    for (int i = 0; i < 16; i++) sprintf(tmp + 2*i, "%02X", buf[i]);
    DEVICE_ID = String(tmp);
    prefs.putString("device_id", DEVICE_ID);
    Serial.printf("🔑 New DEVICE_ID: %s\n", DEVICE_ID.c_str());
  } else {
    DEVICE_ID = prefs.getString("device_id");
    Serial.printf("🔑 Loaded DEVICE_ID: %s\n", DEVICE_ID.c_str());
  }
  prefs.end();

  // Wi-Fi splash
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Connecting WiFi...");
  display.display();

  // connect
  WiFiManager wm;
  wifiState = CONNECTING;
  updateBlinkInterval();
  if (!wm.autoConnect("ESP32-Setup")) {
    Serial.println("❌ WiFiManager failed");
  }
  wifiState = CONNECTED;
  updateBlinkInterval();

  Serial.printf("✅ IP: %s\n", WiFi.localIP().toString().c_str());

  // mDNS
  if (MDNS.begin("esp32")) {
    Serial.println("✅ mDNS responder started");
  } else {
    Serial.println("❌ mDNS responder failed");
  }

  display.clearDisplay();
  display.display();

  // OTA check
  checkForOTA();

  // initial fetch & endpoints
  latestHtml = fetchCurrentSpotifySong();
  server.on("/", HTTP_GET, [](){
    Serial.println("➡️  HTTP GET /");
    String page = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Now Playing</title></head><body>";
    page += "<div id='content'>" + latestHtml + "</div>";
    if (ACCESS_TOKEN != "") page += "<form action='/logout' method='POST'><button>Log Out</button></form>";
    page += "<script>setInterval(()=>{fetch('/current').then(r=>r.text()).then(h=>document.getElementById('content').innerHTML=h);},"
            + String(POLL_INTERVAL) + ");</script></body></html>";
    server.send(200, "text/html", page);
  });
  server.on("/current", HTTP_GET, [](){
    Serial.println("➡️  HTTP GET /current");
    server.send(200, "text/html", latestHtml);
  });
  server.on("/pair", HTTP_GET, [](){
    Serial.println("➡️  HTTP GET /pair");
    String link = "https://oauth.thespacewaffles.com/login?device_id=" + DEVICE_ID;
    server.send(200, "text/html", "<h1>Pair</h1><p><a href='"+link+"'>"+link+"</a></p>");
  });
  server.on("/logout", HTTP_POST, [](){
    Serial.println("➡️  HTTP POST /logout");
    ACCESS_TOKEN.clear();
    REFRESH_TOKEN.clear();
    unlinkFromServer();
    latestHtml = fetchCurrentSpotifySong();
    server.sendHeader("Location","/");
    server.send(302,"text/plain","");
  });
  server.begin();
  Serial.println("✅ HTTP server started");

  // wait for OAuth
  while (!pollForTokens()) {
    server.handleClient();
    delay(2000);
  }
  latestHtml = fetchCurrentSpotifySong();
  lastPollTime = millis();
}

void loop() {
  // handle button toggle
  bool currentState = digitalRead(BUTTON_PIN);
  if (currentState != lastButtonState) {
    if (currentState == LOW) {
      deviceOn = !deviceOn;
      Serial.printf("🔘 Button pressed, deviceOn = %s\n", deviceOn ? "true" : "false");
      if (deviceOn) {
        display.ssd1306_command(SSD1306_DISPLAYON);
        display.clearDisplay();
        display.setTextSize(2);
        display.setCursor(0, 0);
        display.println("Hello");
        display.display();
      } else {
        display.ssd1306_command(SSD1306_DISPLAYOFF);
      }
      delay(200);
    }
    lastButtonState = currentState;
  }

  // serve web
  server.handleClient();

  // periodic Spotify poll
  unsigned long now = millis();
  if (now - lastPollTime >= POLL_INTERVAL) {
    Serial.println("⏱️ Poll interval reached, refreshing song");
    latestHtml = fetchCurrentSpotifySong();
    lastPollTime = now;
  }

  // LED heartbeat
  static bool ledOn = false;
  if (millis() - lastBlink >= blinkInterval) {
    ledOn = !ledOn;
    digitalWrite(LED_PIN, ledOn);
    lastBlink = millis();
  }

  // Wi-Fi reconnection check
  if (wifiState == CONNECTED && WiFi.status() != WL_CONNECTED) {
    wifiState = DISCONNECTED;
    updateBlinkInterval();
    Serial.println("⚠️ WiFi disconnected");
  }
  if (wifiState == DISCONNECTED && WiFi.status() == WL_CONNECTED) {
    wifiState = CONNECTED;
    updateBlinkInterval();
    Serial.println("✅ WiFi reconnected");
  }
}
