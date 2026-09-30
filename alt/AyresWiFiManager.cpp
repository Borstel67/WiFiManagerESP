/*
 *  SPDX-License-Identifier: MIT
 *  AyresWiFiManager — Implementierung (Quellcode)
 *  ---------------------------------------------------------------
 *  @file      AyresWiFiManager.cpp
 *  @version   2.0.2
 *  @author    Daniel C. Salgado — AyresNet
 *  @license   MIT
 *  @project   https://github.com/AyresNet/AyresWiFiManager
 *
 *  @file      AyresWiFiManager.cpp
 *  @version   2.0.3
 *  @author    R. Wisser – Anpassung und Erweiterung
 *  @license   MIT
 *  @project   https://github.com/Borstel67
 *
 *  @summary
 *    Plattformübergreifende Implementierung (ESP32/ESP8266) des WiFi-Managers mit:
 *    Captive Portal (AP + DNS Catch-All), Speicherung in LittleFS,
 *    Fallback-Richtlinien, Bereitstellungsschaltfläche, LED-Statusanzeigen, optionales NTP
 *    und Internetverifizierung (generate_204).
 *
 *  Implementierungshinweise
 *  ---------------------------------------------------------------
 *  • ESP32
 *      - Energiesparen deaktiviert: esp_wifi_set_ps(WIFI_PS_NONE).
 *      - LittleFS.begin(true) → Automatische Formatierung bei fehlgeschlagenem Montage.
 *      - JSON-Löschung: Datei vor dem Löschen schließen; rekursiv pro Ordner.
 *
 *  • ESP8266
 *      - Sleep deaktiviert: WiFi.setSleepMode(WIFI_NONE_SLEEP).
 *      - JSON-Löschung nicht rekursiv: eraseJsonInDir() iteriert durch Ordner.
 *
 *  LED-Zustände (Standard-Pin 2)
 *      ON             → verbunden
 *      BLINK_SLOW     → Portal aktiv
 *      BLINK_FAST     → wird gescannt
 *      BLINK_DOUBLE   → Rückmeldung während Hold 2–5 s (öffnet Portal)
 *      BLINK_TRIPLE   → Rückmeldung während Hold ≥5 s (löscht Anmeldedaten)
 *      OFF            → keine Verbindung und kein Portal
 *
 *  Schaltfläche (Pin 0, INPUT_PULLUP, aktiv bei LOW)
 *      2–5 s  → öffnet Portal (wenn enableButtonPortal(true))
 *      ≥5 s   → löscht Anmeldedaten (respektiert setProtectedJsons) und startet neu
 *
 *  Captive Portal
 *      • setCaptivePortal(true) aktiviert DNS Catch-All und "Captive"-Routen.
 *      • setPortalTimeout(sec) Schließung durch Inaktivität.
 *      • setAPClientCheck(true) verhindert Schließung, wenn Clients im AP vorhanden sind.
 *      • setWebClientCheck(true) setzt das Timeout bei jeder HTTP-Anfrage zurück.
 *
 *  Sichere Löschung von .json
 *      • eraseJsonInDir("/") löscht alle .json außer den geschützten
 *        über setProtectedJsons({...}). Auf ESP32 rekursiv; auf ESP8266,
 *        pro Ordner aufrufen, wenn Unterverzeichnisse vorhanden sind.
 *
 *  Neuigkeiten v2.0.1
 *  ---------------------------------------------------------------
 *  • Konfigurierbare Wiederverbindung:
 *      - setReconnectBackoffMs(ms) → Minimale Verzögerung zwischen Versuchen (Standard 10 s)
 *      - setReconnectAttemptMs(ms) → Fenster pro Versuch (Standard 5 s)
 *  • Koexistenz mit externem AP/Portal:
 *      - setExternalApActive(true) aktiviert AP+STA-Szenarien ohne Herunterfahren des externen SoftAP
 *        während Wiederverbindungsversuche; das Portal wird nicht versehentlich geschlossen,
 *        wenn ein externer AP/Portal aktiv ist.
 *      - stopPortal() bewahrt den SoftAP wenn das externe Flag aktiv ist.
 *      - forzarReconexion() und reintentarConexionSiNecesario() respektieren dieses Flag.
 *
 *  Leistungsaspekte
 *      - Zu häufige Scans vermeiden (SCAN_INTERVAL_MS).
 *      - Ergebnis von /scan(.json) cachen wenn das Fenster kurz ist.
 *      - LED-Muster anpassen um Jitter bei WiFi/HTTP zu minimieren.
 */


#include "AyresWiFiManager.h"
#include <ArduinoJson.h>
#include "AWM_Logging.h"

#if defined(ESP32)
#include <HTTPClient.h>
#include <esp_wifi.h>
#elif defined(ESP8266)
#include <ESP8266HTTPClient.h>
#endif

#include <time.h>

 // ---------- Konstruktor ----------
AyresWiFiManager::AyresWiFiManager(uint8_t ledPin_, uint8_t buttonPin_)
	: server(80), ledPin(ledPin_), buttonPin(buttonPin_) {
}

