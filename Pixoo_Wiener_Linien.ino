// Wiener Linien Abfahrten -> Divoom Pixoo 16x16 (Bluetooth Classic) mit ESP32
// Schrift 3x5 Pixel, 2 Zeilen, kein Lauftext.
// Jede Seite = eine Linie (Linie + Richtung), beide Zeilen zeigen deren naechste 2 Abfahrten.
// Mehrere Linien/RBLs werden im Wechsel als Seiten angezeigt.
// Board: ESP32 Dev Module (klassischer ESP32), Partition Scheme: Huge APP
// Bibliotheken: ArduinoJson (Benoit Blanchon, 6.21.x), ESP32-Core (BluetoothSerial, WiFi, HTTPClient)

#include "BluetoothSerial.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>

// ===== Einstellungen =====
// WLAN-Zugangsdaten stehen in secrets.h (nicht im Repository). Vorlage: secrets.example.h
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h fehlt: secrets.example.h nach secrets.h kopieren und WLAN-Daten eintragen."
#endif

// RBL(s), siehe https://till.mabe.at/rbl/
// Eine oder mehrere, kommagetrennt: "567" oder "567,4932,156"
const char* RBL = "567,4932,156";
// Nur diese Linien anzeigen, kommagetrennt, z.B. "D,2,U2Z". Leer = alle.
const char* LINE_FILTER = "";
// Seitenwechsel (naechste Linie) alle 5 s
const unsigned long PAGE_MS = 5000;
// Daten alle 30 s neu laden
const unsigned long REFRESH_MS = 30000;
// Neustart alle 3 Stunden (haelt den Speicher sauber), 0 = aus
const unsigned long REBOOT_MS = 3UL * 60 * 60 * 1000;
// Sicherheitsnetz: Ist nach dem Bluetooth-Neustart weniger RAM frei, wird neu gestartet
const unsigned MIN_HEAP_AFTER_BT = 20000;

const uint8_t COLOR[3] = {255, 160, 0};   // R, G, B der Schrift
// Das Rampensymbol (Unterstrich) wird bis einschliesslich dieser Minute angezeigt
const int RAMP_MAX_MINUTES = 19;

// ===== Nachtmodus =====
// Gibt es in diesem Zeitfenster keine Abfahrten (die Anzeige waere "NO DATA"), geht die Anzeige aus,
// und nur ein Pixel links unten pulsiert langsam. Waehrend dieser Zeit wird nicht abgefragt.
// Ab dem Ende des Fensters wird wieder abgefragt; sind Daten da, werden sie angezeigt.
// Ausserhalb des Fensters bleibt "NO DATA" wie gehabt.
const int NIGHT_START_HOUR   = 0;    // Beginn: 0:00 Uhr
const int NIGHT_START_MINUTE = 0;
const int NIGHT_END_HOUR     = 4;    // Ende: 4:30 Uhr
const int NIGHT_END_MINUTE   = 30;
// Zeitzone fuer die Uhrzeit aus dem Internet (NTP); dieser Wert gilt fuer Wien inkl. Sommerzeit
const char* TIME_ZONE = "CET-1CEST,M3.5.0,M10.5.0/3";
// Pulsierender Pixel: Dauer eines Zyklus, Helligkeit in Prozent der Schriftfarbe, Bildabstand
const unsigned long PULSE_PERIOD_MS = 4000;
const int PULSE_MIN_PERCENT = 2;
const int PULSE_MAX_PERCENT = 30;
const unsigned long PULSE_STEP_MS = 100;

// Der Server leitet http:// auf https:// um (Status 302), daher HTTPS.
const char* URL_SCHEME = "https://";

// Die RBLs werden im Code als "&rbl=..." angehaengt (je RBL ein Parameter)
#define URL_BASE \
  "www.wienerlinien.at/ogd_realtime/monitor?" \
  "activateTrafficInfo=stoerungkurz&activateTrafficInfo=stoerunglang"

BluetoothSerial SerialBT;

// Forward-Deklaration: die Arduino-IDE erzeugt die Funktionsprototypen automatisch
// VOR der ersten Funktion, und die verwenden "Monitor" schon.
struct Monitor;

