# Pixoo Wiener Linien

Zeigt die Echtzeit-Abfahrten der Wiener Linien auf einem **Divoom Pixoo 16x16** an.
Ein ESP32 holt die Daten per WLAN und schickt die Bilder per Bluetooth Classic an den Pixoo.

Ich wollte immer schon das **Divoom Pixoo 16x16 Display** als Anzeige für andere Daten benutzen als jene welche durch die Hersteller App vorgegeben sind. Das Display soll auch Daten anzeigen wenn der PC (der die Bluetooth-Verbindung zum Display herstellt) gerade mal nicht läuft. Inspiriert durch dieses Projekt: https://github.com/coppermilk/wiener_linien_esp32_monitor wollte ich das mal selber versuchen.

Da nur maximal 2 Zeilen/Seite vernünftig angezeigt werden können, kann man nur Echtzeitdaten pro Linie und Station und Richtung anzeigen lassen. Es werden keine Stationsnamen des Ziels angezeigt, da diese dann Scrollen müssten was dann zu lange dauert.

Echtzeitdaten werden alle 30 Sekunden neu abgefragt.

Es gibt noch Potential für Verbesserungen:
- Die Bluetooth-Verbindung muss kurzfristig aus und wieder eingeschaltet werden damit genug Speicher für die Echtzeitdatenabfrage vorhanden ist. Eventuell kann das anders gelöst werden.
- Die Echtzeitdaten beinhalten auch die Störungsnachrichten welche aber nicht verwendet werden. Hier könnte man eine gestörte Abfahrt mit einer anderen Farbe hinterlegen oder auch Störungsnachrichten am Display anzeigen. Aufgrund der 16*16 Pixel gibt es hier aber nicht viel Spielraum.
- Will man das Display einrichten (Linien hinzufügen oder Parameter anpassen), muss der Code verändert, kompiliert und hochgeladen werden. Hier wäre eine Einrichtung über das Handy per W-Lan eine gute Option.

Erstellt wurde das Projekt mit Claude (gratis Version) als Vibe-Coding Projekt, die Prompts stammen von mir :-)

Forks sind erwünscht, bin gespannt ob es Verbesserungen oder Weiterentwicklungen gibt.

<img width="300" height="400" alt="PXL_20261113_124121436" src="https://github.com/user-attachments/assets/058715c2-6e07-4d14-83e0-3b18dce91e07" /> <img width="300" height="400" alt="PXL_20261113_124726999" src="https://github.com/user-attachments/assets/229efc72-cad8-4af7-9515-37ef41b32cf8" />

Der ESP32:

<img width="389" height="291" alt="PXL_20261113_162735534 MACRO_FOCUS~2" src="https://github.com/user-attachments/assets/f91da6f9-0566-496d-85ae-eeb6cc13a9b5" />


## Anzeige

- Zwei Zeilen: links die Linie, rechts die Minuten bis zur Abfahrt. Die beiden Zeilen zeigen die **nächsten zwei Abfahrten derselben Linie**.
- Eine Seite entspricht einer Linie in einer Richtung (Linienname + `richtungsId`). Verschiedene Fahrtziele derselben Richtung werden zusammengefasst. Bei mehreren Linien wechselt die Seite alle 5 Sekunden.
- Bei **0 Minuten** erscheint statt der "0" ein Quadrat, das im Sekundentakt zwischen rechts oben und links unten springt.
- Bei **U-Bahnen mit Rampe** (`vehicle.type` = `ptMetro`, `foldingRamp` = `true`) steht hinter dem Liniennamen ein Unterstrich, bis einschließlich Minute 19.
- Gibt es keine Abfahrten, steht **NO DATA** auf dem Display.
- **Nachtmodus:** Gibt es im Nachtfenster (Standard 0:00 bis 4:30 Uhr) keine Abfahrten, geht die Anzeige aus, und ein einzelner Pixel links unten pulsiert langsam. Im Nachtmodus wird nicht abgefragt. Nach dem Ende des Fensters wird wieder abgefragt, und vorhandene Daten werden angezeigt. Außerhalb des Fensters bleibt NO DATA stehen.
- Die Schrift ist 3x5 Pixel groß und kennt Großbuchstaben, Ziffern, Leerzeichen, `-` und `_`. Zu lange Liniennamen werden gekürzt, wenn sonst kein Platz neben den Minuten bleibt (z. B. wird `13A` neben zweistelligen Minuten zu `13`). 4-Stellige Liniennamen wurden nicht getestet.

## Hardware

- **Divoom Pixoo 16x16** (Bluetooth Classic). Getestet nur mit diesem Modell.
- **ESP32 mit Bluetooth Classic**, z. B. ESP32-DevKitC V4 mit ESP32-WROOM-32.
- Nicht geeignet: ESP32-S2, -S3 und -C3 (kein Bluetooth Classic) sowie der Arduino Uno R4 WiFi (nur BLE). Die Pixoo 16x16 reagierte darüber bei den Tests nicht auf Steuerbefehle.