// =====================================================
//                 SETTER / UMSCHALTER
// =====================================================
void AyresWiFiManager::setHtmlPathPrefix(const String& prefix) {
	htmlPathPrefix = prefix.endsWith("/") ? prefix : prefix + "/";
}

void AyresWiFiManager::setHostname(const String& host) { hostname = host; }

void AyresWiFiManager::setAPCredentials(const String& ssid_, const String& pass_) {
	apSSID = ssid_; apPASS = pass_;
}

void AyresWiFiManager::setCaptivePortal(bool enabled) { captiveEnabled = enabled; }
void AyresWiFiManager::setPortalTimeout(uint32_t seconds) { portalTimeoutMs = seconds * 1000UL; }
void AyresWiFiManager::setAPClientCheck(bool enabled) { apClientCheck = enabled; }
void AyresWiFiManager::setWebClientCheck(bool enabled) { webClientCheck = enabled; }
bool AyresWiFiManager::isPortalActive() const { return portalActive; }
void AyresWiFiManager::openPortal() { startPortal(); }
void AyresWiFiManager::closePortal() { stopPortal(); }

void AyresWiFiManager::setFallbackPolicy(FallbackPolicy p) { fallbackPolicy = p; }
void AyresWiFiManager::setSmartRetries(uint8_t maxRetries, uint32_t windowMs) {
	maxFailRetries = maxRetries; failWindowMs = windowMs;
}
void AyresWiFiManager::enableButtonPortal(bool enable) { allowButtonPortal = enable; }

// ===== [NEU] Konfigurierbare Wiederverbindung =====
void AyresWiFiManager::setReconnectBackoffMs(uint32_t ms) {
	reconnectBackoffMs = (ms < 1000) ? 1000 : ms; // Sicherheitsminimum 1s
	AWM_LOGI("⚙️  Wiederverbindungs-Backoff = %lu ms", (unsigned long)reconnectBackoffMs);
}
void AyresWiFiManager::setReconnectAttemptMs(uint32_t ms) {
	reconnectAttemptMs = (ms < 1000) ? 1000 : ms; // Minimum 1s
	AWM_LOGI("⚙️  Versuchsfenster = %lu ms", (unsigned long)reconnectAttemptMs);
}
// ===== [NEU] Externer AP/Portal =====
void AyresWiFiManager::setExternalApActive(bool active) {
	externalApActive = active;
	AWM_LOGI("⚙️  Externer AP aktiv: %s", externalApActive ? "ja" : "nein");
}
bool AyresWiFiManager::isExternalApActive() const { return externalApActive; }

