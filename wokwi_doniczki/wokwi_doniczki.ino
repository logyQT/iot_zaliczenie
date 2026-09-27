// ============================================================================
// WOKWI - DANE OGRODOWE (publisher MQTT)
// ============================================================================
// Symulowany ESP32 zbiera dane i publikuje je na topiku "KacperAlanMuszarski"
// w brokerze HiveMQ - dokladnie tam czyta fizyczny ESP32 z ekranem SSD1306.
//
// Dane:
//   - wilgotnosc : potencjometr (GPIO36), 0..1023 -> 0..100 %
//   - stan       : liczony z wilgotnosci: >=60 dobry, 30..59 trzeba podlac, <30 susza
//   - koty       : czujnik ruchu PIR (GPIO16) - kazda detekcja = +1 kot odstraszony
//
// Sterowanie zwrotne:
//   - subskrybuje "KacperAlanMuszarski/led"; kazda wiadomosc przełącza diode (GPIO25)
//   - fizyczny ESP32 wysyla "toggle" na ten topic po nacisnieciu K3
//
// Publikacja: przy kazdej zmianie danych albo co 30 s (swieze dane).
// Przyklad payloadu: {"wilgotnosc":64,"stan":"dobry","koty":3}
//
// Obwod (diagram.json):
//   pot.SIG  -> GPIO36   pir.OUT -> GPIO16   led.A <- R220 <- GPIO25   zasilanie 3V3/GND
//
// Uwaga: uzywamy pinow ADC1 (32-39), bo ADC2 nie dziala gdy WiFi jest aktywne.
// ============================================================================

#include <WiFi.h>
#include <PubSubClient.h>

// ---- WiFi w Wokwi ----
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";          // otwarta siec
const uint8_t WIFI_CHANNEL = 6;      // kanal 6 = szybsze polaczenie w symulacji

// ---- MQTT ----
const char* MQTT_HOST    = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_TOPIC   = "KacperAlanMuszarski";
const char* MQTT_CLIENT  = "esp32-wokwi-doniczki";
const char* LED_TOPIC    = "KacperAlanMuszarski/led";  // polecenie z fizycznego ESP32 (K3)

// ---- Piny ----
const int PIN_POT = 36;   // wilgotnosc (ADC1)
const int PIN_PIR = 16;   // czujnik ruchu (koty)
const int PIN_LED = 25;   // dioda sterowana z fizycznego ESP32

// ---- Progi stanu (w %) ----
const int PROG_DOBRY    = 60;   // >= 60 -> "dobry"
const int PROG_PODLAC   = 30;   // 30..59 -> "trzeba podlac", <30 -> "susza"

const unsigned long PUBLISH_MS = 30000;  // swieze dane co 30 s
const unsigned long RETRY_MS   = 5000;

WiFiClient espClient;
PubSubClient mqtt(espClient);

int humidity = -1;          // odczyt % z potencjometru
int cats = 0;               // odstraszone koty (rosnie po detekcji PIR)
bool pirPrev = false;

int lastSentHum = -1;
int lastSentCats = -1;
unsigned long lastPublish = 0;
unsigned long published = 0;
bool ledOn = false;

// Wiadomosci przychodzace z fizycznego ESP32 (przelaczanie diody)
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, LED_TOPIC) != 0) return;
  ledOn = !ledOn;
  digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
  Serial.printf("LED %s\n", ledOn ? "ON" : "OFF");
}

// Stan z wilgotnosci - UWAGA: bez polskich znakow (font OLED je ma tylko czesciowo)
const char* computeState(int hum) {
  if (hum < 0) return "-";
  if (hum >= PROG_DOBRY) return "dobry";
  if (hum >= PROG_PODLAC) return "trzeba podlac";
  return "susza";
}

void wifiEnsureConnected() {
  static unsigned long lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastTry < RETRY_MS) return;
  lastTry = millis();
  Serial.printf("Laczenie z WiFi %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
}

void mqttEnsureConnected() {
  static unsigned long lastTry = 0;
  if (mqtt.connected()) return;
  if (millis() - lastTry < RETRY_MS) return;
  lastTry = millis();
  Serial.print("Laczenie z brokerem... ");
  if (mqtt.connect(MQTT_CLIENT)) {
    Serial.println("OK");
    mqtt.subscribe(LED_TOPIC);
    Serial.printf("Subskrypcja: %s\n", LED_TOPIC);
  } else {
    Serial.printf("blad, kod=%d\n", mqtt.state());
  }
}

// Zliczamy narastajace zbocze na PIR = jedna detekcja ruchu = jeden kot
void readPir() {
  bool sig = digitalRead(PIN_PIR) == HIGH;
  if (sig && !pirPrev) {
    cats++;
    Serial.printf("Ruch! Koty odstraszeni: %d\n", cats);
  }
  pirPrev = sig;
}

void publishData() {
  int newHum = map(analogRead(PIN_POT), 0, 1023, 0, 100);

  bool changed = (newHum != humidity) || (cats != lastSentCats);
  bool heartbeat = (millis() - lastPublish >= PUBLISH_MS);
  if (!changed && !heartbeat) return;
  if (lastPublish != 0 && millis() - lastPublish < 100) return;  // nie zalewamy brokera

  humidity = newHum;
  const char* stan = computeState(humidity);

  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"wilgotnosc\":%d,\"stan\":\"%s\",\"koty\":%d}",
           humidity, stan, cats);

  if (mqtt.publish(MQTT_TOPIC, payload)) {
    lastSentHum = humidity;
    lastSentCats = cats;
    lastPublish = millis();
    published++;
    Serial.printf("Wyslano #%lu: %s\n", published, payload);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_POT, INPUT);
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  wifiEnsureConnected();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
}

void loop() {
  wifiEnsureConnected();
  readPir();

  if (WiFi.status() == WL_CONNECTED) {
    mqttEnsureConnected();
    if (mqtt.connected()) {
      publishData();
      mqtt.loop();
    }
  }

  delay(20);
}