// ===== 3x5-Schrift: je Zeichen 5 Zeilen (oben nach unten), Bit 4 = links, Bit 1 = rechts =====
static const uint8_t DIGITS[10][5] = {
  {7,5,5,5,7},{2,6,2,2,2},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
  {7,4,7,1,7},{7,4,7,5,7},{7,1,2,4,4},{7,5,7,5,7},{7,5,7,1,7}
};
static const uint8_t LETTERS[26][5] = {
  {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},{3,4,5,5,3},
  {5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},{6,5,5,5,5},
  {2,5,5,5,2},{6,5,6,4,4},{2,5,5,7,3},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},{5,5,5,5,7},
  {5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},{7,1,2,4,7}
};
static const uint8_t GLYPH_DASH[5]  = {0,0,7,0,0};
static const uint8_t GLYPH_BLANK[5] = {0,0,0,0,0};
static const uint8_t GLYPH_QUEST[5] = {7,1,2,0,2};
static const uint8_t GLYPH_UNDERSCORE[5] = {0,0,0,0,6};   // '_' : 2 Pixel breit, unterste Zeile

const uint8_t* glyph(char c) {
  if (c >= '0' && c <= '9') return DIGITS[c - '0'];
  if (c >= 'A' && c <= 'Z') return LETTERS[c - 'A'];
  if (c == '-') return GLYPH_DASH;
  if (c == ' ') return GLYPH_BLANK;
  if (c == '_') return GLYPH_UNDERSCORE;
  return GLYPH_QUEST;
}

// ===== Bildspeicher 16x16, 1 Bit pro Pixel =====
uint8_t fb[32];

void clearFb() { memset(fb, 0, sizeof(fb)); }

void setPx(int x, int y) {
  if (x < 0 || x > 15 || y < 0 || y > 15) return;
  int idx = y * 16 + x;
  fb[idx >> 3] |= (1 << (idx & 7));
}

// Breite eines Zeichens in Pixeln (ohne Luecke). Die "1" ist nur 2 Pixel breit,
// damit zwischen ihr und den Nachbarzeichen genau 1 Pixel frei bleibt. Der Unterstrich ebenfalls.
int glyphWidth(char c) { return (c == '1' || c == '_') ? 2 : 3; }

// Gesamtbreite eines Textes: Zeichenbreiten + 1 Pixel Luecke zwischen den Zeichen
int textWidthPx(const String& s) {
  int w = 0;
  for (size_t i = 0; i < s.length(); i++) {
    w += glyphWidth(s[i]);
    if (i + 1 < s.length()) w += 1;
  }
  return w;
}

void drawText(int x, int y, const String& s) {
  int cx = x;
  for (size_t i = 0; i < s.length(); i++) {
    const uint8_t* g = glyph(s[i]);
    int gw = glyphWidth(s[i]);
    for (int r = 0; r < 5; r++) {
      for (int c = 0; c < gw; c++) {
        if (g[r] & (4 >> c)) setPx(cx + c, y + r);
      }
    }
    cx += gw + 1;
  }
}

// Symbole fuer "0 Minuten" (je 3x5 Pixel, 1 = leuchtet)
static const uint8_t SYMBOL_TOP[5]    = {7,7,7,0,0};   // Symbol 1: obere 3 Zeilen
static const uint8_t SYMBOL_BOTTOM[5] = {0,0,7,7,7};   // Symbol 2: untere 3 Zeilen

void drawBlock(int x, int y, const uint8_t* rows) {
  for (int r = 0; r < 5; r++) {
    for (int c = 0; c < 3; c++) {
      if (rows[r] & (4 >> c)) setPx(x + c, y + r);
    }
  }
}

