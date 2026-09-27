// ============================================================================
// FIZYCZNY ESP32 - ODBIORCA MQTT (subscriber)
// ============================================================================
// - laczy sie z WiFi i brokerem HiveMQ (tcp://broker.hivemq.com:1883)
// - czyta wiadomosci z topiku "KacperAlanMuszarski" (publikuje je ESP32 z Wokwi)
// - SSD1306 wyswietla 3 views przelaczane przyciskami:
//     1. DANE   - dane z brokera: wilgotnosc, stan, odstraszone koty
//     2. SLUPKI - wizualizacja wilgotnosci + stan + koty
//     3. STATUS - WiFi, MQTT, liczba wiadomosci, czas ostatniej aktualizacji
//
// Payload z Wokwi: {"wilgotnosc":0,"status":"SUCHO","ustrzeloneKoty":0}
//
// Podlaczenie (modul OLED 0.96" z 4 przyciskami, piny aktywne w dol):
//   VCC -> 3V3, GND -> GND, SDA -> GPIO21, SCL -> GPIO22
//   K1 (∧) -> GPIO4   = poprzedni view
//   K2 (∨) -> GPIO5   = nastepny view
//   K3 (#) -> GPIO13  = przelacz diode w Wokwi (wysyla "on"/"off")
//   K4 (✱) -> GPIO14  = skok do DANYCH
//   Przyciski maja wspolne GND modulu; wewnetrzny pull-up, brak rezystorow.
//
// Web dashboard (ta sama siec WiFi co ESP32):
//   http://<IP_ESP32>/      - strona z danymi i przelacznikiem diody
//   GET /api                - dane jako JSON
//   GET /led/toggle         - przelacz diode (jak K3)
//   GET /led?state=on|off   - ustaw diode wprost
//   IP widoczne na view 3 (STATUS)
//
// Wymagane biblioteki: Adafruit SSD1306, Adafruit GFX, PubSubClient, ArduinoJson
//
// Sekrety (SSID/haslo WiFi + webhook Discord) trzymamy w secrets.h, ktory JEST
// W .gitignore - do repo trafia tylko secrets.h.example. Kopiujesz example na
// secrets.h, wpisujesz wlasne wartosci i dopiero wtedy kompilujesz.
// ============================================================================

#include "secrets.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

// ---- WiFi ---- (SSID/haslo: secrets.h - patrz naglowek pliku)

// ---- MQTT ----
const char* MQTT_HOST   = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_TOPIC   = "KacperAlanMuszarski";
const char* MQTT_CLIENT  = "esp32-display-kacper";
const char* LED_TOPIC    = "KacperAlanMuszarski/led";  // stan diody do Wokwi (K3): "on"/"off"

// ---- Discord webhook (powiadomienie o stanie diody) ---- (URL: secrets.h)

// ---- OLED ----
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C
#define OLED_RESET    -1
#define I2C_SDA       21
#define I2C_SCL       22

// ---- Przyciski K1-K4 (do GND modulu, INPUT_PULLUP, aktywne w dol) ----
#define PIN_K1  4    // ∧ poprzedni
#define PIN_K2  5    // ∨ nastepny
#define PIN_K3  13   // # przelacz diode w Wokwi (on/off)
#define PIN_K4  14   // ✱ skok do DANYCH

// ---- Timery ----
const unsigned long MQTT_RETRY_MS   = 10000;  // co ile probujemy polaczyc z brokerem
const unsigned long RENDER_MS       = 200;    // odswiezanie ekranu
const unsigned long DATA_STALE_MS   = 60000;  // po tym czasie dane uznajemy za nieaktualne (wyzej niz PUBLISH_MS=30s)
const unsigned long DEBOUNCE_MS     = 30;
const uint16_t MQTT_CONN_TIMEOUT_S  = 2;      // SEKUNDY - blokada connect() max 2s (PubSubClient domyslnie 15!)

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiClient espClient;
PubSubClient mqtt(espClient);
WebServer server(80);

// ---- Stan danych z topiku ----
struct {
  int humidity = -1;           // wilgotnosc w % (-1 = brak danych)
  char state[24] = "-";        // "status" z brokera, np. "SUCHO"
  int cats = -1;               // ustrzelone koty (-1 = brak danych)
  unsigned long msgCount = 0;
  unsigned long lastMsgMs = 0; // millis() ostatniej wiadomosci
} data;

