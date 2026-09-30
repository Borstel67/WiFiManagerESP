// SPDX-License-Identifier: MIT
// Sketch für WiFiManagerESP (AP + STA + Captive Portal + OTA)
// Ablauf:
//  - Start: versucht STA-Verbindung mit gespeicherten Zugangsdaten
//  - Fallback: öffnet Captive-Portal (AP+DNS) zur Neukonfiguration
//  - LED und Taster werden vom AP-Manager gesteuert (Boot: 2–5 s Portal, ≥5 s Löschen);
//    im Betrieb öffnet ein Tastendruck ≥ SHORT_PRESS_MS das Portal
//  - Verbindungsverlust: nicht-blockierender Reconnect, nach LOST_PORTAL_MS Portal
//  - OTA über ElegantOTA (/update) und ArduinoOTA im AP- und STA-Modus verfügbar

#include <Arduino.h>
#ifndef ELEGANTOTA_USE_ASYNC_WEBSERVER
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#endif
#include <ElegantOTA.h>
#include <src/ESPWiFiManagerAP.h>
#include <src/ESPWiFiManagerSTA.h>
#include <src/ESPWiFiManagerCommon.h>

// ----- Hardware-Konfiguration -----
#ifndef LED_PIN
#define LED_PIN 2          // Status-LED (typisch GPIO2)
#endif

#ifndef BTN_PIN
#define BTN_PIN 0          // Provisioning-Taster, LOW-aktiv; -1 zum Deaktivieren
#endif

// ----- Timing / Netzwerk -----
static const char*    DEFAULT_HOSTNAME  = "ESPDevice";  // wird durch "hostname" aus der Config überschrieben
static const uint32_t SHORT_PRESS_MS    = 700;          // Tastendauer für Portal-Trigger
static const uint32_t LOST_PORTAL_MS    = 60000;        // Portal öffnen, wenn so lange keine Verbindung
static const uint32_t PORTAL_TIMEOUT_S  = 300;          // Portal schließen nach Inaktivität (nur mit Credentials)
static const uint16_t OTA_PORT          = 3232;

// ----- Manager-Instanzen -----
// Der AP-Manager wertet beim Boot immer einen Taster aus (Default GPIO0 = BOOT-Taste)
ESPWiFiManagerAP  wifiManagerAP(LED_PIN, BTN_PIN >= 0 ? BTN_PIN : 0);
ESPWiFiManagerSTA wifiManagerSTA;

// ----- Zustände -----
bool     otaReady      = false;
uint32_t disconnectedAt = 0;      // 0 = verbunden

#if (BTN_PIN >= 0)
bool     btnPrev      = true;
uint32_t btnPressedAt = 0;
#endif

// Gibt Zeit aus, falls NTP gesetzt ist; sonst Hinweis
void printTimeIfReady() {
	const time_t t = time(nullptr);
	struct tm tm_info;
	char buf[32];
	if (t > 100000 && localtime_r(&t, &tm_info) && strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info)) {
		Serial.print("Time: "); Serial.println(buf);
	} else {
		Serial.println("Zeit noch nicht gesetzt.");
	}
}

// ArduinoOTA einmalig starten, sobald ein Netz-Interface aktiv ist
static void ensureOtaReady() {
	if (otaReady) return;
	String h = WiFi.getHostname();
	if (!h.length()) h = DEFAULT_HOSTNAME;
	ensureArduinoOta(h, OTA_PORT, "");
	otaReady = true;
}

// Öffnet das Captive-Portal; STA-Server vorher stoppen (beide nutzen Port 80)
void startPortalNow(const char* reason) {
	if (wifiManagerAP.isPortalActive()) return;
	Serial.printf("Starte Captive-Portal (%s)...\n", reason);
	wifiManagerSTA.stopSTA();
	wifiManagerAP.openPortal();
	ensureOtaReady();
}