// Erste leuchtende Spalte eines Textes in der untersten Pixelzeile (x = linke Kante), 99 wenn leer.
// Der Rampen-Unterstrich liegt nur in dieser Zeile; bei 10-19 Minuten leuchtet die "1" dort erst
// in ihrer zweiten Spalte, daher geht sich der Unterstrich noch aus.
int firstLitColBottom(int x, const String& s) {
  int cx = x;
  for (size_t i = 0; i < s.length(); i++) {
    const uint8_t* g = glyph(s[i]);
    int gw = glyphWidth(s[i]);
    for (int c = 0; c < gw; c++) {
      if (g[4] & (4 >> c)) return cx + c;
    }
    cx += gw + 1;
  }
  return 99;
}

// Wird in drawRow() gesetzt, wenn auf der aktuellen Seite ein Symbol statt "0" steht.
// Dann muss das Bild im Sekundentakt neu gezeichnet werden.
bool zeroShown = false;

// ===== Datenmodell =====
struct Departure {
  int minutes;     // Minuten bis zur Abfahrt
  bool ramp;       // U-Bahn (vehicle.type == ptMetro) mit Klapprampe (vehicle.foldingRamp == true)
  String towards;  // Fahrtziel dieser Abfahrt (nur fuer die Ausgabe im Serial Monitor)
};

struct Monitor {
  String name;                  // Linie: 2, 72, D, U1 ...
  String richtungsId;           // ID der Richtung: Linienname + richtungsId = eine Seite
  std::vector<Departure> deps;  // Abfahrten (aufsteigend nach Minuten)
};

std::vector<Monitor> monitors;
unsigned long lastFetch = 0;
bool dataLoaded = false;   // mindestens ein Abruf war erfolgreich
bool nightSleep = false;   // Nachtmodus aktiv

// Eine Zeile: Linie links, Minuten rechtsbuendig. Zu lange Liniennamen werden gekuerzt.
void drawRow(const String& lineName, int minutes, bool ramp, int y) {
  if (minutes > 99) minutes = 99;
  if (minutes < 0) minutes = 0;

  int cdX;
  int bottomStart;   // erste leuchtende Spalte der Minuten in der untersten Pixelzeile
  if (minutes == 0) {
    // 0 Minuten: statt "0" ein Quadrat, das im Sekundentakt zwischen rechts oben (Symbol 1)
    // und links unten (Symbol 2) springt. Das Feld ist 5 Pixel breit; die beiden Positionen
    // ueberlappen in einer Spalte.
    cdX = 16 - 5;
    if ((millis() / 1000) % 2 == 0) drawBlock(cdX + 2, y, SYMBOL_TOP);
    else drawBlock(cdX, y, SYMBOL_BOTTOM);
    bottomStart = cdX;   // vorsichtig: im Takt mit dem unteren Quadrat leuchtet dort die erste Spalte
    zeroShown = true;
  } else {
    String cd = String(minutes);
    cdX = 16 - textWidthPx(cd);
    if (minutes == 1) cdX -= 1;   // genau 1 Minute (nur dann): ganze Zahl 1 Pixel nach links
   // if (minutes == 11) cdX -= 1;   // OPTIONAL genau 11 Minuten (nur dann): ganze Zahl 1 Pixel nach links
    drawText(cdX, y, cd);
    bottomStart = firstLitColBottom(cdX, cd);
  }

  String name = lineName;
  name.toUpperCase();
  // Zu lange Namen kuerzen: mindestens 1 leere Spalte bis zu den Minuten
  while (name.length() > 0 && textWidthPx(name) > cdX - 1) name.remove(name.length() - 1);

  // Klapprampe: Unterstrich nach dem Namen (1 Pixel Luecke), nur bis RAMP_MAX_MINUTES, wenn der
  // Name nicht gekuerzt wurde und in der untersten Pixelzeile noch 1 Spalte bis zu den Minuten frei bleibt.
  if (ramp && minutes <= RAMP_MAX_MINUTES && name.length() == lineName.length()) {
    String withRamp = name + "_";
    if (textWidthPx(withRamp) <= bottomStart - 1) name = withRamp;
  }
  drawText(0, y, name);
}