int activeView = 0;                // 0..2
bool viewDirty = true;             // wymusza przerysowanie
bool ledOn = false;                // stan diody - to MY zarzadzamy, Wokwi sie stosuje
bool ledCmdPending = false;        // stan sie zmienil - wyslemy gdy MQTT polaczone
bool webhookPending = false;       // powiadomienie Discord czeka na WiFi
unsigned long ledCmdSent = 0;      // ile polecen diody juz poszlo

// ---- Debounce przyciskow ----
struct Button {
  uint8_t pin;
  bool raw = false;
  bool stable = false;
  unsigned long lastChange = 0;
};
Button btnK1 = {PIN_K1};
Button btnK2 = {PIN_K2};
Button btnK3 = {PIN_K3};
Button btnK4 = {PIN_K4};

// Zwraca true raz - przy nacisnieciu
bool buttonPressed(Button& b) {
  bool raw = (digitalRead(b.pin) == LOW);
  unsigned long now = millis();
  if (raw != b.raw) {
    b.raw = raw;
    b.lastChange = now;
  }
  if (b.raw != b.stable && (now - b.lastChange) >= DEBOUNCE_MS) {
    b.stable = b.raw;
    if (b.stable) {
      return true;  // krawiznia narastajaca = nacisniecie
    }
  }
  return false;
}

// Powiadomienie na Discord o nowym stanie diody (HTTPS, wiec troszke trwa -
// stad kolejka zamiast wysylania w srodku obslugi przycisku)
void sendDiscordWebhook() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, DISCORD_WEBHOOK)) {
    Serial.println("Webhook: blad begin()");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"content\":\"ESP32: dioda w Wokwi -> ")
                + (ledOn ? "ON" : "OFF") + "\"}";
  int code = http.POST(body);
  Serial.printf("Webhook Discord: HTTP %d\n", code);
  http.end();
}