// ----- Arduino Setup -----
void setup() {
	Serial.begin(115200);
	delay(200);

	// Partition-Check: erwartet mind. 2 OTA-App-Slots + 1 DATA-FS
	if (!checkOtaAndDataPartitions(true)) {
		Serial.println("⚠️ Partition-Layout unzureichend: benötige mindestens 2 OTA-Slots und 1 DATA (SPIFFS/FFat/LittleFS).");
	}

	Serial.println("ESPWiFiManagerAP v" AWM_VERSION);

	wifiManagerAP.setHostname(DEFAULT_HOSTNAME);
	wifiManagerAP.setFallbackPolicy(ESPWiFiManagerAP::FallbackPolicy::ON_FAIL);
	wifiManagerAP.begin();   // lädt Credentials + Hostname aus NVS
	// Timeout nur mit Credentials: sonst würde WiFi nach dem Schließen komplett abgeschaltet
	if (wifiManagerAP.hasCredentials()) wifiManagerAP.setPortalTimeout(PORTAL_TIMEOUT_S);
	wifiManagerAP.run();     // Boot-Taster, STA-Verbindung (inkl. NTP/mDNS), ggf. Portal

	if (wifiManagerAP.isConnected()) {
		Serial.print("WiFi verbunden. IP: "); Serial.println(WiFi.localIP());
		printTimeIfReady();
		wifiManagerSTA.setHostname(WiFi.getHostname());
		Serial.println(wifiManagerSTA.startSTA() ? "STA-Server läuft." : "STA-Server-Start fehlgeschlagen.");
	} else if (!wifiManagerAP.isPortalActive()) {
		startPortalNow("connect failed");
	}
	ensureOtaReady();
}

// ----- Arduino Loop -----
void loop() {
	const uint32_t now = millis();

	// AP-Manager: Portal/DNS, LED-Status, verzögerter Neustart
	wifiManagerAP.update();
	// STA-Manager: verzögerter Neustart nach /erase
	wifiManagerSTA.handleClient();
	// OTA-Handler (ArduinoOTA + ElegantOTA-Neustart nach Update)
	handleArduinoOta();
	ElegantOTA.loop();

	const bool portal    = wifiManagerAP.isPortalActive();
	const bool connected = wifiManagerAP.isConnected();

#if (BTN_PIN >= 0)
	// Tastendruck ≥ SHORT_PRESS_MS öffnet Portal (Re-Provisioning)
	const bool btnNow = digitalRead(BTN_PIN); // HIGH=frei, LOW=gedrückt
	if (btnPrev && !btnNow) {
		btnPressedAt = now;
	} else if (!btnPrev && btnNow && now - btnPressedAt >= SHORT_PRESS_MS) {
		startPortalNow("button");
	}
	btnPrev = btnNow;
#endif

	// Reconnect-Zustandsmaschine auch im verbundenen Zustand bedienen, damit sie nach
	// erfolgreichem Reconnect nach IDLE zurückkehrt (sonst falscher Fehlversuch beim nächsten Ausfall)
	if (!portal) wifiManagerAP.checkReconnect();

	if (connected) {
		disconnectedAt = 0;
		// STA-Server (nach Reconnect oder Portal-Timeout) nachstarten
		static uint32_t lastStaRetry = 0;
		if (!portal && !wifiManagerSTA.isRunning() && now - lastStaRetry > 3000) {
			lastStaRetry = now;
			wifiManagerSTA.startSTA();
		}
	} else if (!portal) {
		// Nicht-blockierender Reconnect; bei längerem Ausfall Portal öffnen
		if (!disconnectedAt) {
			disconnectedAt = now ? now : 1;
			Serial.println("WiFi-Verbindung verloren → Reconnect");
		}
		if (now - disconnectedAt > LOST_PORTAL_MS) {
			disconnectedAt = 0;
			startPortalNow("connection lost");
		}
	}

	// Status-Log alle 5 s
	static uint32_t lastStatus = 0;
	if (now - lastStatus > 5000) {
		lastStatus = now;
		if (connected) {
			Serial.printf("WiFi OK | IP=%s | RSSI=%d dBm | STA-Server=%s\n",
				WiFi.localIP().toString().c_str(), WiFi.RSSI(), wifiManagerSTA.isRunning() ? "ON" : "OFF");
			printTimeIfReady();
		} else {
			Serial.println(portal ? "Portal aktiv (http://192.168.4.1)." : "WiFi nicht verbunden.");
		}
	}
}