// ===== Divoom-Paket: 01 | len(2,LE) | payload | checksum(2,LE) | 02 =====
size_t buildPacket(const uint8_t* payload, uint16_t n, uint8_t* out) {
  uint16_t len = n + 2;
  uint16_t sum = (len & 0xFF) + (len >> 8);
  size_t i = 0;
  out[i++] = 0x01;
  out[i++] = len & 0xFF;
  out[i++] = len >> 8;
  for (uint16_t k = 0; k < n; k++) { out[i++] = payload[k]; sum += payload[k]; }
  out[i++] = sum & 0xFF;
  out[i++] = sum >> 8;
  out[i++] = 0x02;
  return i;
}

void sendCmd(const uint8_t* payload, uint16_t n) {
  uint8_t pkt[128];
  size_t len = buildPacket(payload, n, pkt);
  SerialBT.write(pkt, len);
}

// Bildspeicher als 16x16-Bild senden: Palette mit 2 Farben (schwarz + Schriftfarbe)
void sendFbScaled(int percent) {
  uint8_t p[50];
  int i = 0;
  p[i++] = 0x44; p[i++] = 0x00; p[i++] = 0x0A; p[i++] = 0x0A; p[i++] = 0x04;  // Bild senden
  p[i++] = 0xAA; p[i++] = 45;   p[i++] = 0;                                   // Frame-Header + Laenge
  p[i++] = 0;    p[i++] = 0;                                                  // Frame-Zeit
  p[i++] = 0;                                                                 // Palette zuruecksetzen
  p[i++] = 2;                                                                 // 2 Farben
  p[i++] = 0;    p[i++] = 0;    p[i++] = 0;                                   // Farbe 0: schwarz
  p[i++] = COLOR[0] * percent / 100;                                          // Farbe 1: Schrift,
  p[i++] = COLOR[1] * percent / 100;                                          // skaliert mit der
  p[i++] = COLOR[2] * percent / 100;                                          // Helligkeit in %
  memcpy(&p[i], fb, 32); i += 32;
  sendCmd(p, i);
}

void sendFb() { sendFbScaled(100); }

void connectPixoo() {
  Serial.println("Verbinde mit Pixoo...");
  while (!SerialBT.connect("Pixoo")) {
    Serial.println("  fehlgeschlagen, neuer Versuch...");
    delay(2000);
  }
  Serial.println("Verbunden!");
}

// Der Bluetooth-Stack belegt ca. 100 KB RAM, dann reicht der Speicher nicht fuer HTTPS (TLS).
// Darum wird er fuer jeden Datenabruf kurz abgeschaltet und danach neu gestartet.
void btDown() {
  SerialBT.end();
  delay(300);
  Serial.printf("Bluetooth aus, Heap frei: %u\n", (unsigned)ESP.getFreeHeap());
}

void btUp() {
  SerialBT.begin("ESP32-Master", true);
  unsigned freeHeap = (unsigned)ESP.getFreeHeap();
  Serial.printf("Bluetooth an, Heap frei: %u\n", freeHeap);
  if (freeHeap < MIN_HEAP_AFTER_BT) {
    Serial.println("Zu wenig RAM nach Bluetooth-Start, Neustart.");
    delay(200);
    ESP.restart();
  }
  connectPixoo();
}

// ===== Seiten: eine Seite = eine Linie =====
int pageCount() {
  int n = (int)monitors.size();
  return n > 0 ? n : 1;
}

void drawPage(int page, bool log) {
  clearFb();
  zeroShown = false;
  int n = (int)monitors.size();

  if (n == 0) {
    drawText(0, 2, "NO");
    drawText(0, 9, "DATA");
    if (log) Serial.println("Anzeige: NO DATA");
    sendFb();
    return;
  }

  const Monitor& m = monitors[page % n];
  if (m.deps.size() >= 2) {
    drawRow(m.name, m.deps[0].minutes, m.deps[0].ramp, 2);    // naechste Abfahrt
    drawRow(m.name, m.deps[1].minutes, m.deps[1].ramp, 9);    // uebernaechste Abfahrt
    if (log) Serial.printf("Seite %d/%d: Linie %s (Richtung %s): %d%s -> %s, %d%s -> %s\n", (page % n) + 1, n,
                           m.name.c_str(), m.richtungsId.c_str(),
                           m.deps[0].minutes, m.deps[0].ramp ? " (Rampe)" : "", m.deps[0].towards.c_str(),
                           m.deps[1].minutes, m.deps[1].ramp ? " (Rampe)" : "", m.deps[1].towards.c_str());
  } else {
    drawRow(m.name, m.deps[0].minutes, m.deps[0].ramp, 5);    // nur eine Abfahrt bekannt: mittig
    if (log) Serial.printf("Seite %d/%d: Linie %s (Richtung %s): %d%s -> %s\n", (page % n) + 1, n,
                           m.name.c_str(), m.richtungsId.c_str(),
                           m.deps[0].minutes, m.deps[0].ramp ? " (Rampe)" : "", m.deps[0].towards.c_str());
  }
  sendFb();
}

