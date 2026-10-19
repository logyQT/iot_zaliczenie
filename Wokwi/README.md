# Strażnik Rośliny (ESP32 + Wokwi)

Inteligentna doniczka na ESP32: mierzy wilgotność gleby i sama podlewa, odstrasza koty
"pistoletem na wodę", pokazuje dane na ekranie OLED, wysyła je po MQTT, alarmuje na Discordzie i
ma własny panel WWW.

## Pliki w repozytorium

| Plik | Co to |
|---|---|
| [`sketch.ino`](sketch.ino) | **Cały kod programu** (Arduino/ESP32) — do przeczytania bezpośrednio tutaj, bez uruchamiania Wokwi |
| [`diagram.json`](diagram.json) | Schemat obwodu dla symulatora Wokwi (ESP32, OLED, czujnik, PIR, przekaźniki, przycisk, LED) |
| [`libraries.txt`](libraries.txt) | Lista bibliotek Arduino używanych przez projekt |

## Jak uruchomić w Wokwi (najprościej, przez przeglądarkę)

1. Wejdź na https://wokwi.com/projects/new/esp32 (zakłada nowy projekt ESP32).
2. W zakładce **sketch.ino** wklej całą zawartość [sketch.ino](sketch.ino) z tego folderu.
3. W **Library Manager** (zakładka obok sketch.ino) dodaj (przycisk "+"): **Adafruit GFX Library**,
   **Adafruit SSD1306**, **PubSubClient** (autor: Nick O'Leary) — Wokwi samo je pobierze i skompiluje.
4. Otwórz zakładkę **diagram.json**, podmień jej zawartość na [diagram.json](diagram.json).
   - Ten plik został przetestowany na żywo w symulatorze Wokwi (kompilacja + działający ekran +
     podlewanie + wykrywanie kota + WiFi + MQTT + Discord) — powinien zadziałać od razu po wklejeniu.
5. Kliknij zielony przycisk **Play** — symulacja wystartuje (pierwsza kompilacja potrafi trwać
   30–90s na darmowym koncie Wokwi, jest kolejka do serwera budowania).
6. Testowanie:
   - **Wilgotność gleby** = pokrętło potencjometru (przeciągnij myszką) — zmienia % na ekranie.
   - **Kot** = kliknij na czujnik PIR na diagramie → pojawi się panel "PIR Motion Sensor" z
     przyciskiem **Simulate motion** — kliknij go, żeby wymusić wykrycie ruchu (uruchomi też
     alert na Discordzie).
   - **Przycisk/dioda** = kliknij i przytrzymaj pushbutton na diagramie — dioda obok powinna się
     zapalić na czas trzymania.
   - Kropka w prawym górnym rogu ekranu OLED = status WiFi (wypełniona = połączono).

## WiFi / MQTT / Discord — jak to działa i co przetestowałem live

To wszystko zostało uruchomione naprawdę w symulatorze (nie tylko napisane w kodzie):

- **WiFi**: ESP32 łączy się z siecią `Wokwi-GUEST` — to wirtualna, otwarta sieć symulatora z
  prawdziwym dostępem do internetu. Potwierdzone w Serialu: `WiFi połączone, IP: 10.10.0.2`.
  Na fizycznym sprzęcie dane sieci wpisujesz w `fizyczny_esp32/secrets.h` (nie w tym pliku) —
  patrz sekcja 7 w głównym README.
- **MQTT**: dane z czujników (wilgotność, status, liczba kotów) publikowane są co 5s na
  `tcp://broker.hivemq.com:1883`, topic **`KacperAlanMuszarski`**, jako JSON, np.:
  `{"wilgotnosc":42,"status":"OK","ustrzeloneKoty":2}` — z flagą `retain`, więc każdy kto
  zasubskrybuje ten topic (np. przez http://www.hivemq.com/demos/websocket-client/) od razu
  zobaczy ostatnią wartość. Potwierdzone live: `MQTT: połączono z broker.hivemq.com` i faktyczne
  publikacje w Serialu.
- **Sterowanie diodą przez MQTT**: ESP32 subskrybuje `KacperAlanMuszarski/led`. Publikacja
  payloadu `"on"`/`"1"`/`"true"` zapala diodę, cokolwiek innego (np. `"off"`) gasi ją. Dioda
  świeci gdy **przycisk fizyczny jest wciśnięty LUB** ostatnia komenda MQTT to „on” (logika OR) —
  do zmiany w `handleButtonLed()`, jeśli wolisz żeby MQTT miał pierwszeństwo/nadpisywał przycisk.
  Potwierdzone live przez bezpośredni odczyt stanu diody w silniku symulacji: jasność skoczyła
  z 0 do 0.96 w trakcie trzymania przycisku i wróciła do 0 po puszczeniu, niezależnie od MQTT.
- **Discord**: przy wykryciu kota (PIR) ESP32 wysyła POST na webhook Discorda. Test live z
  prawdziwym webhookiem zwrócił `Discord webhook -> HTTP 204`, czyli wiadomość faktycznie dotarła
  na kanał.
  ⚠️ **Webhook został usunięty z kodu ze względów bezpieczeństwa** (to w praktyce hasło do
  kanału — każdy kto go ma, może wysyłać na niego wiadomości). W `sketch.ino` stała
  `DISCORD_WEBHOOK_URL` zawiera tylko placeholder. **W celu poprawnego działania alertów trzeba
  dodać własny webhook**: Ustawienia kanału na Discordzie → Integracje → Webhooki → Nowy webhook →
  kopiuj URL i wklej go w `sketch.ino` w miejsce `https://discord.com/api/webhooks/TWOJE_ID/TWOJ_TOKEN`.
  Bez tego reszta projektu działa normalnie, tylko alert na Discorda zwróci błąd w Serialu.
- **Panel WWW**: ESP32 uruchamia własny serwer HTTP (port 80, biblioteka `WebServer.h` — wbudowana
  w rdzeń ESP32, nic dodatkowego nie trzeba instalować). Strona `/` pokazuje te same dane co ekran
  (wilgotność, status, czas od podlania, liczbę kotów, WiFi) plus przycisk włączający/wyłączający
  diodę (`/api/led/on`, `/api/led/off`, dane jako JSON pod `/api/status`, odświeżanie co 1,5s).
  Adres drukowany jest w Serialu po starcie (`Panel WWW: http://<ip>/`).
  ⚠️ **W symulatorze Wokwi tego adresu nie da się otworzyć z przeglądarki** — sieć `Wokwi-GUEST`
  daje wirtualnemu ESP32 tylko dostęp *wychodzący* do internetu (dlatego MQTT/Discord działają),
  ale nie routuje ruchu *przychodzącego* z powrotem do urządzenia w symulacji. Sprawdzone: próba
  wejścia na wydrukowany adres z osobnej karty kończy się błędem sieciowym po stronie przeglądarki.
  To ograniczenie samego Wokwi, nie błąd w kodzie — **na prawdziwym ESP32 podłączonym do domowej
  sieci WiFi panel zadziała od razu**: dowolne urządzenie w tej samej sieci wejdzie na wydrukowany
  adres IP i zobaczy pełny panel.

### Uwaga o nazwach pinów w Wokwi

Jeśli będziesz sam edytować `diagram.json` albo dodawać kolejne elementy: piny GPIO na płytce
`board-esp32-devkit-c-v4` w Wokwi nazywają się **samą liczbą** (`"esp:21"`, `"esp:34"`, `"esp:26"`...),
**bez przedrostka "D"** (nie `"esp:D21"`). To nietypowe względem starszych diagramów, które można
znaleźć w sieci (tam bywa `wokwi-esp32-devkit-v1` z inną konwencją). Użycie złej nazwy pinu nie
rzuca błędu — Wokwi po prostu po cichu nie rysuje przewodu i pin zostaje niepodłączony, co ciężko
zauważyć bez uważnego sprawdzenia diagramu. Piny GND mają tylko 3 warianty (`GND.1`, `GND.2`,
`GND.3`) — kolejne urządzenia po prostu podłącza się ponownie do jednego z tych trzech (to jeden
wspólny węzeł masy, więc nic nie szkodzi).

Pushbutton ma 4 nóżki: `1.l`, `2.l`, `1.r`, `2.r`. **Uwaga, pułapka**: `1.l` i `1.r` (ta sama
liczba, różna litera) to w rzeczywistości ta sama, zawsze zwarta nóżka fizycznego przycisku —
podłączenie do obu nie da żadnego przełączania, bo obwód jest zawsze zamknięty! Trzeba wziąć
dwie nóżki o **różnych liczbach**, np. `1.l` + `2.l` (albo `1.r` + `2.r`, albo `1.l` + `2.r`) —
to są dwa faktycznie rozłączane styki. Sam to na własnej skórze przetestowałem: podłączenie
`1.l`+`1.r` dawało diodę zapaloną na stałe niezależnie od przycisku i MQTT. Dioda LED ma piny
`A` (anoda) i `C` (katoda).

## Podłączenie (pinout)

| Element | Pin ESP32 | Uwagi |
|---|---|---|
| OLED SSD1306 SDA | GPIO21 | I2C |
| OLED SSD1306 SCL | GPIO22 | I2C |
| Potencjometr (symuluje czujnik wilgotności) | GPIO34 | wejście ADC |
| Czujnik PIR (ruch/kot) | GPIO27 | wejście cyfrowe |
| Przekaźnik — pompka do podlewania | GPIO26 | wyjście cyfrowe |
| Przekaźnik — "pistolet" na kota | GPIO25 | wyjście cyfrowe |
| Przycisk fizyczny (obok ekranu) | GPIO32 | wejście, `INPUT_PULLUP` |
| Dioda LED (zapalana przyciskiem) | GPIO33 | wyjście, przez rezystor 220Ω |

Na prawdziwym sprzęcie: piny sterujące (26, 25) podłącza się do wejścia `IN` modułu przekaźnika,
a przekaźnik dopiero przełącza realne zasilanie pompek — ESP32 nie zasila pompek bezpośrednio.

## Logika działania

- Wilgotność gleby **≥60%** → zielony, status "OK", pompka wyłączona.
- **30–59%** → żółty, status "Podlej!", **pompka się włącza**.
- **<30%** → czerwony, "Za sucho!", pompka też leci (bo jest gorzej niż żółty).
- Pompka gasi się, gdy wilgotność wróci do ~65% (mały histerezys, żeby nie migała) albo po 2s
  bezpiecznika czasowego (do zmiany w kodzie: `WATER_PUMP_MS`).
- PIR wykrywa ruch → 1,5s "psiknięcia" z drugiego przekaźnika + licznik trafień, z 8s odstępem
  między strzałami (`CAT_COOLDOWN_MS`), żeby nie strzelać seriami do tego samego kota.
- Licznik ustrzelonych kotów jest **zapisywany do pamięci flash (Preferences/NVS)**, więc przetrwa
  restart ESP32 — na realnym sprzęcie to się przyda.
- Przycisk (GPIO32) działa jak zwykły włącznik światła: trzymasz → dioda (GPIO33) świeci; puszczasz
  → gaśnie. Bez zapadki/przełącznika — do zmiany w `handleButtonLed()`, jeśli wolisz inne zachowanie.
- Co 5s wysyłana jest publikacja MQTT z aktualną wilgotnością, statusem i licznikiem kotów.

## Ekran

- Buźka narysowana wektorowo (nie bitmapa) — uśmiech / neutralna / smutna, zależnie od statusu.
- Duży procent wilgotności obok buźki.
- Tekstowy status ("OK" / "Podlej!" / "Za sucho!").
- Czas od ostatniego podlewania (sekundy/minuty/godziny temu).
- Licznik ustrzelonych kotów, z chwilowym "*PSIK*" w trakcie strzału.

## Co zmienić pod prawdziwy sprzęt

Masz fizycznie ESP32 + wyświetlacz — kod jest identyczny dla symulacji i prawdziwego sprzętu
(to standardowy Arduino/ESP32 core), zmienia się tylko to, co podłączasz pod GPIO34/27/26/25:
- zamiast potencjometru — prawdziwy pojemnościowy czujnik wilgotności gleby (analogowy, 3.3V),
  ewentualnie trzeba będzie odwrócić/przeskalować `map()` w `readMoisturePercent()`, bo różne
  czujniki dają odwrotną charakterystykę (mokro = niższe napięcie).
- zamiast wirtualnego PIR — prawdziwy HC-SR501 (te same 3 nóżki: VCC/OUT/GND).
- przekaźniki 5V steruj przez GPIO ESP32 (3.3V) — większość modułów przekaźnikowych ma to
  obsłużone na module, ale sprawdź datasheet swojego modułu.
