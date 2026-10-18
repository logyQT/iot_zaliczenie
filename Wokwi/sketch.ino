/*
 * Strażnik Rośliny — ESP32 + Wokwi
 * - czujnik wilgotności gleby (symulowany potencjometrem) -> 3 progi: dobry / trzeba podlać / za sucho
 * - pompka wody uruchamia się, gdy wilgotność spadnie poniżej progu (żółty/czerwony)
 * - czujnik PIR wykrywa zbliżającego się kota -> uruchamia "pistolet na wodę", liczy trafienia
 *   i wysyła alert na Discorda (webhook)
 * - przycisk fizyczny obok ekranu zapala/gasi diodę; diodą można też sterować zdalnie przez MQTT
 *   (topic KacperAlanMuszarski/led, payload "on"/"off")
 * - dane z czujników publikowane po MQTT (broker.hivemq.com, topic KacperAlanMuszarski)
 * - wbudowany serwer WWW (port 80) z panelem HTML: te same dane co na ekranie + przycisk diody
 * - wszystko wyświetlane na OLED SSD1306: wilgotność w %, status jako buźka, czas od ostatniego
 *   podlania, liczba ustrzelonych kotów
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>

// ---------- WiFi ----------
// "Wokwi-GUEST" to wirtualna otwarta sieć symulatora Wokwi z dostępem do prawdziwego internetu.
// Na fizycznym ESP32 podmień na dane swojej domowej sieci WiFi.
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// ---------- MQTT ----------
const char* MQTT_BROKER    = "broker.hivemq.com";
const int   MQTT_PORT      = 1883;
const char* MQTT_TOPIC     = "KacperAlanMuszarski";
const char* MQTT_TOPIC_LED = "KacperAlanMuszarski/led"; // sterowanie diodą zdalnie: payload "on"/"off"
const unsigned long MQTT_PUBLISH_INTERVAL_MS = 5000;

bool mqttLedOn = false; // stan diody zadany zdalnie przez MQTT (OR-owany z przyciskiem fizycznym)

// ---------- Discord ----------
// USUNIĘTO ZE WZGLĘDÓW BEZPIECZEŃSTWA (webhook to w praktyce hasło do kanału).
// W CELU POPRAWNEGO DZIAŁANIA alertów należy dodać własny webhook:
// Ustawienia kanału na Discordzie -> Integracje -> Webhooki -> Nowy webhook -> kopiuj URL
// i wkleić poniżej w miejsce placeholdera.
const char* DISCORD_WEBHOOK_URL = "https://discord.com/api/webhooks/TWOJE_ID/TWOJ_TOKEN";

WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttPublish = 0;

WebServer webServer(80);
bool webLedOn = false; // stan diody zadany przez przycisk w panelu WWW

// ---------- piny ----------
#define PIN_SOIL        34   // ADC - czujnik wilgotności (potencjometr w symulacji)
#define PIN_PIR         27   // czujnik ruchu PIR
#define PIN_PUMP_WATER  26   // przekaźnik: pompka do podlewania
#define PIN_PUMP_CAT    25   // przekaźnik: "pistolet" na kota
#define PIN_BUTTON      32   // przycisk fizyczny obok ekranu
#define PIN_LED         33   // dioda sterowana przyciskiem

// ---------- ekran ----------
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------- progi wilgotności ----------
const int MOISTURE_GREEN  = 60;  // >=60%  -> dobry
const int MOISTURE_YELLOW = 30;  // 30-59% -> trzeba podlać (włącza pompkę)
                                  // <30%   -> za sucho (pompka też leci)
const int MOISTURE_STOP   = 65;  // histerezys: pompka gasi się dopiero po przekroczeniu tego progu

// ---------- czasy ----------
const unsigned long WATER_PUMP_MS   = 2000;  // jak długo maksymalnie leci podlewanie na "raz" (bezpiecznik)
const unsigned long CAT_BLAST_MS    = 1500;  // jak długo trwa psiknięcie
const unsigned long CAT_COOLDOWN_MS = 8000;  // odstęp między kolejnymi psiknięciami

// ---------- stan ----------
Preferences prefs;

bool pumpWaterOn = false;
unsigned long pumpWaterStartedAt = 0;
unsigned long lastWateredAtMillis = 0;
bool everWatered = false;

bool catBlastOn = false;
unsigned long catBlastStartedAt = 0;
unsigned long lastCatBlastAt = 0;
uint32_t catsHit = 0;

unsigned long lastDisplayUpdate = 0;

enum MoistureStatus { STATUS_GOOD, STATUS_WARN, STATUS_DRY };

int readMoisturePercent() {
  int raw = analogRead(PIN_SOIL);           // 0-4095
  int percent = map(raw, 0, 4095, 0, 100);
  return constrain(percent, 0, 100);
}

MoistureStatus statusFromMoisture(int percent) {
  if (percent >= MOISTURE_GREEN) return STATUS_GOOD;
  if (percent >= MOISTURE_YELLOW) return STATUS_WARN;
  return STATUS_DRY;
}

const char* statusToStr(MoistureStatus status) {
  return status == STATUS_GOOD ? "OK" : status == STATUS_WARN ? "PODLEJ" : "SUCHO";
}

void setPumpWater(bool on) {
  pumpWaterOn = on;
  digitalWrite(PIN_PUMP_WATER, on ? HIGH : LOW);
  if (on) {
    pumpWaterStartedAt = millis();
    lastWateredAtMillis = millis();
    everWatered = true;
  }
}

void setCatBlast(bool on) {
  catBlastOn = on;
  digitalWrite(PIN_PUMP_CAT, on ? HIGH : LOW);
  if (on) {
    catBlastStartedAt = millis();
  }
}

void connectWiFi() {
  Serial.printf("Łączenie z WiFi '%s'...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi połączone, IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nNie udało się połączyć z WiFi — funkcje MQTT/Discord będą wyłączone.");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  msg.toLowerCase();

  if (String(topic) == MQTT_TOPIC_LED) {
    mqttLedOn = (msg == "on" || msg == "1" || msg == "true");
    Serial.printf("MQTT <- %s: \"%s\" -> dioda %s\n", topic, msg.c_str(), mqttLedOn ? "ON" : "OFF");
  }
}

void mqttEnsureConnected() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  String clientId = "esp32-straznik-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("MQTT: połączono z broker.hivemq.com");
    mqttClient.subscribe(MQTT_TOPIC_LED);
    Serial.printf("MQTT: subskrybuję %s\n", MQTT_TOPIC_LED);
  }
}

void mqttLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  mqttEnsureConnected();
  mqttClient.loop();
}

void publishMqtt(int moisturePercent, MoistureStatus status) {
  if (!mqttClient.connected()) return;

  String payload = "{\"wilgotnosc\":" + String(moisturePercent) +
                    ",\"status\":\"" + statusToStr(status) + "\"" +
                    ",\"ustrzeloneKoty\":" + String(catsHit) + "}";
  mqttClient.publish(MQTT_TOPIC, payload.c_str(), true);
  Serial.printf("MQTT -> %s: %s\n", MQTT_TOPIC, payload.c_str());
}

void sendDiscordAlert(const String& message) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (https.begin(client, DISCORD_WEBHOOK_URL)) {
    https.addHeader("Content-Type", "application/json");
    String payload = "{\"content\":\"" + message + "\"}";
    int code = https.POST(payload);
    Serial.printf("Discord webhook -> HTTP %d\n", code);
    https.end();
  }
}

String wateredStatusText() {
  if (!everWatered) return "jeszcze nigdy";
  if (pumpWaterOn) return "teraz...";
  return formatAgo(millis() - lastWateredAtMillis);
}

// ---------- panel WWW ----------
const char PANEL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Strażnik Rośliny</title>
<style>
  body { font-family: system-ui, sans-serif; background: #111; color: #eee; text-align: center; padding: 24px; margin: 0; }
  .card { background: #1e1e1e; border-radius: 16px; padding: 24px; max-width: 340px; margin: 0 auto; box-shadow: 0 4px 16px rgba(0,0,0,.4); }
  h1 { font-size: 20px; margin: 0 0 16px; }
  .moisture { font-size: 56px; font-weight: bold; margin: 8px 0; }
  .row { display: flex; justify-content: space-between; padding: 6px 0; border-top: 1px solid #333; font-size: 14px; }
  .status-OK { color: #4caf50; }
  .status-PODLEJ { color: #ffb300; }
  .status-SUCHO { color: #f44336; }
  button { font-size: 16px; padding: 14px 28px; border-radius: 10px; border: none; margin-top: 20px; cursor: pointer; width: 100%; font-weight: bold; }
  .on { background: #4caf50; color: #fff; }
  .off { background: #444; color: #ccc; }
</style>
</head>
<body>
<div class="card">
  <h1>🌱 Strażnik Rośliny</h1>
  <div class="moisture" id="moisture">--%</div>
  <div class="row"><span>Status gleby</span><span id="status">--</span></div>
  <div class="row"><span>Podlano</span><span id="watered">--</span></div>
  <div class="row"><span>Ustrzelone koty</span><span id="cats">--</span></div>
  <div class="row"><span>WiFi</span><span id="wifi">--</span></div>
  <button id="ledBtn" onclick="toggleLed()">...</button>
</div>
<script>
let ledOn = false;
async function refresh() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    document.getElementById('moisture').textContent = d.moisture + '%';
    const statusEl = document.getElementById('status');
    statusEl.textContent = d.status;
    statusEl.className = 'status-' + d.status;
    document.getElementById('watered').textContent = d.watered;
    document.getElementById('cats').textContent = d.catsHit;
    document.getElementById('wifi').textContent = d.wifi ? 'połączono' : 'brak';
    ledOn = d.ledOn;
    const btn = document.getElementById('ledBtn');
    btn.textContent = ledOn ? 'Dioda: WŁĄCZONA (wyłącz)' : 'Dioda: wyłączona (włącz)';
    btn.className = ledOn ? 'on' : 'off';
  } catch (e) {}
}
async function toggleLed() {
  await fetch(ledOn ? '/api/led/off' : '/api/led/on');
  refresh();
}
refresh();
setInterval(refresh, 1500);
</script>
</body>
</html>
)rawliteral";

void handleWebRoot() {
  webServer.send_P(200, "text/html", PANEL_HTML);
}

void handleApiStatus() {
  int moisturePercent = readMoisturePercent();
  MoistureStatus status = statusFromMoisture(moisturePercent);
  bool ledOn = digitalRead(PIN_LED) == HIGH;

  String json = "{";
  json += "\"moisture\":" + String(moisturePercent) + ",";
  json += "\"status\":\"" + String(statusToStr(status)) + "\",";
  json += "\"watered\":\"" + wateredStatusText() + "\",";
  json += "\"catsHit\":" + String(catsHit) + ",";
  json += "\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ledOn\":" + String(ledOn ? "true" : "false");
  json += "}";
  webServer.send(200, "application/json", json);
}

void handleApiLedOn() {
  webLedOn = true;
  handleApiStatus();
}

void handleApiLedOff() {
  webLedOn = false;
  handleApiStatus();
}

void handleWatering(int moisturePercent) {
  if (!pumpWaterOn && moisturePercent < MOISTURE_GREEN) {
    setPumpWater(true);
  }
  if (pumpWaterOn) {
    bool dryEnoughToStop = moisturePercent >= MOISTURE_STOP;
    bool safetyTimeout = (millis() - pumpWaterStartedAt) >= WATER_PUMP_MS;
    if (dryEnoughToStop || safetyTimeout) {
      setPumpWater(false);
    }
  }
}

void handleCatDefense() {
  bool motion = digitalRead(PIN_PIR) == HIGH;

  if (!catBlastOn && motion && (millis() - lastCatBlastAt) >= CAT_COOLDOWN_MS) {
    setCatBlast(true);
    lastCatBlastAt = millis();
    catsHit++;
    prefs.putUInt("catsHit", catsHit);
    Serial.printf("Kot wykryty! Lacznie ustrzelonych: %u\n", catsHit);
    sendDiscordAlert("🐱 Kot wykryty przy roślinie! Łącznie odstraszonych: " + String(catsHit));
  }

  if (catBlastOn && (millis() - catBlastStartedAt) >= CAT_BLAST_MS) {
    setCatBlast(false);
  }
}

void handleButtonLed() {
  bool pressed = digitalRead(PIN_BUTTON) == LOW; // przycisk z INPUT_PULLUP -> LOW gdy wciśnięty
  bool ledOn = pressed || mqttLedOn || webLedOn; // dioda świeci od przycisku, MQTT LUB panelu WWW
  digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
}

// rysuje prostą buźkę (wektorowo, bez bitmapy) w punkcie (cx, cy) o promieniu r
void drawFace(int cx, int cy, int r, MoistureStatus status) {
  display.drawCircle(cx, cy, r, SSD1306_WHITE);

  int eyeOffsetX = r / 2;
  int eyeOffsetY = r / 3;
  display.fillCircle(cx - eyeOffsetX, cy - eyeOffsetY, 2, SSD1306_WHITE);
  display.fillCircle(cx + eyeOffsetX, cy - eyeOffsetY, 2, SSD1306_WHITE);

  int mouthW = r * 2 / 3;
  int mouthY = cy + r / 3;

  switch (status) {
    case STATUS_GOOD:
      // uśmiech: "U"
      display.drawLine(cx - mouthW, mouthY, cx, mouthY + 5, SSD1306_WHITE);
      display.drawLine(cx, mouthY + 5, cx + mouthW, mouthY, SSD1306_WHITE);
      break;
    case STATUS_WARN:
      // neutralna: prosta linia
      display.drawLine(cx - mouthW, mouthY + 2, cx + mouthW, mouthY + 2, SSD1306_WHITE);
      break;
    case STATUS_DRY:
      // smutna: odwrócone "U"
      display.drawLine(cx - mouthW, mouthY + 5, cx, mouthY, SSD1306_WHITE);
      display.drawLine(cx, mouthY, cx + mouthW, mouthY + 5, SSD1306_WHITE);
      break;
  }
}

String formatAgo(unsigned long ms) {
  unsigned long totalSec = ms / 1000;
  unsigned long totalMin = totalSec / 60;
  if (totalMin < 1) return String(totalSec) + "s temu";
  if (totalMin < 60) return String(totalMin) + "min temu";
  unsigned long hours = totalMin / 60;
  return String(hours) + "h temu";
}

void updateDisplay(int moisturePercent, MoistureStatus status) {
  display.clearDisplay();

  // buźka + status po lewej
  drawFace(20, 20, 15, status);

  // wilgotność po prawej, dużym tekstem
  display.setTextSize(2);
  display.setCursor(48, 4);
  display.print(moisturePercent);
  display.print("%");

  display.setTextSize(1);
  display.setCursor(48, 22);
  switch (status) {
    case STATUS_GOOD: display.print("Gleba: OK"); break;
    case STATUS_WARN: display.print("Podlej!"); break;
    case STATUS_DRY:  display.print("Za sucho!"); break;
  }

  // kropka statusu WiFi w prawym górnym rogu (wypełniona = połączono)
  if (WiFi.status() == WL_CONNECTED) {
    display.fillCircle(122, 4, 3, SSD1306_WHITE);
  } else {
    display.drawCircle(122, 4, 3, SSD1306_WHITE);
  }

  // linia oddzielająca
  display.drawLine(0, 40, SCREEN_WIDTH, 40, SSD1306_WHITE);

  // ostatnie podlanie
  display.setCursor(0, 46);
  display.print("Podlano: ");
  display.print(wateredStatusText());

  // licznik kotów
  display.setCursor(0, 56);
  display.print("Ustrzelone koty: ");
  display.print(catsHit);
  if (catBlastOn) {
    display.print(" *PSIK*");
  }

  display.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_PUMP_WATER, OUTPUT);
  pinMode(PIN_PUMP_CAT, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_PUMP_WATER, LOW);
  digitalWrite(PIN_PUMP_CAT, LOW);
  digitalWrite(PIN_LED, LOW);

  prefs.begin("straznik", false);
  catsHit = prefs.getUInt("catsHit", 0);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Nie znaleziono OLED SSD1306!");
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Strazik Rosliny");
  display.println("startuje...");
  display.display();

  connectWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  webServer.on("/", handleWebRoot);
  webServer.on("/api/status", handleApiStatus);
  webServer.on("/api/led/on", handleApiLedOn);
  webServer.on("/api/led/off", handleApiLedOff);
  webServer.begin();
  Serial.printf("Panel WWW: http://%s/\n", WiFi.localIP().toString().c_str());

  delay(500);
}

void loop() {
  int moisturePercent = readMoisturePercent();
  MoistureStatus status = statusFromMoisture(moisturePercent);

  webServer.handleClient();
  mqttLoop();

  handleWatering(moisturePercent);
  handleCatDefense();
  handleButtonLed();

  if (millis() - lastMqttPublish >= MQTT_PUBLISH_INTERVAL_MS) {
    publishMqtt(moisturePercent, status);
    lastMqttPublish = millis();
  }

  if (millis() - lastDisplayUpdate >= 300) {
    updateDisplay(moisturePercent, status);
    lastDisplayUpdate = millis();
  }
}
