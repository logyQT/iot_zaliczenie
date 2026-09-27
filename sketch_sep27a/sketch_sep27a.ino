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
// Payload z Wokwi: {"wilgotnosc":64,"stan":"dobry","koty":3}
//
// Podlaczenie (modul OLED 0.96" z 4 przyciskami, piny aktywne w dol):
//   VCC -> 3V3, GND -> GND, SDA -> GPIO21, SCL -> GPIO22
//   K1 (∧) -> GPIO4   = poprzedni view
//   K2 (∨) -> GPIO5   = nastepny view
//   K3 (#) -> GPIO13  = wyslij polecenie "toggle" diody do Wokwi
//   K4 (✱) -> GPIO14  = skok do DANYCH
//   Przyciski maja wspolne GND modulu; wewnetrzny pull-up, brak rezystorow.
//
// Wymagane biblioteki: Adafruit SSD1306, Adafruit GFX, PubSubClient, ArduinoJson
// ============================================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

// ---- WiFi ----
const char* WIFI_SSID = "KCK_ULM";
const char* WIFI_PASS = "twoje-haslo";

// ---- MQTT ----
const char* MQTT_HOST   = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_TOPIC   = "KacperAlanMuszarski";
const char* MQTT_CLIENT  = "esp32-display-kacper";
const char* LED_TOPIC    = "KacperAlanMuszarski/led";  // polecenie diody do Wokwi (K3)

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
#define PIN_K3  13   // # wyslij polecenie diody (LED) do Wokwi
#define PIN_K4  14   // ✱ skok do DANYCH

// ---- Timery ----
const unsigned long MQTT_RETRY_MS   = 5000;   // co ile probujemy polaczyc z brokerem
const unsigned long RENDER_MS       = 200;    // odswiezanie ekranu
const unsigned long DATA_STALE_MS   = 60000;  // po tym czasie dane uznajemy za nieaktualne (wyzej niz PUBLISH_MS=30s)
const unsigned long DEBOUNCE_MS     = 30;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---- Stan danych z topiku ----
struct {
  int humidity = -1;           // wilgotnosc w % (-1 = brak danych)
  char state[24] = "-";        // "dobry" / "trzeba podlac" / "susza"
  int cats = -1;               // odstraszone koty (-1 = brak danych)
  unsigned long msgCount = 0;
  unsigned long lastMsgMs = 0; // millis() ostatniej wiadomosci
} data;

int activeView = 0;                // 0..2
bool viewDirty = true;             // wymusza przerysowanie
bool ledCmdPending = false;        // K3 wcisniety - wyslemy gdy MQTT polaczone
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
    if (doc["wilgotnosc"].is<int>()) {
      data.humidity = doc["wilgotnosc"].as<int>();
    }
    if (doc["stan"].is<const char*>()) {
      strlcpy(data.state, doc["stan"].as<const char*>(), sizeof(data.state));
    }
    if (doc["koty"].is<int>()) {
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
  if (millis() - lastTry < MQTT_RETRY_MS) return;
  lastTry = millis();

  Serial.print("Laczenie z brokerem MQTT... ");
  if (mqtt.connect(MQTT_CLIENT)) {
    Serial.println("OK");
    mqtt.subscribe(MQTT_TOPIC);
    Serial.printf("Subskrypcja: %s\n", MQTT_TOPIC);
    viewDirty = true;
  } else {
    Serial.printf("blad, kod=%d\n", mqtt.state());
  }
}

// ============================================================================
// WiFi
// ============================================================================
void wifiEnsureConnected() {
  static unsigned long lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastTry < MQTT_RETRY_MS) return;
  lastTry = millis();

  Serial.printf("Laczenie z WiFi %s", WIFI_SSID);
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
  if (buttonPressed(btnK3)) {                    // # wyslij polecenie diody
    ledCmdPending = true;
    Serial.println("K3: polecenie diody do wyslania");
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
}

void loop() {
  wifiEnsureConnected();

  if (WiFi.status() == WL_CONNECTED) {
    mqttEnsureConnected();
    if (mqtt.connected()) {
      // Wyslij oczekujace polecenie diody (np. po wcisnieciu K3 lub po reconnectie)
      if (ledCmdPending) {
        if (mqtt.publish(LED_TOPIC, "toggle")) {
          ledCmdPending = false;
          ledCmdSent++;
          viewDirty = true;
          Serial.println("Wyslano polecenie diody (toggle)");
        }
      }
      mqtt.loop();
    }
  }

  handleButtons();

  // Przerysuj gdy dane sie zmienily albo co RENDER_MS (zegary w tekscie)
  static unsigned long lastRender = 0;
  if (viewDirty || millis() - lastRender >= RENDER_MS) {
    render();
    lastRender = millis();
    viewDirty = false;
  }

  delay(10);  // krotka pauza, przyciski i MQTT nadal reaguja
}