// =====================================================
//                      BEGIN / RUN
// =====================================================
void AyresWiFiManager::begin() {
	pinMode(ledPin, OUTPUT);
	digitalWrite(ledPin, LOW);
	pinMode(buttonPin, INPUT_PULLUP);

#if defined(ESP32)
	WiFi.persistent(false);
	WiFi.setAutoReconnect(true);
	WiFi.setSleep(false);
	esp_wifi_set_ps(WIFI_PS_NONE);
#else
	WiFi.persistent(false);
	WiFi.setAutoReconnect(true);
	WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif

#if defined(ESP32)
	if (!LittleFS.begin(true)) {
#else
	if (!LittleFS.begin()) {
#endif
		AWM_LOGE("❌ Fehler beim Monten von LittleFS");
		return;
	}

	loadCredentials();
}

void AyresWiFiManager::run() {// Versuche sofort zu verbinden; wenn es fehlschlägt, öffne das Portal (Richtlinie beachten)
	// Fenster zur Erkennung von Hold mit LED-Rückmeldung
	AWM_LOGI("🔔 Schaltfläche: 2–5s öffnet Portal | ≥5s löscht Anmeldedaten");

	unsigned long startTime = millis();
	bool pressed = false;

	ledSet(LedPattern::BLINK_SLOW); // blinken während Fenster
	while (millis() - startTime < 2000) {
		if (digitalRead(buttonPin) == LOW) { pressed = true; break; }
		ledTask(); delay(10);
	}
	ledSet(LedPattern::OFF);

	if (pressed) {// Taste gehalten → Warte auf Loslassen mit Rückmeldung
		unsigned long t0 = millis();
		while (digitalRead(buttonPin) == LOW) {
			unsigned long held = millis() - t0;
			if (held >= 5000) {
				setLedPatternManual(LedPattern::BLINK_TRIPLE);
				AWM_LOGW("🩹 Hold ≥5s → Anmeldedaten löschen und neu starten");
				eraseCredentials();
				delay(900);
				ESP.restart();
			}
			else if (held >= 2000) {
				setLedPatternManual(LedPattern::BLINK_DOUBLE); // Warnung: öffne Portal
			}
			else {
				setLedPatternManual(LedPattern::BLINK_FAST);   // kurzer Hold
			}
			ledTask(); delay(10);
		}
		unsigned long held = millis() - t0;
		if (held >= 2000 && held < 5000 && allowButtonPortal) {
			AWM_LOGI("🟢 Hold 2–5s → Portal öffnen");
			setLedAuto(true);
			startPortal();
			return;
		}
		setLedAuto(true);
	}

	// Verbinde wenn Anmeldedaten vorhanden sind
	if (connectToWiFi()) {// Erfolg → Portal nicht öffnen
		AWM_LOGI("✅ WiFi-Verbindung erfolgreich.");
		sincronizarHoraNTP();// NTP-Zeitsynchronisation
		setupHTTPRoutes();// HTTP-Server mit Routen einrichten
		server.begin();// HTTP-Server starten
		ledSet(LedPattern::ON);// LED an wenn verbunden
		connected = true;
		return;
	}

	// Konnte nicht verbinden → Richtlinie befolgen
	switch (fallbackPolicy) {
	case FallbackPolicy::ON_FAIL:
		AWM_LOGI("🟡 Verbindung fehlgeschlagen → Portal öffnen (Richtlinie=ON_FAIL)");
		startPortal();
		break;
	case FallbackPolicy::NO_CREDENTIALS_ONLY:
		if (!tieneCredenciales()) {
			AWM_LOGI("🟡 Keine Anmeldedaten → Portal öffnen");
			startPortal();
		}
		else {
			AWM_LOGI("🟠 Mit Anmeldedaten → Portal NICHT öffnen (NO_CREDENTIALS_ONLY)");
		}
		break;
	case FallbackPolicy::SMART_RETRIES:
		AWM_LOGI("🟠 SMART_RETRIES aktiv → kein Portal vorerst; wird geöffnet wenn mehrere Versuche fehlschlagen");
		break;
	case FallbackPolicy::BUTTON_ONLY:
		AWM_LOGI("🟠 BUTTON_ONLY → Portal nicht automatisch öffnen");
		break;
	case FallbackPolicy::NEVER:
		AWM_LOGI("🟠 NEVER → Portal nicht automatisch öffnen");
		break;
	}
}

// =====================================================
void AyresWiFiManager::update() {
	server.handleClient();
	if (dnsRunning) dns.processNextRequest();

	ledAutoUpdate();
	ledTask();

	if (portalActive && portalHasTimedOut()) {
		AWM_LOGW("⏳ Portal Timeout → schließen");
		stopPortal();
	}
}

// =====================================================
//                    AP / DNS / HTTP
// =====================================================
void AyresWiFiManager::redirectToRoot() {
	server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
	server.sendHeader("Pragma", "no-cache");
	server.sendHeader("Expires", "0");
	server.sendHeader("Location", "/", true);
	server.send(302, "text/plain", "");
}

void AyresWiFiManager::setupHTTPRoutes() {
	// Eigene Seiten
	server.on("/", std::bind(&AyresWiFiManager::handleRoot, this));// GET Root
	server.on("/save", std::bind(&AyresWiFiManager::handleSave, this));// POST Speichern

	// CSS-Dateieroute
	server.on("/WifiManager.css", std::bind(&AyresWiFiManager::handleCss, this));// GET CSS

	// Scan: Hauptroute und klassischer Alias
	server.on("/scan", std::bind(&AyresWiFiManager::handleScan, this));// GET Scan (HTML)
	server.on("/scan.json", std::bind(&AyresWiFiManager::handleScan, this));// GET Scan (JSON)

	// NEU: Anmeldedaten via POST /erase löschen
	server.on("/erase", HTTP_POST, std::bind(&AyresWiFiManager::handleErase, this));// POST Löschen

	// Verbindungserkennung Routen: Portal erzwingen
	if (captiveEnabled) {// Diese Routen werden von Betriebssystemen aufgerufen, um die Internetverbindung zu überprüfen. Indem wir sie hier abfangen, können wir Geräte zum Captive Portal umleiten, wenn sie versuchen, eine Verbindung herzustellen.
		server.on("/generate_204", [this]() { redirectToRoot(); }); // Android
		server.on("/gen_204", [this]() { redirectToRoot(); }); // Android Alt
		server.on("/hotspot-detect.html", [this]() { redirectToRoot(); }); // iOS/macOS
		server.on("/connecttest.txt", [this]() { redirectToRoot(); }); // Windows
		server.on("/ncsi.txt", [this]() { redirectToRoot(); }); // Windows NCSI
		server.on("/fwlink", [this]() { redirectToRoot(); }); // Windows IE/Edge
	}

	// Favicon
	server.on("/favicon.ico", [this]() { // Verhindere unnötige 404-Fehler im Log, indem wir eine leere Antwort für favicon.ico zurückgeben, das von Browsern angefordert wird.
		server.sendHeader("Cache-Control", "public, max-age=86400");
		server.send(204, "text/plain", "");
		});

	// Jede andere Route → 302 zum Root
	server.onNotFound(std::bind(&AyresWiFiManager::handleNotFound, this));// Alle nicht definierten Routen → 302 zum Root (hilft bei Captive Portal Umleitung)
}

void AyresWiFiManager::startDNS() {
	if (dnsRunning) return;
	dns.setErrorReplyCode(DNSReplyCode::NoError);
	dns.start(53, "*", WiFi.softAPIP()); // nutze die echte AP-IP
	dnsRunning = true;
}

void AyresWiFiManager::stopDNS() {
	if (!dnsRunning) return;
	dns.stop();
	dnsRunning = false;
}

void AyresWiFiManager::setupAP() {
	WiFi.mode(WIFI_AP);
	WiFi.softAPConfig(apIP, apGW, apSN);
	WiFi.softAP(apSSID.c_str(), apPASS.c_str());
#if defined(ESP32)
	if (hostname.length()) WiFi.softAPsetHostname(hostname.c_str());
#endif
	AWM_LOGI("📡 AP: %s | IP %s", apSSID.c_str(), apIP.toString().c_str());
}

void AyresWiFiManager::startPortal() { // öffnet den AP, richtet die HTTP-Routen ein und startet den Server
	if (portalActive) return;
	setupAP();
	setupHTTPRoutes();// HTTP-Server mit Routen einrichten
	server.begin();// HTTP-Server starten
	if (captiveEnabled) {// DNS-Catch-All nur aktivieren wenn Captive Portal aktiviert ist
		startDNS();          // DNS Catch-All aktiv
	}
	else {
		if (dnsRunning) stopDNS(); // zur Sicherheit
	}
	portalActive = true;
	portalStart = millis();
	lastHttpAccess = portalStart;
	AWM_LOGI("🌐 Captive Portal aktiv auf 192.168.4.1 (GET /, /scan, POST /save, POST /erase)");
	ledSet(LedPattern::BLINK_SLOW);
}

void AyresWiFiManager::stopPortal() {
	if (!portalActive) return;
	stopDNS();
	server.stop();

	// [GEÄNDERT] Wenn externer AP aktiv, den globalen SoftAP NICHT herunterfahren.
	if (!externalApActive) {
		WiFi.softAPdisconnect(true);
	}
	else {
		AWM_LOGI("🔒 Externer AP aktiv → bewahre SoftAP (wird nicht getrennt).");
	}

	portalActive = false;

	// [GEÄNDERT] Modus je nach Kontext wiederherstellen:
	if (externalApActive) {
		// Radio im AP-Modus lassen (extern verwaltet sein Web)
		WiFi.mode(WIFI_AP);
	}
	else {
		if (!ssid.isEmpty()) WiFi.mode(WIFI_STA);
		else                 WiFi.mode(WIFI_OFF);
	}

	AWM_LOGI("✅ Captive Portal gestoppt");
}

bool AyresWiFiManager::captivePortalRedirect() {
	if (!portalActive || !captiveEnabled) return false;

	String host = server.hostHeader();
	String ap = WiFi.softAPIP().toString();
	if (ap == "0.0.0.0") ap = apIP.toString();

	if (host != ap) {
		server.sendHeader("Location", String("http://") + ap, true);
		server.send(302, "text/plain", "");
		server.client().stop();
		return true;
	}
	return false;
}

uint8_t AyresWiFiManager::softAPStationCount() {
	return WiFi.softAPgetStationNum();
}

bool AyresWiFiManager::portalHasTimedOut() {
	if (portalTimeoutMs == 0) return false;

	if (apClientCheck && softAPStationCount() > 0) {
		portalStart = millis(); // Fenster verlängern solange Clients vorhanden sind
		return false;
	}

	unsigned long base = webClientCheck ? lastHttpAccess : portalStart;
	return (millis() - base) > portalTimeoutMs;
}

// ---------- HTTP ----------
void AyresWiFiManager::handleRoot() { // GET /
	if (captivePortalRedirect()) return;// Ruft ein Gerät eine andere Route auf, während das Portal aktiv ist, leite zum Root um (DNS-Catch-All)
	lastHttpAccess = millis(); // Aktualisiere den letzten Zugriff für Timeout-Zwecke

	// beginn aufruf index.html
	if (WiFi.status() == WL_CONNECTED) { // Wenn verbunden, indexSTA.html laden, sonst indexAP.html
		String path = htmlPathPrefix + "indexSTA.html";

		if (!LittleFS.exists(path)) { // Überprüfe ob die Datei existiert
			server.send(500, "text/html", "<h1>Fehler: indexSTA.html nicht gefunden</h1>");
			return;// Fehlerseite könnte hier auch aus LittleFS geladen werden, wenn gewünscht
		}
		File file = LittleFS.open(path, "r");// Öffne die Hauptseite

		if (!file || file.isDirectory()) { // Überprüfe ob die Datei korrekt geöffnet wurde
			server.send(500, "text/html", "<h1>Fehler beim Öffnen von indexSTA.html</h1>");
			return;// Fehlerseite könnte hier auch aus LittleFS geladen werden, wenn gewünscht
		}
		// HTML streamen indexSTA.html; CSS wird über eigene Route geliefert
		server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");// Verhindere Caching der Hauptseite
		server.send(200, "text/html", file.readString());// sendet die Hauptseite als String; für große Dateien könnte auch server.streamFile(file, "text/html") verwendet werden
		file.close();
	}
	else {
		String path = htmlPathPrefix + "indexAP.html"; // Wenn nicht verbunden, indexAP.html laden

		if (!LittleFS.exists(path)) { // Überprüfe ob die Datei existiert
			server.send(500, "text/html", "<h1>Fehler: indexAP.html nicht gefunden</h1>");
			return;// Fehlerseite könnte hier auch aus LittleFS geladen werden, wenn gewünscht
		}
		File file = LittleFS.open(path, "r");// Öffne die Hauptseite

		if (!file || file.isDirectory()) { // Überprüfe ob die Datei korrekt geöffnet wurde
			server.send(500, "text/html", "<h1>Fehler beim Öffnen von indexAP.html</h1>");
			return;// Fehlerseite könnte hier auch aus LittleFS geladen werden, wenn gewünscht
		}
		// HTML streamen indexAP.html; CSS wird über eigene Route geliefert
		server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");// Verhindere Caching der Hauptseite
		server.send(200, "text/html", file.readString());// sendet die Hauptseite als String; für große Dateien könnte auch server.streamFile(file, "text/html") verwendet werden
		
		file.close();
	}
	// ende aufruf index.html
}

void AyresWiFiManager::handleSave() { // POST /save
	if (captivePortalRedirect()) return;
	lastHttpAccess = millis();

	if (server.method() != HTTP_POST) {
		server.send(405, "text/plain", "Methode nicht erlaubt");
		return;
	}

	String inSsid = server.arg("ssid");
	String inPass = server.arg("password");
	if (inSsid.isEmpty() || inPass.isEmpty()) {
		mostrarPaginaError("Nicht genug Daten zum Speichern.");
		return;
	}

	StaticJsonDocument<192> doc;
	doc["ssid"] = inSsid;
	doc["password"] = inPass;

	File file = LittleFS.open("/wifi.json", "w");
	if (!file) {
		mostrarPaginaError("Fehler beim Speichern von Anmeldedaten.");
		return;
	}
	serializeJson(doc, file);
	file.close();

	File success = LittleFS.open(htmlPathPrefix + "success.html", "r");// Versuche success.html zu laden, um eine Rückmeldung an den Benutzer zu geben, dass die Daten gespeichert wurden. Wenn es nicht gefunden wird, gebe eine einfache Nachricht zurück.
	if (!success) {
		server.send(200, "text/html", "<h1>Gespeichert. Starte neu...</h1>");
	}
	else {
		server.send(200, "text/html", success.readString());
		success.close();
	}

	delay(1000);
	ESP.restart();
}

void AyresWiFiManager::handleErase() { // POST /erase
	if (captivePortalRedirect()) return;
	lastHttpAccess = millis();

	if (server.method() != HTTP_POST) {
		server.send(405, "text/plain", "Methode nicht erlaubt");
		return;
	}

	// Antworte zuerst dem Browser um Timeouts zu vermeiden
	server.send(200, "application/json", "{\"ok\":true}");
	delay(150);

	// Lösche alle .json (respektiere Whitelist setProtectedJsons)
	eraseCredentials();

	delay(300);
	ESP.restart();
}

void AyresWiFiManager::handleScan() { // GET /scan oder /scan.json
	lastHttpAccess = millis();
	AWM_LOGI("🔍 Scanne WiFi-Netzwerke (SYNC, AP+STA)…");

	// Behalte AP während STA scannt
	WiFi.mode(WIFI_AP_STA);
	delay(50);

	// Falls Scan läuft, breche ihn ab
	int st = WiFi.scanComplete();// -2 = läuft, -1 = nicht gestartet oder abgeschlossen
	if (st == WIFI_SCAN_RUNNING) {
		WiFi.scanDelete();
	}

	// LED-Status "scanning"
	scanning = true;
	scanningUntil = millis() + 1500;

	// Blockierendes Scannen (zuverlässig und einfach)
	int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
	if (n < 0) {
		scanning = false;
		server.send(200, "application/json", "[]");
		AWM_LOGW("⚠️ Scan fehlgeschlagen, gebe [] zurück");
		return;
	}

	// Konstruiere JSON [{ssid, rssi, secure}, ...]
	size_t cap = 64U + (size_t)n * 64U;
	if (cap < 512U) cap = 512U;
	DynamicJsonDocument doc(cap);
	JsonArray arr = doc.to<JsonArray>();

	for (int i = 0; i < n; ++i) {
		String s = WiFi.SSID(i);
		if (!s.length()) continue; // versteckte
		JsonObject o = arr.createNestedObject();
		o["ssid"] = s;
		o["rssi"] = WiFi.RSSI(i);
		o["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
	}

	WiFi.scanDelete(); // Ergebnisse aus RAM löschen
	scanning = false;

	String out;
	serializeJson(arr, out);
	server.send(200, "application/json", out);
	AWM_LOGI("✅ Scan OK: %d Netzwerke", (int)arr.size());
}

void AyresWiFiManager::handleNotFound() {
	if (captivePortalRedirect()) return;
	lastHttpAccess = millis();
	server.sendHeader("Location", "/", true);
	server.send(302, "text/plain", "");
}

void AyresWiFiManager::mostrarPaginaError(const String & mensajeFallback) {
	File errorFile = LittleFS.open(htmlPathPrefix + "error.html", "r");
	if (!errorFile) {
		server.send(500, "text/html", "<h1>Fehler: " + mensajeFallback + "</h1>");
	}
	else {
		server.send(500, "text/html", errorFile.readString());
		errorFile.close();
	}
}

void AyresWiFiManager::handleCss() {
	if (captivePortalRedirect()) return;
	lastHttpAccess = millis();

	String pathcss = htmlPathPrefix + "WifiManager.css";
	if (!LittleFS.exists(pathcss)) {
		server.send(404, "text/css", "");
		return;
	}

	File filecss = LittleFS.open(pathcss, "r");
	if (!filecss) {
		server.send(500, "text/css", "");
		return;
	}

	// Cache-Header für statische Ressource
	server.sendHeader("Cache-Control", "public, max-age=86400");
	server.streamFile(filecss, "text/css");
	filecss.close();
}
// =====================================================
//                    ANMELDEDATEN
// =====================================================
bool AyresWiFiManager::tieneCredenciales() const {
	return LittleFS.exists("/wifi.json") && !ssid.isEmpty() && !password.isEmpty();
}

void AyresWiFiManager::loadCredentials() {
	if (!LittleFS.exists("/wifi.json")) {
		AWM_LOGI("ℹ️ /wifi.json existiert nicht.");
		return;
	}
	File file = LittleFS.open("/wifi.json", "r");
	if (!file) {
		AWM_LOGE("❌ Konnte /wifi.json nicht öffnen");
		return;
	}
	StaticJsonDocument<192> doc;
	DeserializationError error = deserializeJson(doc, file);
	file.close();
	if (error) {
		AWM_LOGE("❌ Fehler beim Deserialisieren von JSON aus /wifi.json");
		return;
	}

	String loadedSsid = doc["ssid"].as<String>();
	String loadedPassword = doc["password"].as<String>();
	if (loadedSsid.isEmpty() || loadedPassword.isEmpty()) {
		AWM_LOGW("⚠️ Leere Anmeldedaten in Datei.");
		return;
	}
	ssid = loadedSsid;
	password = loadedPassword;
	AWM_LOGI("✅ Anmeldedaten geladen (SSID=\"%s\").", ssid.c_str());
	AWM_LOGI("✅ Anmeldedaten geladen (PW=\"%s\").", password.c_str());
}

void AyresWiFiManager::saveCredentials(String s, String p) {
	StaticJsonDocument<192> doc;
	doc["ssid"] = s;
	doc["password"] = p;
	File file = LittleFS.open("/wifi.json", "w");
	if (!file) {
		AWM_LOGE("❌ Fehler beim Öffnen von /wifi.json zum Schreiben");
		return;
	}
	serializeJson(doc, file);
	file.close();
}

void AyresWiFiManager::eraseCredentials() {
	eraseJsonInDir("/");   // Wurzel

#if defined(ESP8266)
	// Auf ESP8266 ist der Iterator nicht rekursiv; pro Ordner aufrufen wenn nötig.
#endif

	AWM_LOGI("🧹 .json Bereinigung abgeschlossen (respektiere geschützte).");
}

// =====================================================
//                     VERBINDUNG STA
// =====================================================
bool AyresWiFiManager::connectToWiFi() {// Versucht sich mit den gespeicherten Anmeldedaten zu verbinden. Gibt true zurück wenn erfolgreich verbunden, sonst false.
	if (!tieneCredenciales()) return false;

	WiFi.mode(WIFI_STA);// Stelle sicher dass wir im STA-Modus sind (AP wird bei Bedarf in Portal-Start aktiviert)
	WiFi.begin(ssid.c_str(), password.c_str());// Starte Verbindung

	AWM_LOGI("Verbinde mit %s", ssid.c_str());

	const uint32_t TOUT_MS = 15000;
	uint32_t t0 = millis();
	while (millis() - t0 < TOUT_MS) {
		if (WiFi.status() == WL_CONNECTED) {
			AWM_LOGI("Verbunden. IP: %s", WiFi.localIP().toString().c_str());
#if defined(ESP32)
			WiFi.setSleep(false);
#endif
			connected = true;
			return true;
		}
		delay(250);
	}

	AWM_LOGW("⏱️ Timeout erreicht. Konnte nicht verbinden.");
	connected = false;
	return false;
}

bool AyresWiFiManager::isConnected() {// Gibt den aktuellen Verbindungsstatus zurück. Achtung: Dieser Status wird nicht automatisch aktualisiert, außer durch connectToWiFi() oder reintentarConexionSiNecesario(). Es ist ratsam, diese Methode nicht in einem schnellen Loop zu verwenden, da sie den WiFi-Status abfragt, was je nach Plattform teuer sein kann.
	connected = (WiFi.status() == WL_CONNECTED);
	return connected;
}

int AyresWiFiManager::getSignalStrength() {
	return WiFi.RSSI();
}

void AyresWiFiManager::reintentarConexionSiNecesario() {
	if (!autoReconnect) return;
	if (WiFi.status() == WL_CONNECTED) { connected = true; return; }

	connected = false;
	unsigned long ahora = millis();

	// [GEÄNDERT] Konfigurierbarer Backoff
	if (ahora - ultimoIntentoWiFi < reconnectBackoffMs) return;
	ultimoIntentoWiFi = ahora;

	if (!ssid.isEmpty() && !password.isEmpty()) {
		AWM_LOGI("🔁 Versuche WiFi-Wiederverbindung... (Fenster=%lu ms, Backoff=%lu ms)",
			(unsigned long)reconnectAttemptMs, (unsigned long)reconnectBackoffMs);

		// [GEÄNDERT] Wenn AWM-Portal oder externer AP, behalte AP während Versuch aktiv
		if (portalActive || externalApActive) WiFi.mode(WIFI_AP_STA);
		else                                  WiFi.mode(WIFI_STA);

		WiFi.begin(ssid.c_str(), password.c_str());
		uint32_t t0 = millis();
		bool ok = false;

		// [GEÄNDERT] Konfigurierbares Fenster
		while (millis() - t0 < reconnectAttemptMs) {
			if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
			delay(250);
		}
		if (ok) {
			AWM_LOGI("🔌 Wiederverbunden mit WiFi.");
			sincronizarHoraNTP();
			connected = true;
			failCount = 0; failWindowStart = 0;
			return;
		}
		AWM_LOGW("❌ WiFi-Wiederverbindung fehlgeschlagen.");

		// SMART_RETRIES (keine Änderungen)
		if (fallbackPolicy == FallbackPolicy::SMART_RETRIES) {
			if (failWindowStart == 0 || (millis() - failWindowStart) > failWindowMs) {
				failWindowStart = millis();
				failCount = 0;
			}
			failCount++;
			AWM_LOGD("📉 SMART: Fehler=%u/%u in %lu ms",
				failCount, maxFailRetries, (unsigned long)(millis() - failWindowStart));
			if (failCount >= maxFailRetries) {
				AWM_LOGW("🚪 SMART: öffne Portal aufgrund akkumulierter Fehler");
				startPortal();
				failCount = 0; failWindowStart = 0;
			}
		}
	}
}

bool AyresWiFiManager::scanRedDetectada() {
	unsigned long ahora = millis();
	if (ahora - ultimoScan < SCAN_INTERVAL_MS) return false;
	ultimoScan = ahora;

	if (WiFi.status() == WL_CONNECTED && !portalActive) return false;

	int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
	bool encontrada = false;
	for (int i = 0; i < n; ++i) {
		if (WiFi.SSID(i) == ssid) { encontrada = true; break; }
	}
	WiFi.scanDelete();
	return encontrada;
}

void AyresWiFiManager::forzarReconexion() {
	AWM_LOGI("🔄  Erzwinge Wiederverbindung…");
	// [GEÄNDERT] Respektiere externen AP um ihn nicht zu beeinträchtigen
	if (portalActive || externalApActive) WiFi.mode(WIFI_AP_STA);
	else                                  WiFi.mode(WIFI_STA);

	WiFi.begin(ssid.c_str(), password.c_str());
	ultimoIntentoWiFi = millis();
}

// =====================================================
//                     NTP / ZEIT
// =====================================================
void AyresWiFiManager::sincronizarHoraNTP() {
	configTime(0, 0, "pool.ntp.org", "time.nist.gov");
	for (int j = 0; j < 20; j++) {
		time_t now = time(nullptr);
		if (now > 100000) {
			AWM_LOGI("🕒 Zeit synchronisiert: %s", ctime(&now));
			return;
		}
		delay(200);
	}
	AWM_LOGW("⚠️ NTP reagierte nicht. Fortfahren ohne Synchronisierung.");
}

uint64_t AyresWiFiManager::getTimestamp() {
	time_t now = time(nullptr);
	return (now > 100000) ? static_cast<uint64_t>(now) * 1000ULL : 0;
}

// =====================================================
//                  INTERNETPRÜFUNG
// =====================================================
bool AyresWiFiManager::hayInternet() {
	if (WiFi.status() != WL_CONNECTED) return false;
	WiFiClient client;
	HTTPClient http;
	http.begin(client, "http://clients3.google.com/generate_204");
#if defined(ESP32)
	http.setConnectTimeout(3000);
#else
	http.setTimeout(3000);
#endif
	int httpCode = http.GET();
	http.end();
	return (httpCode == 204);
}

// =====================================================
//                     LED FSM
// =====================================================
void AyresWiFiManager::setLedAuto(bool enable) {
	ledAuto = enable;
	if (ledAuto) ledSet(LedPattern::OFF);
}
void AyresWiFiManager::setLedPatternManual(LedPattern p) {
	ledAuto = false;
	ledSet(p);
}
void AyresWiFiManager::ledSet(LedPattern p) {
	ledPat = p; ledStep = 0; ledT0 = millis();
}
void AyresWiFiManager::ledAutoUpdate() {
	if (!ledAuto) return;

	LedPattern want = LedPattern::OFF;

	// Priorität: Scan > Portal > Verbunden > Idle
	if (scanning || (millis() < scanningUntil)) {
		want = LedPattern::BLINK_FAST;
	}
	else if (portalActive) {
		want = LedPattern::BLINK_SLOW;
	}
	else if (WiFi.status() == WL_CONNECTED) {
		want = LedPattern::ON;
	}
	else {
		want = LedPattern::OFF;
	}

	if (want != ledPat) ledSet(want);
}

void AyresWiFiManager::ledTask() {
	const unsigned long now = millis();

	auto write = [&](uint8_t v) {
		if (ledOut != v) {
			ledOut = v;
			digitalWrite(ledPin, v);
		}
		};

	switch (ledPat) {
	case LedPattern::OFF: write(LOW); break;
	case LedPattern::ON:  write(HIGH); break;

	case LedPattern::BLINK_SLOW: {          // 500ms AN / 500ms AUS
		const unsigned long period = 1000;
		write(((now - ledT0) % period) < 500 ? HIGH : LOW);
	} break;

	case LedPattern::BLINK_FAST: {          // 100ms AN / 100ms AUS
		const unsigned long period = 200;
		write(((now - ledT0) % period) < 100 ? HIGH : LOW);
	} break;

	case LedPattern::BLINK_DOUBLE: {        // AN 120, AUS 120, AN 120, AUS 640
		static const uint16_t seq[] = { 120,120,120,640 };
		static const uint8_t  on[] = { 1,  0,  1,  0 };
		if (now - ledT0 >= seq[ledStep]) {
			ledT0 = now;
			ledStep = (ledStep + 1) % 4;
			write(on[ledStep] ? HIGH : LOW);
		}
	} break;

	case LedPattern::BLINK_TRIPLE: {        // AN 100, AUS 100 x3, AUS 500
		static const uint16_t seq[] = { 100,100,100,100,100,500 };
		static const uint8_t  on[] = { 1,  0,  1,  0,  1,  0 };
		if (now - ledT0 >= seq[ledStep]) {
			ledT0 = now;
			ledStep = (ledStep + 1) % 6;
			write(on[ledStep] ? HIGH : LOW);
		}
	} break;
	}
}

// =====================================================
//                  WIEDERVERBINDUNGS-TREIBER
// =====================================================
void AyresWiFiManager::setAutoReconnect(bool habilitado) {
	autoReconnect = habilitado;
	WiFi.setAutoReconnect(habilitado);
}

// =====================================================
//             Statische Helfer (JSON-Löschung)
// =====================================================
void AyresWiFiManager::setProtectedJsons(std::initializer_list<const char*> names) {
	_protectedExact.clear();
	for (auto n : names) {
		String s(n ? n : "");
		if (!s.length()) continue;
		if (!s.startsWith("/")) s = "/" + s;
		_protectedExact.push_back(s);
	}
}

bool AyresWiFiManager::isProtectedJson(const String & name) const {
	String n = name;
	if (!n.startsWith("/")) n = "/" + n;

	for (const auto& ex : _protectedExact) {
		if (n.equalsIgnoreCase(ex)) return true;
	}
	return false;
}

#if defined(ESP32)
// Rekursiv + schließe Datei VOR dem Löschen (vermeidet "Has open FD")
void AyresWiFiManager::eraseJsonInDir(const char* dirPath) {
	if (!dirPath || !*dirPath) return;

	File dir = LittleFS.open(dirPath);
	if (!dir || !dir.isDirectory()) return;

	String base = dirPath;
	if (!base.endsWith("/")) base += "/";

	// Sichere Iteration: schließe die Datei 'f' VOR dem Löschen
	for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
		String name = f.name();             // kann ohne initiales '/' kommen
		String full = name;
		if (!full.startsWith("/")) full = base + full;

		const bool isDir = f.isDirectory();
		f.close();                          // ← HANDLE VOR FORTFAHREN SCHLIESSEN

		if (isDir) {
			eraseJsonInDir(full.c_str());     // rekursiv
		}
		else {
			if (full.endsWith(".json") && !isProtectedJson(full)) {
				if (LittleFS.remove(full)) {
					AWM_LOGI("🗑️  Gelöscht: %s", full.c_str());
				}
				else {
					AWM_LOGW("⚠️  Konnte nicht löschen: %s", full.c_str());
				}
			}
		}
	}

	dir.close();
}
#else   // ESP8266 (nicht rekursiv)
void AyresWiFiManager::eraseJsonInDir(const char* dirPath) {
	if (!dirPath || !*dirPath) return;

	Dir d = LittleFS.openDir(dirPath);
	while (d.next()) {
		String name = d.fileName();         // normalerweise ohne initiales '/'
		String full = name;
		if (!full.startsWith("/")) full = String("/") + full;

		if (full.endsWith(".json") && !isProtectedJson(full)) {
			if (LittleFS.remove(full)) {
				AWM_LOGI("🗑️  Gelöscht: %s", full.c_str());
			}
			else {
				AWM_LOGW("⚠️  Konnte nicht löschen: %s", full.c_str());
			}
		}
	}
}
#endif