// ===== Hilfsfunktionen =====
String toLatin(String s) {
  s.replace("ä", "a");
  s.replace("Ä", "A");
  s.replace("ö", "o");
  s.replace("Ö", "O");
  s.replace("ü", "u");
  s.replace("Ü", "U");
  s.replace("ß", "ss");
  return s;
}

std::vector<String> splitStrings(String data, char separator) {
  std::vector<String> result;
  int idx = 0;
  while (idx != -1) {
    idx = data.indexOf(separator);
    String chunk = data.substring(0, idx);
    chunk.trim();
    result.push_back(chunk);
    if (idx != -1) data = data.substring(idx + 1);
  }
  return result;
}

bool lineAllowed(const String& name, const std::vector<String>& allowed) {
  if (allowed.empty()) return true;
  for (const auto& a : allowed) {
    if (a.equalsIgnoreCase(name)) return true;
  }
  return false;
}

// ===== WLAN =====
bool ensureWifi(unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

// ===== Abruf + Parsen =====
bool fetchMonitors(std::vector<Monitor>& out) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  HTTPClient http;
  String url = String(URL_SCHEME) + URL_BASE;
  for (const auto& r : splitStrings(String(RBL), ',')) {
    if (r.length() > 0) url += "&rbl=" + r;
  }
  Serial.println(url);

  bool begun;
  if (url.startsWith("https://")) {
    secureClient.setInsecure();   // keine Zertifikatspruefung (fuer ein Display vertretbar)
    begun = http.begin(secureClient, url);
  } else {
    begun = http.begin(plainClient, url);
  }
  if (!begun) return false;
  http.useHTTP10(true);   // kein Chunked-Encoding -> Stream direkt parsbar
  http.setTimeout(8000);

  int code = http.GET();
  Serial.printf("HTTP-Status: %d", code);
  if (code < 0) Serial.printf(" (%s)", HTTPClient::errorToString(code).c_str());
  Serial.println();
  if (code >= 300 && code < 400) Serial.println("Umleitung: der Server verlangt wohl HTTPS.");
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  // Nur die benoetigten Felder behalten, spart viel Speicher
  StaticJsonDocument<1024> filter;
  filter["data"]["monitors"][0]["lines"][0]["name"] = true;
  filter["data"]["monitors"][0]["lines"][0]["towards"] = true;
  filter["data"]["monitors"][0]["lines"][0]["richtungsId"] = true;
  filter["data"]["monitors"][0]["lines"][0]["departures"]["departure"][0]["departureTime"]["countdown"] = true;
  filter["data"]["monitors"][0]["lines"][0]["departures"]["departure"][0]["vehicle"]["name"] = true;
  filter["data"]["monitors"][0]["lines"][0]["departures"]["departure"][0]["vehicle"]["towards"] = true;
  filter["data"]["monitors"][0]["lines"][0]["departures"]["departure"][0]["vehicle"]["type"] = true;
  filter["data"]["monitors"][0]["lines"][0]["departures"]["departure"][0]["vehicle"]["foldingRamp"] = true;
  filter["data"]["monitors"][0]["lines"][0]["departures"]["departure"][0]["vehicle"]["richtungsId"] = true;

  // Durch den Filter bleiben nur wenige KB uebrig; kleiner Puffer schont den RAM waehrend TLS.
  // Bei "JSON-Fehler: NoMemory" diesen Wert erhoehen.
  DynamicJsonDocument doc(10 * 1024);
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter),
                                             DeserializationOption::NestingLimit(64));
  http.end();
  if (err) {
    Serial.print("JSON-Fehler: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonObject data = doc["data"];

  // Abfahrten, zusammengefasst nach Linie + Richtung (Reihenfolge wie in der Antwort = RBL-Reihenfolge)
  for (JsonObject mon : data["monitors"].as<JsonArray>()) {
    for (JsonObject line : mon["lines"].as<JsonArray>()) {
      for (JsonObject dep : line["departures"]["departure"].as<JsonArray>()) {
        int countdown = dep["departureTime"]["countdown"] | -1;
        if (countdown < 0) continue;

        String name, towards, richtungsId;
        bool ramp = false;
        JsonObject veh = dep["vehicle"];
        if (!veh.isNull()) {
          name    = String(veh["name"] | "");
          towards = String(veh["towards"] | "");
          richtungsId = String(veh["richtungsId"] | "");
          // Nur bei U-Bahn (ptMetro) zaehlt die Klapprampe
          const char* type = veh["type"] | "";
          if (strcmp(type, "ptMetro") == 0) {
            JsonVariant fr = veh["foldingRamp"];
            if (fr.is<bool>()) {
              ramp = fr.as<bool>();
            } else if (fr.is<const char*>()) {
              String s = fr.as<const char*>();
              s.toLowerCase();
              ramp = (s == "true");
            }
          }
        } else {
          name    = String(line["name"] | "");
          towards = String(line["towards"] | "");
        }
        towards = toLatin(towards);

        // Eine Seite = Linienname + richtungsId. Verschiedene Fahrtziele (towards) derselben
        // Richtung gehoeren zur selben Linie. Fehlt die richtungsId am Fahrzeug, gilt die der Linie;
        // fehlt sie ganz, wird ersatzweise das Fahrtziel verwendet.
        if (richtungsId.length() == 0) richtungsId = String(line["richtungsId"] | "");
        if (richtungsId.length() == 0) richtungsId = towards;

        auto it = std::find_if(out.begin(), out.end(), [&](const Monitor& m) {
          return m.name == name && m.richtungsId == richtungsId;
        });
        if (it == out.end()) {
          Monitor m;
          m.name = name;
          m.richtungsId = richtungsId;
          m.deps.push_back({countdown, ramp, towards});
          out.push_back(m);
        } else {
          it->deps.push_back({countdown, ramp, towards});
        }
      }
    }
  }

  // Filter nach Linien; Abfahrten je Linie aufsteigend sortieren
  std::vector<String> allowed;
  if (strlen(LINE_FILTER) > 0) allowed = splitStrings(String(LINE_FILTER), ',');
  std::vector<Monitor> filtered;
  for (auto& m : out) {
    if (lineAllowed(m.name, allowed)) {
      std::sort(m.deps.begin(), m.deps.end(), [](const Departure& a, const Departure& b) {
        return a.minutes < b.minutes;
      });
      filtered.push_back(m);
    }
  }

  out = filtered;
  return true;
}

void refreshData() {
  lastFetch = millis();
  if (!ensureWifi(10000)) { Serial.println("Kein WLAN, alte Daten bleiben."); return; }

  Serial.printf("Heap vor Abruf: frei %u, groesster Block %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  std::vector<Monitor> fresh;
  if (fetchMonitors(fresh)) {
    monitors = fresh;
    dataLoaded = true;
    Serial.printf("%d Linien/Richtungen geladen\n", (int)monitors.size());
  } else {
    Serial.println("Abruf fehlgeschlagen, alte Daten bleiben.");
  }
  Serial.printf("Freier Heap: %u\n", ESP.getFreeHeap());
}

// ===== Nachtmodus =====
// Uhrzeit per NTP holen (SNTP laeuft danach im Hintergrund weiter)
void syncClock() {
  configTzTime(TIME_ZONE, "pool.ntp.org", "time.google.com");
  struct tm t;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&t, 500)) {
      Serial.printf("Uhrzeit: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
      return;
    }
  }
  Serial.println("Uhrzeit nicht verfuegbar (NTP). Der Nachtmodus bleibt aus, bis sie da ist.");
}

