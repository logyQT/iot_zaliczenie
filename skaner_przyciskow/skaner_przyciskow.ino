// ============================================================================
// SKANER PRZYCISKOW - Ktory GPIO obsluguje K1..K4?
// ============================================================================
// Wgrywasz, otwierasz Monitor Szeregowy (115200) i naciskasz przyciski.
// Przy kazdym nacisnieniu wypisze: pin, stan i etykiete K1..K4 (wg kolejnosci
// pierwszych reakcji - Ty sam przypiszesz co naciskales).
//
// Dziala na zasadzie INPUT_PULLUP: przycisk zwiera pin do GND, wiec
// nacisniecie = stan LOW. Nie podawaj zadnych zewnetrznych napiec na te piny!
// ============================================================================

// Kandydaci - piny wejsciowe ESP32 (bez strappingowych 0/2/12 na wszelki wypadek)
const int PINS[] = {4, 5, 13, 14, 15, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33, 34, 35, 39};
const int N = sizeof(PINS) / sizeof(PINS[0]);

int prevState[N];

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== SKANER PRZYCISKOW K1..K4 ===");
  Serial.println("Naciskaj przyciski - wypisze sie reakcja.");

  for (int i = 0; i < N; i++) {
    pinMode(PINS[i], INPUT_PULLUP);
    prevState[i] = digitalRead(PINS[i]);
    Serial.printf("  GPIO%-2d start=%s\n", PINS[i], prevState[i] ? "HIGH" : "LOW");
  }
  Serial.println("--- gotowy, wciskaj! ---");
}

void loop() {
  for (int i = 0; i < N; i++) {
    int s = digitalRead(PINS[i]);
    if (s != prevState[i]) {
      prevState[i] = s;
      unsigned long t = millis() / 1000;
      Serial.printf("[%lus] GPIO%-2d -> %s %s\n",
                    t, PINS[i], s ? "HIGH" : "LOW",
                    s ? "(puszczenie)" : "<== NACISNIECIE");
    }
  }
  delay(20);
}
