# WiFiManagerESP

Umfassender WLAN-Manager für ESP32 mit Captive-Portal, OTA (Firmware + LittleFS), Web-UI und automatischer Fallback-Logik. Provisionierung erfolgt wahlweise per SoftAP/Captive-Portal oder über die STA-UI, Daten liegen in LittleFS. OTA wird über ElegantOTA bereitgestellt.

## Features
- **Captive Portal (AP + DNS-Catch-All)** für Erstkonfiguration / Re-Provisionierung.
- **STA-Modus** mit DHCP oder statischer IP, DNS, Hostname, NTP.
- **Web-UI** (LittleFS, `data/*.html`, `WifiManager.css`) und **REST-API** für STA-Config (`/api/sta/config`).
- **OTA-Updates**:
  - Firmware OTA via `App-OTA.ps1` (espota.py, WLAN).
  - LittleFS seriell via `LittleFS.ps1` oder OTA via `LittleFS-OTA.ps1`.
  - ElegantOTA unter `/update` (AP + STA-Server).
- **WiFi-Scan** (`/scan`, `/scan.json`).
- **Fallback-Strategien**: ON_FAIL, NO_CREDENTIALS_ONLY, SMART_RETRIES, BUTTON_ONLY, NEVER.
- **Auto-Reconnect** mit Backoff/Fenster; optionale Internetprüfung (HTTP 204).
- **LED-/Button-Handling**, **mDNS** (`<hostname>.local`), **NTP**.

## Projektstruktur (relevant)
- `WiFiManagerESP.ino` — Beispiel-Sketch (Startlogik, LED/Button, OTA-Init).
- `src/ESPWiFiManagerAP.*` — AP/Captive-Portal, Scan, OTA, mDNS, Fallback.
- `src/ESPWiFiManagerSTA.*` — STA-Server, `/api/sta/config`, optional Captive-Redirect.
- `src/ESPWiFiManagerCommon.*` — Utilities (Credentials, `config.json`, NTP, File-Streaming).
- `data/` — Web-Assets (LittleFS-Inhalt: `indexAP.html`, `indexSTA.html`, `settings.html`, `success.html`, `error.html`, `WifiManager.css`).
- `LittleFS.ps1`, `LittleFS-OTA.ps1`, `App-OTA.ps1` — Flash/OTA-Skripte.

## Abhängigkeiten
- Arduino Core ESP32 (3.x empfohlen).
- Bibliotheken: ArduinoJson, ESPAsyncWebServer, AsyncTCP, ElegantOTA, LittleFS (im Core enthalten).
- Tools: `espota.py` (kommt mit Core), `mklittlefs`.

## Build (Visual Studio + vMicro)
1. Board/FQBN wählen (z. B. ESP32-WROOM-DA).  
2. Partitionen: benötigt 2 OTA-Slots + 1 DATA (LittleFS). Werte in `LittleFS.ps1`/`partitions.csv` prüfen (Offset/Size).  
3. Abhängigkeiten installieren (s.o.).  
4. LittleFS-Inhalt hochladen (seriell) oder per Skript/OTA (siehe unten).  
5. Sketch build/flash über vMicro (USB) oder OTA.

## Flash-/OTA-Skripte
- LittleFS seriell:  
  `.\LittleFS.ps1 -Port COM4`  (Port bei Bedarf anpassen)
- LittleFS OTA:  
  `.\LittleFS-OTA.ps1 -Ip <ESP-IP> [-Port 3232] [-Password <ota-pass>]`
- Firmware OTA:  
  `.\App-OTA.ps1 -Ip <ESP-IP> -Bin .\build\firmware.bin [-Port 3232] [-Password <ota-pass>]`

## Laufzeitverhalten (Beispiel-Sketch)
- Boot: versucht STA-Verbindung mit gespeicherten Credentials.
- Fallback: öffnet Captive-Portal (AP + DNS-Catch-All) bei Fehlschlag.
- Button (`BTN_PIN`, LOW-aktiv):  
  - Kurzer Druck (~≥700 ms): Portal öffnen.  
  - 2–5 s halten: Portal öffnen (Sketch-Logik).  
  - ≥5 s halten: Credentials löschen, Neustart.
- LED (`LED_PIN`): OFF/ON/BLINK_SLOW/BLINK_FAST (Sketch); interne AWM-LED-Patterns für Portal/Scan/Status.

## HTTP-Routen (Auszug)
- AP: `/`, `/settings`, `/scan`, `/scan.json`, `/save` (POST), `/erase` (POST), `/WifiManager.css`
- STA: `/`, `/settings`, `/api/sta/config` (GET/POST), `/erase` (POST), `/WifiManager.css`
- Captive-Redirects: `/generate_204`, `/gen_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`, `/fwlink`
- OTA: `/update` (ElegantOTA)

## Persistenz
- `config.json`/NVS: SSID, Passwort, DHCP/static (IP/GW/SN/DNS), Hostname, NTP.  
- Geschützte JSONs können via `setProtectedJsons({...})` von Löschroutinen ausgenommen werden.

## Hardware-Annahmen
- LED: `LED_PIN` (Standard GPIO2).
- Taster: `BTN_PIN` (Standard GPIO0, LOW-aktiv, -1 zum Deaktivieren).
- Board: ESP32; Sleep ist deaktiviert, Power-Save aus.

## Hinweise zu Partitionen
- LittleFS-Offset/-Size müssen zu `partitions.csv` passen (siehe `LittleFS.ps1`: `offset`, `siz`).  
- Für OTA werden zwei App-Slots benötigt; sicherstellen, dass das Partitionsschema das vorsieht.

## Lizenz
MIT (siehe SPDX-Header in den Quelldateien).