## Einrichtung

1. Arduino IDE installieren und im Boardverwalter das Paket **esp32** (Espressif Systems) installieren. In den Einstellungen unter "Zusätzliche Boardverwalter-URL's:" https://espressif.github.io/arduino-esp32/package_esp32_index.json eingeben.
2. Bibliothek **ArduinoJson** von Benoit Blanchon installieren (Version 6.x, entwickelt mit 6.21.x).
3. Unter *Werkzeuge* einstellen:
   - Board: **ESP32 Dev Module**
   - Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**
4. Im Sketch-Ordner `Pixoo_Wiener_Linien` die Datei `secrets.example.h` kopieren, als `secrets.h` speichern und WLAN-Namen und Passwort eintragen.
5. Oben in `Pixoo_Wiener_Linien.ino` die RBL-Nummern der Haltestellen eintragen. Passende Nummern findest du auf https://till.mabe.at/rbl/. Mehrere RBLs werden mit Komma getrennt, z. B. `"567,4932"`.
6. Die Pixoo einschalten und vom Handy trennen (Bluetooth am Handy ausschalten), damit die Verbindung nicht belegt ist. Sie muss unter dem Namen **Pixoo** gefunden werden.
7. Sketch hochladen und im Serial Monitor (115200 Baud) den Ablauf beobachten.

## Einstellungen

Alle Einstellungen stehen oben in `Pixoo_Wiener_Linien.ino`:

| Einstellung | Bedeutung |
|---|---|
| `RBL` | RBL-Nummern der Haltestellen, kommagetrennt |
| `LINE_FILTER` | Nur diese Linien anzeigen, z. B. `"D,2,U2Z"`. Leer = alle |
| `PAGE_MS` | Zeit pro Seite bei mehreren Linien (Standard 5 s) |
| `REFRESH_MS` | Abstand der Datenabrufe (Standard 30 s) |
| `REBOOT_MS` | Neustart nach dieser Zeit, `0` = aus (Standard 3 Stunden) |
| `COLOR` | Farbe der Schrift (R, G, B) |
| `RAMP_MAX_MINUTES` | Rampen-Unterstrich bis einschließlich dieser Minute |
| `NIGHT_START_*`, `NIGHT_END_*` | Beginn und Ende des Nachtfensters (Standard 0:00 bis 4:30) |
| `TIME_ZONE` | Zeitzone für die Uhrzeit aus dem Internet (Standard: Wien) |
| `PULSE_PERIOD_MS`, `PULSE_MIN_PERCENT`, `PULSE_MAX_PERCENT` | Dauer und Helligkeit des pulsierenden Pixels |

## Technische Hinweise

- **Bluetooth und HTTPS gleichzeitig:** Der Bluetooth-Stack belegt rund 100 KB RAM. Dann schlug der HTTPS-Abruf mit `HTTP-Status: -1 (connection refused)` fehl, weil für die TLS-Verbindung zu wenig Speicher frei blieb. Deshalb wird Bluetooth vor jedem Datenabruf beendet und danach neu gestartet und mit der Pixoo verbunden. Ist danach weniger als `MIN_HEAP_AFTER_BT` RAM frei, startet der ESP32 neu.
- **Protokoll:** Die Pixoo wird mit Divoom-Paketen angesteuert (`01 | Länge | Befehl und Daten | Prüfsumme | 02`). Das Bild wird als 16x16-Bild mit einer Palette aus zwei Farben gesendet. Der pulsierende Pixel ändert dazu die Palettenfarbe.
- **Datenabruf:** Der Sketch ruft die Echtzeitdaten der Wiener Linien (`ogd_realtime/monitor`) per HTTPS ab und liest nur die benötigten Felder aus (ArduinoJson-Filter).
- **Uhrzeit:** Die Uhrzeit kommt per NTP. Ist sie nicht verfügbar, bleibt der Nachtmodus aus.
- **Zertifikat:** Die HTTPS-Verbindung wird ohne Zertifikatsprüfung aufgebaut (`setInsecure()`). Für die Anzeige öffentlicher Fahrplandaten ist das vertretbar, für andere Zwecke nicht.

## Einschränkungen

- Nur mit dem Pixoo 16x16 und dem ESP32-DevKitC V4 getestet.
- Die WLAN-Zugangsdaten stehen in `secrets.h`. Es gibt kein WLAN-Konfigurationsportal.
- Während jedes Datenabrufs wird die Bluetooth-Verbindung kurz getrennt und danach wieder verbunden. Leider erscheint dann kurz das Bluetooth-Symbol am Display.
- Im Nachtmodus wird nicht abgefragt. Fährt in dieser Zeit doch etwas, erscheint es erst nach dem Ende des Nachtfensters.

## Datenquelle und Hinweis

Die Fahrplandaten stammen aus den Echtzeitdaten der Wiener Linien (Open Data). Bitte beachte deren Nutzungsbedingungen. Dieses Projekt steht in keiner Verbindung zu Divoom oder den Wiener Linien.