bool inNightWindow() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;   // Uhrzeit unbekannt: nie abschalten
  int now  = t.tm_hour * 60 + t.tm_min;
  int from = NIGHT_START_HOUR * 60 + NIGHT_START_MINUTE;
  int to   = NIGHT_END_HOUR * 60 + NIGHT_END_MINUTE;
  if (from <= to) return now >= from && now < to;
  return now >= from || now < to;           // Fenster reicht ueber Mitternacht
}

// Nachtmodus: im Zeitfenster und es gibt (nach erfolgreichem Abruf) keine Abfahrten
bool shouldSleep() {
  return dataLoaded && monitors.empty() && inNightWindow();
}

// Ein Pixel links unten pulsiert langsam, der Rest der Anzeige bleibt schwarz
void pulsePixel() {
  static unsigned long lastStep = 0;
  if (millis() - lastStep < PULSE_STEP_MS) return;
  lastStep = millis();
  float phase = (millis() % PULSE_PERIOD_MS) / (float)PULSE_PERIOD_MS;   // 0..1
  float wave = 0.5f - 0.5f * cosf(2.0f * PI * phase);                    // 0..1, weich
  int percent = PULSE_MIN_PERCENT + (int)((PULSE_MAX_PERCENT - PULSE_MIN_PERCENT) * wave);
  clearFb();
  setPx(0, 15);
  sendFbScaled(percent);
}