// ============================================================================
// WEB DASHBOARD
// ============================================================================
// Strona odswieza sie sama co 2 s przez fetch('/api'). Ciemny motyw, layout
// pod telefon (grid 2 kolumny). HTML w PROGMEM - nie zajmuje RAM-u.
const char DASH_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="pl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Dashboard</title>
<style>
 body{font-family:system-ui,sans-serif;background:#111418;color:#e8eaed;margin:0;padding:14px}
 h1{font-size:19px;margin:0 0 14px;display:flex;justify-content:space-between;align-items:center}
 .pill{font-size:11px;padding:3px 9px;border-radius:99px;background:#2a2f36;color:#9aa0a6}
 .grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
 .card{background:#1a1d21;border:1px solid #2a2f36;border-radius:12px;padding:12px}
 .card .lbl{font-size:11px;color:#9aa0a6;text-transform:uppercase;letter-spacing:.5px}
 .card .val{font-size:30px;font-weight:700;margin-top:4px}
 .wide{grid-column:1/-1}
 #status{font-size:19px;font-weight:700}
 #koty{font-size:24px;font-weight:700}
 #stan{font-size:19px;font-weight:700}
 .bar{height:14px;background:#2a2f36;border-radius:7px;overflow:hidden;margin-top:8px}
 .bar i{display:block;height:100%;background:#4285f4;width:0%;transition:width .4s}
 button{width:100%;padding:14px;font-size:16px;font-weight:700;border:0;border-radius:12px;
        background:#4285f4;color:#fff;cursor:pointer}
 button.on{background:#34a853}
 .meta{font-size:12px;color:#9aa0a6;margin-top:10px;line-height:1.6}
 .onoff{font-weight:700}
 .onoff[data-s="ON"]{color:#34a853}.onoff[data-s="OFF"]{color:#9aa0a6}
</style></head><body>
<h1>🌱 ESP32 Dashboard <span class="pill" id="conn">...</span></h1>
<div class="grid">
 <div class="card"><div class="lbl">Wilgotnosc</div>
   <div class="val"><span id="hum">--</span>%</div>
   <div class="bar"><i id="bar"></i></div></div>
 <div class="card"><div class="lbl">Status</div>
   <div class="val" id="stan">-</div></div>
 <div class="card"><div class="lbl">Ustrzelone koty</div>
   <div class="val" id="koty">-</div></div>
 <div class="card"><div class="lbl">Dioda Wokwi</div>
   <div class="val onoff" id="led" data-s="OFF">OFF</div></div>
 <div class="card wide"><button id="btn" class="" onclick="tgl()">Ladowanie...</button>
   <div class="meta">
     Wiadomosci: <b id="msgs">0</b> | Ostatnia dane: <b id="age">-</b><br>
     WiFi: <b id="wifi">-</b> | MQTT: <b id="mqtt">-</b><br>
     Klik diody dziala jak przycisk K3 na ESP32.
   </div></div>
</div>
<script>
async function load(){
  try{
    const r = await fetch('/api'); const d = await r.json();
    document.getElementById('hum').textContent = d.hum;
    document.getElementById('bar').style.width = (d.hum < 0 ? 0 : d.hum) + '%';
    document.getElementById('stan').textContent = d.stan;
    document.getElementById('koty').textContent = d.koty;
    const led = document.getElementById('led');
    led.textContent = d.led ? 'ON' : 'OFF'; led.dataset.s = d.led ? 'ON' : 'OFF';
    const b = document.getElementById('btn');
    b.textContent = d.led ? '🔴 Wylacz diode' : '🟢 Wlacz diode';
    b.className = d.led ? 'on' : '';
    document.getElementById('msgs').textContent = d.msgs;
    document.getElementById('age').textContent = d.age;
    document.getElementById('wifi').textContent = d.wifi;
    document.getElementById('mqtt').textContent = d.mqtt;
    document.getElementById('conn').textContent = 'online';
  }catch(e){ document.getElementById('conn').textContent = 'brak polaczenia'; }
}
async function tgl(){ await fetch('/led/toggle'); await load(); }
load(); setInterval(load, 2000);
</script>
</body></html>)HTML";

// JSON z aktualnym stanem - pobierany przez dashboard co 2 s
void handleApi() {
  char buf[288];
  snprintf(buf, sizeof(buf),
    "{\"hum\":%d,\"stan\":\"%s\",\"koty\":%d,\"led\":%s,"
    "\"msgs\":%lu,\"age\":\"%lus\",\"wifi\":\"%s\",\"mqtt\":\"%s\"}",
    data.humidity,
    data.msgCount > 0 ? data.state : "-",
    data.cats,
    ledOn ? "true" : "false",
    (unsigned long)data.msgCount,
    data.lastMsgMs ? (unsigned long)((millis() - data.lastMsgMs) / 1000) : 0,
    WiFi.status() == WL_CONNECTED ? "OK" : "BRAK",
    mqtt.connected() ? "OK" : "BRAK");
  server.send(200, "application/json", buf);
}

// Przelacz diody - dokladnie ta sama sciezka co przycisk K3
void setLed(bool on) {
  ledOn = on;
  ledCmdPending = true;   // wysle on/off do Wokwi
  webhookPending = true;  // powiadomi Discord
  viewDirty = true;
  Serial.printf("Web: dioda -> %s\n", ledOn ? "on" : "off");
}

void handleLedToggle() {
  setLed(!ledOn);
  server.send(200, "application/json", ledOn ? "{\"led\":true}" : "{\"led\":false}");
}

void handleLedSet() {
  if (server.hasArg("state")) {
    String s = server.arg("state");
    if (s == "on" || s == "1" || s == "true") setLed(true);
    else if (s == "off" || s == "0" || s == "false") setLed(false);
    else { server.send(400, "text/plain", "state=on|off"); return; }
  } else {
    setLed(!ledOn);
  }
  server.send(200, "application/json", ledOn ? "{\"led\":true}" : "{\"led\":false}");
}

void handleRoot() {
  server.send_P(200, "text/html", DASH_HTML);
}

// ============================================================================
// MQTT
// ============================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Kopiujemy payload do bufora zerowanego na koncu (ArduinoJson lub tekst)
  char buf[256];
  unsigned int n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = '\0';

  JsonDocument doc;
  if (!deserializeJson(doc, buf)) {
    // Format glowny: {"wilgotnosc":0,"status":"SUCHO","ustrzeloneKoty":0}
    // Warianty zapasowe: stan/koty, pot1 - dla zgodnosci wstecznej
    if (doc["wilgotnosc"].is<int>()) {
      data.humidity = doc["wilgotnosc"].as<int>();
    } else if (doc["pot1"].is<int>()) {
      data.humidity = map(doc["pot1"].as<int>(), 0, 1023, 0, 100);
    }

    const char* st = doc["status"].as<const char*>();
    if (!st) st = doc["stan"].as<const char*>();
    if (st) strlcpy(data.state, st, sizeof(data.state));

    if (doc["ustrzeloneKoty"].is<int>()) {
      data.cats = doc["ustrzeloneKoty"].as<int>();
    } else if (doc["koty"].is<int>()) {
      data.cats = doc["koty"].as<int>();
    }

    data.msgCount++;
    data.lastMsgMs = millis();
    viewDirty = true;
  } else {
    Serial.printf("Blad JSON: %s\n", buf);
  }
}

void mqttEnsureConnected() {
  static unsigned long lastTry = 0;
  if (mqtt.connected()) return;
  if (lastTry != 0 && millis() - lastTry < MQTT_RETRY_MS) return;  // lastTry=0 -> pierwsza proba od razu
  lastTry = millis();

  Serial.print("Laczenie z brokerem MQTT... ");
  unsigned long t0 = millis();
  if (mqtt.connect(MQTT_CLIENT)) {
    Serial.printf("OK (%lums)\n", millis() - t0);
    mqtt.subscribe(MQTT_TOPIC);
    Serial.printf("Subskrypcja: %s\n", MQTT_TOPIC);
    viewDirty = true;
  } else {
    Serial.printf("blad, kod=%d (%lums)\n", mqtt.state(), millis() - t0);
  }
}

// ============================================================================
// WiFi
// ============================================================================
void wifiEnsureConnected() {
  static unsigned long lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (lastTry != 0 && millis() - lastTry < MQTT_RETRY_MS) return;  // lastTry=0 -> start od razu
  lastTry = millis();

  Serial.printf("Laczenie z WiFi %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  viewDirty = true;
}

// ============================================================================
// Widoki (views)
// ============================================================================
void drawHeader(const char* name, int index) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(name);
  // numer widoku po prawej stronie: "1/3"
  int16_t x1, y1;
  uint16_t w, h;
  char num[6];
  snprintf(num, sizeof(num), "%d/3", index + 1);
  display.getTextBounds(num, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w, 0);
  display.print(num);
  display.drawLine(0, 10, SCREEN_WIDTH - 1, 10, SSD1306_WHITE);
}

void drawStaleMark() {
  // gwiazdka przy tytule, jesli dawno nie bylo wiadomosci
  if (data.lastMsgMs == 0 || millis() - data.lastMsgMs > DATA_STALE_MS) {
    display.setCursor(SCREEN_WIDTH - 26, 0);
    display.print(F("stale"));
  }
}

// View 0 - DANE z brokera: wilgotnosc, stan, koty
void viewData() {
  drawHeader("DANE", 0);
  drawStaleMark();
  display.setTextSize(2);  // duza czcionka na glowne dane
  display.setCursor(0, 15);
  if (data.humidity < 0) {
    display.print(F("--"));
  } else {
    display.printf("%d%%", data.humidity);
  }
  display.setTextSize(1);
  display.setCursor(0, 36);
  display.print(F("Stan: "));
  if (data.msgCount > 0) display.print(data.state);
  else display.print(F("-"));
  display.setCursor(0, 46);
  display.print(F("Odstraszone koty: "));
  if (data.cats < 0) display.print(F("-"));
  else display.print(data.cats);
  display.setCursor(0, 56);
  if (data.msgCount > 0) {
    display.printf("od: %lus temu", (millis() - data.lastMsgMs) / 1000);
  } else {
    display.print(F("czekam na dane..."));
  }
}

// View 1 - SLUPKI: pasek wilgotnosci + stan + koty
void viewBars() {
  drawHeader("SLUPKI", 1);
  drawStaleMark();
  const int barW = SCREEN_WIDTH - 2;

  // pasek wilgotnosci 0..100%
  display.setCursor(0, 14);
  display.printf("Wilgotnosc: %s", data.humidity < 0 ? "-" : "");
  if (data.humidity >= 0) display.printf("%d%%", data.humidity);
  display.drawRect(0, 24, barW, 12, SSD1306_WHITE);
  if (data.humidity > 0) {
    int fill = map(data.humidity, 0, 100, 0, barW - 4);
    display.fillRect(2, 26, fill, 8, SSD1306_WHITE);
  }

  display.setCursor(0, 40);
  display.print(F("Stan: "));
  if (data.msgCount > 0) display.print(data.state);
  else display.print(F("-"));

  display.setCursor(0, 52);
  display.print(F("Koty: "));
  if (data.cats < 0) display.print(F("-"));
  else display.print(data.cats);
}

// View 2 - status polaczenia
void viewStatus() {
  drawHeader("STATUS", 2);
  display.setCursor(0, 16);
  display.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "OK" : "BRAK");
  if (WiFi.status() == WL_CONNECTED) {
    display.printf("RSSI: %d dBm\n", WiFi.RSSI());
    display.print("IP: ");
    display.println(WiFi.localIP());
  }
  display.printf("MQTT: %s\n", mqtt.connected() ? "OK" : "BRAK");
  display.printf("Wiad.: %lu\n", data.msgCount);
  if (data.lastMsgMs > 0) {
    display.printf("Ostatnia: %lus", (millis() - data.lastMsgMs) / 1000);
  } else {
    display.print(F("Ostatnia: -"));
  }
}

// ============================================================================
// Sterowanie
// ============================================================================
void handleButtons() {
  if (buttonPressed(btnK1)) {                    // ∧ poprzedni
    activeView = (activeView + 2) % 3;
    viewDirty = true;
  }
  if (buttonPressed(btnK2)) {                    // ∨ nastepny
    activeView = (activeView + 1) % 3;
    viewDirty = true;
  }
  if (buttonPressed(btnK3)) {                    // # przelacz stan diody
    ledOn = !ledOn;
    ledCmdPending = true;
    webhookPending = true;
    viewDirty = true;
    Serial.printf("K3: dioda -> %s\n", ledOn ? "on" : "off");
  }
  if (buttonPressed(btnK4)) {                    // ✱ skok do DANYCH
    activeView = 0;
    viewDirty = true;
  }
}

void render() {
  display.clearDisplay();
  switch (activeView) {
    case 0: viewData();  break;
    case 1: viewBars();  break;
    case 2: viewStatus(); break;
  }
  display.display();
}

// ============================================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_K1, INPUT_PULLUP);
  pinMode(PIN_K2, INPUT_PULLUP);
  pinMode(PIN_K3, INPUT_PULLUP);
  pinMode(PIN_K4, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("Blad: nie znaleziono SSD1306"));
    while (true) delay(1000);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Start..."));
  display.display();

  wifiEnsureConnected();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(MQTT_CONN_TIMEOUT_S);  // connect() nie zawiesi petli na 15s

  // Web dashboard
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api", HTTP_GET, handleApi);
  server.on("/led/toggle", HTTP_GET, handleLedToggle);
  server.on("/led", HTTP_GET, handleLedSet);
  server.onNotFound([]() { server.send(404, "text/plain", "Nie ma"); });
  server.begin();

  Serial.println(F("setup() OK - wchodze do loop()"));
  render();  // od razu pokaz pierwszy widok zamiast "Start..."
}

void loop() {
  // 1) EKRAN MA PRIORYTET - narysuj zanim cokolwiek sie zablokuje
  static unsigned long lastRender = 0;
  if (viewDirty || millis() - lastRender >= RENDER_MS) {
    render();
    lastRender = millis();
    viewDirty = false;
  }

  // 2) Siec i serwer (nieblokujace)
  wifiEnsureConnected();
  server.handleClient();  // dashboard - obslugiwany w tle

  // 3) MQTT - potencjalnie blokujace (max MQTT_SOCKET_TIMEOUT sekund)
  if (WiFi.status() == WL_CONNECTED) {
    mqttEnsureConnected();
    if (mqtt.connected()) {
      // Wyslij oczekujacy stan diody (retained - Wokwi dostaje tez po starcie)
      if (ledCmdPending) {
        const char* cmd = ledOn ? "on" : "off";
        if (mqtt.publish(LED_TOPIC, cmd, true)) {
          ledCmdPending = false;
          ledCmdSent++;
          viewDirty = true;
          Serial.printf("Wyslano stan diody: %s\n", cmd);
        }
      }
      mqtt.loop();
    }
  }

  // 4) Powiadomienie Discord - czeka na WiFi, wysylane poza obsluga przycisku
  if (webhookPending && WiFi.status() == WL_CONNECTED) {
    sendDiscordWebhook();
    webhookPending = false;
  }

  // 5) Przyciski
  handleButtons();

  delay(5);  // krotka pauza - MQTT, web i przyciski nadal reaguja
}