void setup() {
  Serial.begin(115200);

  // Erst WLAN + erster Abruf (dann ist noch viel Speicher frei), danach Bluetooth
  while (!ensureWifi(15000)) Serial.println("WLAN-Verbindung fehlgeschlagen, neuer Versuch...");
  Serial.println("WLAN ok");
  syncClock();
  refreshData();

  SerialBT.begin("ESP32-Master", true);
  connectPixoo();
}

void loop() {
  static int page = 0;
  static unsigned long lastPage = 0;
  static bool redraw = true;
  static bool log = true;
  static int lastPhase = -1;

  if (!SerialBT.connected()) { connectPixoo(); redraw = true; log = true; }
  if (REBOOT_MS > 0 && millis() > REBOOT_MS) ESP.restart();

  // Nachtmodus: keine Abfahrten im Nachtfenster -> Anzeige aus, ein Pixel pulsiert, kein Abruf
  if (shouldSleep()) {
    if (!nightSleep) {
      nightSleep = true;
      Serial.println("Nachtmodus: keine Abfahrten, Anzeige aus.");
    }
    pulsePixel();
    while (SerialBT.available()) SerialBT.read();   // Antworten verwerfen
    delay(20);
    return;
  }
  if (nightSleep) {
    nightSleep = false;
    redraw = true;
    log = true;
    Serial.println("Nachtmodus beendet, Abfragen laufen wieder.");
  }

  if (millis() - lastFetch > REFRESH_MS) {
    btDown();                 // RAM fuer TLS freigeben (Display behaelt das letzte Bild)
    refreshData();
    btUp();                   // Bluetooth neu starten und mit der Pixoo verbinden
    redraw = true;            // neue Daten sofort anzeigen
    log = true;
  }

  if (millis() - lastPage >= PAGE_MS) {
    lastPage = millis();
    if (pageCount() > 1) { page++; redraw = true; log = true; }
  }

  // Steht ein 0-Minuten-Symbol auf dem Display, wechselt es jede Sekunde: dann neu zeichnen
  int phaseNow = (millis() / 1000) % 2;
  if (phaseNow != lastPhase) {
    lastPhase = phaseNow;
    if (zeroShown) redraw = true;
  }

  if (redraw) {
    page %= pageCount();
    drawPage(page, log);
    redraw = false;
    log = false;
  }

  while (SerialBT.available()) SerialBT.read();   // Antworten verwerfen
  delay(50);
}
