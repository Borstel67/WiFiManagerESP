// SPDX-License-Identifier: MIT
// AP-/Captive-Portal-Implementierung für WiFiManagerESP
#include "ESPWiFiManagerAP.h"
#include "AWM_Logging.h"
#include "ESPWiFiManagerCommon.h"
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include <time.h>

#ifndef ELEGANTOTA_USE_ASYNC_WEBSERVER
#  define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#endif
#include <ElegantOTA.h>
// Hinweis: Kein Config-Backup um OTA nötig – die Config liegt in NVS,
// das weder Firmware- noch LittleFS-OTA überschreibt.

// ── Konstruktor / Setter ──────────────────────────────────────────────────────

ESPWiFiManagerAP::ESPWiFiManagerAP(uint8_t ledPin_, uint8_t buttonPin_)
    : ledPin(ledPin_), buttonPin(buttonPin_) {}

void ESPWiFiManagerAP::scheduleRestart() {
    _restartReqAt   = millis();
    _restartPending = true;
}

void ESPWiFiManagerAP::setHtmlPathPrefix(const String& prefix) { htmlPathPrefix = prefix.endsWith("/") ? prefix : prefix + "/"; }
void ESPWiFiManagerAP::setHostname(const String& host)         { hostname = host; }
void ESPWiFiManagerAP::setAPCredentials(const String& s, const String& p) { apSSID = s; apPASS = p; }
void ESPWiFiManagerAP::setCaptivePortal(bool en)               { captiveEnabled = en; }
void ESPWiFiManagerAP::setPortalTimeout(uint32_t s)            { portalTimeoutMs = s * 1000UL; }
void ESPWiFiManagerAP::setAPClientCheck(bool en)               { apClientCheck = en; }
void ESPWiFiManagerAP::setWebClientCheck(bool en)              { webClientCheck = en; }
bool ESPWiFiManagerAP::isPortalActive() const                  { return portalActive; }
void ESPWiFiManagerAP::openPortal()                            { startAP(); }
void ESPWiFiManagerAP::closePortal()                           { stopAP(); }
void ESPWiFiManagerAP::setFallbackPolicy(FallbackPolicy p)     { fallbackPolicy = p; }
void ESPWiFiManagerAP::setSmartRetries(uint8_t r, uint32_t w)  { maxFailRetries = r; failWindowMs = w; }
void ESPWiFiManagerAP::enableButtonPortal(bool en)             { allowButtonPortal = en; }
void ESPWiFiManagerAP::setAutoReconnect(bool en)               { autoReconnect = en; WiFi.setAutoReconnect(en); }
void ESPWiFiManagerAP::setExternalApActive(bool a)             { externalApActive = a; AWM_LOGI("⚙️  Externer AP: %s", a ? "ja" : "nein"); }
bool ESPWiFiManagerAP::isExternalApActive() const              { return externalApActive; }

void ESPWiFiManagerAP::setReconnectBackoffMs(uint32_t ms) {
    reconnectBackoffMs = (ms < 1000) ? 1000 : ms;
    AWM_LOGI("⚙️  Reconnect-Backoff = %lu ms", (unsigned long)reconnectBackoffMs);
}
void ESPWiFiManagerAP::setReconnectAttemptMs(uint32_t ms) {
    reconnectAttemptMs = (ms < 1000) ? 1000 : ms;
    AWM_LOGI("⚙️  Reconnect-Versuchsfenster = %lu ms", (unsigned long)reconnectAttemptMs);
}

// ── begin() ──────────────────────────────────────────────────────────────────

void ESPWiFiManagerAP::begin() {
    pinMode(ledPin,    OUTPUT);
    digitalWrite(ledPin, LOW);
    pinMode(buttonPin, INPUT_PULLUP);

    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        AWM_LOGE("❌ LittleFS Mount fehlgeschlagen (Partition 'littlefs')");
        return;
    }
    loadCredentials();
}

// ── run() – einmaliger Aufruf in setup() ─────────────────────────────────────

void ESPWiFiManagerAP::run() {
    AWM_LOGI("🔔 Button: 2–5 s = Portal | ≥5 s = Credentials löschen");

    unsigned long t = millis();
    bool pressed = false;
    ledSet(LedPattern::BLINK_SLOW);
    while (millis() - t < 2000) {
        if (digitalRead(buttonPin) == LOW) { pressed = true; break; }
        ledTask(); delay(10);
    }
    ledSet(LedPattern::OFF);

    if (pressed) {
        unsigned long t0 = millis();
        while (digitalRead(buttonPin) == LOW) {
            unsigned long held = millis() - t0;
            if (held >= 5000) {
                setLedPatternManual(LedPattern::BLINK_TRIPLE);
                AWM_LOGW("🩹 Hold ≥5 s → Credentials löschen und Neustart");
                eraseCredentials();
                delay(900);
                ESP.restart();
            } else if (held >= 2000) setLedPatternManual(LedPattern::BLINK_DOUBLE);
            else                     setLedPatternManual(LedPattern::BLINK_FAST);
            ledTask(); delay(10);
        }
        unsigned long held = millis() - t0;
        setLedAuto(true);
        if (held >= 2000 && held < 5000 && allowButtonPortal) {
            AWM_LOGI("🟢 Hold 2–5 s → Portal öffnen");
            startAP();
            return;
        }
    }

    if (connectToWiFiSTA()) {   // inkl. NTP-Sync und mDNS
        stopAP();
        ledSet(LedPattern::ON);
        return;
    }

    switch (fallbackPolicy) {
    case FallbackPolicy::ON_FAIL:
        AWM_LOGI("🟡 ON_FAIL → Portal öffnen");
        startAP();
        break;
    case FallbackPolicy::NO_CREDENTIALS_ONLY:
        if (!hasCredentials()) { AWM_LOGI("🟡 Keine Credentials → Portal öffnen"); startAP(); }
        else                    AWM_LOGI("🟠 NO_CREDENTIALS_ONLY → kein Portal");
        break;
    case FallbackPolicy::SMART_RETRIES:
        AWM_LOGI("🟠 SMART_RETRIES → Portal erst nach Fehlerserie");
        break;
    case FallbackPolicy::BUTTON_ONLY:
        AWM_LOGI("🟠 BUTTON_ONLY → kein automatisches Portal");
        break;
    case FallbackPolicy::NEVER:
        AWM_LOGI("🟠 NEVER → kein Portal");
        break;
    }
}

// ── update() – nicht-blockierend, im loop() aufrufen ─────────────────────────

void ESPWiFiManagerAP::update() {
    // Deferred Restart: Antwort erst ausliefern lassen, dann neu starten (überlauf-sicher)
    if (_restartPending && millis() - _restartReqAt >= 500) ESP.restart();

    if (dnsRunning) dns.processNextRequest();
    // Scan-Flag zurücksetzen, falls der Client /scan/result nie abfragt
    if (scanTimedOut()) resetScan();
    ledAutoUpdate();
    ledTask();

    if (portalActive && portalHasTimedOut()) {
        AWM_LOGW("⏳ Portal-Timeout → schließen");
        stopAP();
    }
}

// ── SoftAP ────────────────────────────────────────────────────────────────────

void ESPWiFiManagerAP::setupAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apGW, apSN);
    WiFi.softAP(apSSID.c_str(), apPASS.c_str());
#if defined(ESP32)
    if (hostname.length()) WiFi.softAPsetHostname(hostname.c_str());
#endif
    AWM_LOGI("📡 SoftAP: %s | IP %s", apSSID.c_str(), apIP.toString().c_str());
}

void ESPWiFiManagerAP::startAP() {
    if (portalActive) return;
    setupAP();
    if (!routesReady) {
        setupHTTPRoutes();
        ElegantOTA.begin(&apServer);
        routesReady = true;
    }
    apServer.begin();
    if (captiveEnabled) startDNS();
    else if (dnsRunning) stopDNS();
    portalActive     = true;
    portalStart      = millis();
    lastHttpAccess   = portalStart;
    AWM_LOGI("🌐 Captive Portal aktiv – http://192.168.4.1");
    AWM_LOGI("🛠  ElegantOTA unter /update");
    ledSet(LedPattern::BLINK_SLOW);
}

void ESPWiFiManagerAP::stopAP() {
    if (!portalActive) return;
    stopDNS();
    apServer.end();
    if (!externalApActive) WiFi.softAPdisconnect(true);
    else AWM_LOGI("🔒 Externer AP → SoftAP bleibt aktiv");
    portalActive = false;
    if (externalApActive)   WiFi.mode(WIFI_AP);
    else if (!ssid.isEmpty()) WiFi.mode(WIFI_STA);
    else                      WiFi.mode(WIFI_OFF);
    AWM_LOGI("✅ Captive Portal gestoppt");
}

// ── DNS ───────────────────────────────────────────────────────────────────────

void ESPWiFiManagerAP::startDNS() {
    if (dnsRunning) return;
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", WiFi.softAPIP());
    dnsRunning = true;
}
void ESPWiFiManagerAP::stopDNS() {
    if (!dnsRunning) return;
    dns.stop();
    dnsRunning = false;
}

// ── HTTP-Routen ───────────────────────────────────────────────────────────────

void ESPWiFiManagerAP::setupHTTPRoutes() {
    apServer.on("/",              HTTP_GET,  [this](AsyncWebServerRequest* r) { handleRoot(r); });
    apServer.on("/settings",      HTTP_GET,  [this](AsyncWebServerRequest* r) { handleSettings(r); });
    apServer.on("/settingsAP.html",HTTP_GET, [this](AsyncWebServerRequest* r) { handleSettings(r); });
    apServer.on("/save",          HTTP_POST, [this](AsyncWebServerRequest* r) { handleSave(r); });
    apServer.on("/WifiManager.css",HTTP_GET, [this](AsyncWebServerRequest* r) { handleCss(r); });
    // exact: "/scan" würde sonst auch "/scan/result" abfangen (Default-Matcher ^/scan(/.*)?$)
    apServer.on(AsyncURIMatcher::exact("/scan"), HTTP_GET, [this](AsyncWebServerRequest* r) { handleScan(r); });
    apServer.on("/scan.json",     HTTP_GET,  [this](AsyncWebServerRequest* r) { handleScan(r); });
    apServer.on("/scan/result",   HTTP_GET,  [this](AsyncWebServerRequest* r) { handleScanResult(r); });
    apServer.on("/erase",         HTTP_POST, [this](AsyncWebServerRequest* r) { handleErase(r); });
    apServer.on("/api/ap/config", HTTP_GET,  [this](AsyncWebServerRequest* r) { handleApiApConfig(r); });

    if (captiveEnabled) {
        apServer.on("/generate_204",     HTTP_GET, [this](AsyncWebServerRequest* r) { ::redirectToRoot(r); });
        apServer.on("/gen_204",          HTTP_GET, [this](AsyncWebServerRequest* r) { ::redirectToRoot(r); });
        apServer.on("/hotspot-detect.html",HTTP_GET,[this](AsyncWebServerRequest* r){ ::redirectToRoot(r); });
        apServer.on("/connecttest.txt",  HTTP_GET, [this](AsyncWebServerRequest* r) { ::redirectToRoot(r); });
        apServer.on("/ncsi.txt",         HTTP_GET, [this](AsyncWebServerRequest* r) { ::redirectToRoot(r); });
        apServer.on("/fwlink",           HTTP_GET, [this](AsyncWebServerRequest* r) { ::redirectToRoot(r); });
    }

    apServer.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r) {
        auto* res = r->beginResponse(204);
        res->addHeader("Cache-Control", "public, max-age=86400");
        r->send(res);
    });

    apServer.onNotFound([this](AsyncWebServerRequest* r) { handleNotFound(r); });
}

// ── Captive-Redirect ──────────────────────────────────────────────────────────

bool ESPWiFiManagerAP::captivePortalRedirect(AsyncWebServerRequest* request) {
    if (!portalActive || !captiveEnabled || !request) return false;
    const String host = request->host();
    String ap = WiFi.softAPIP().toString();
    if (ap == "0.0.0.0") ap = apIP.toString();
    if (host != ap) { request->redirect("http://" + ap); return true; }
    return false;
}

uint8_t ESPWiFiManagerAP::softAPStationCount() { return WiFi.softAPgetStationNum(); }

bool ESPWiFiManagerAP::portalHasTimedOut() {
    if (portalTimeoutMs == 0) return false;
    if (apClientCheck && softAPStationCount() > 0) { portalStart = millis(); return false; }
    unsigned long base = webClientCheck ? lastHttpAccess : portalStart;
    return (millis() - base) > portalTimeoutMs;
}

// ── HTTP-Handler ──────────────────────────────────────────────────────────────

void ESPWiFiManagerAP::handleRoot(AsyncWebServerRequest* request) {
    if (captivePortalRedirect(request)) return;
    lastHttpAccess = millis();
    const String path = htmlPathPrefix + "indexAP.html";
    if (!serveFileFromLittleFS(request, path, "text/html"))
        request->send(500, "text/html", "<h1>indexAP.html nicht gefunden</h1>");
}

void ESPWiFiManagerAP::handleSettings(AsyncWebServerRequest* request) {
    if (captivePortalRedirect(request)) return;
    lastHttpAccess = millis();
    const String path = htmlPathPrefix + "settings.html";
    if (!serveFileFromLittleFS(request, path, "text/html"))
        request->send(500, "text/html", "<h1>settings.html nicht gefunden</h1>");
}

void ESPWiFiManagerAP::handleCss(AsyncWebServerRequest* request) {
    if (captivePortalRedirect(request)) return;
    lastHttpAccess = millis();
    const String path = htmlPathPrefix + "WifiManager.css";
    if (!serveFileFromLittleFS(request, path, "text/css", true))
        request->send(404, "text/css", "");
}

void ESPWiFiManagerAP::handleNotFound(AsyncWebServerRequest* request) {
    if (captivePortalRedirect(request)) return;
    lastHttpAccess = millis();
    request->redirect("/");
    AWM_LOGW("⚠️ 404: %s", request->url().c_str());
}

void ESPWiFiManagerAP::showErrorPage(const String& msg, AsyncWebServerRequest* request) {
    const String path = htmlPathPrefix + "error.html";
    if (LittleFS.exists(path)) serveFileFromLittleFS(request, path, "text/html");
    else request->send(500, "text/html", "<h1>Fehler: " + msg + "</h1>");
}

// ── POST /save ────────────────────────────────────────────────────────────────

void ESPWiFiManagerAP::handleSave(AsyncWebServerRequest* request) {
    if (captivePortalRedirect(request)) return;
    lastHttpAccess = millis();

    const String newSsid = request->arg("ssid");
    if (newSsid.isEmpty() || newSsid.length() > 32) { showErrorPage("SSID fehlt oder ist zu lang.", request); return; }

    // settings.html sendet das Radio als "ap-dhcp"; "dhcp" für ältere Clients
    const String dhcpRaw = request->hasArg("dhcp") ? request->arg("dhcp") : request->arg("ap-dhcp");
    const bool   useDhcp = (dhcpRaw != "false");   // Default DHCP

    // Leeres Passwort-Feld → gespeichertes Passwort behalten (wird von /api/ap/config nicht ausgeliefert)
    String newPass = request->arg("password");
    if (newPass.isEmpty() && newSsid == ssid) newPass = password;
    if (newPass.length() < 8 || newPass.length() > 64) { showErrorPage("Passwort muss 8–64 Zeichen haben.", request); return; }

    // Statische Konfiguration vor dem Neustart prüfen (analog /api/sta/config)
    if (!useDhcp) {
        IPAddress tmp;
        if (!parseIpText(request->arg("ip"), tmp) || !parseIpText(request->arg("gw"), tmp) ||
            !parseIpText(request->arg("sn"), tmp)) {
            showErrorPage("Ungültige IP/Gateway/Subnetz.", request);
            return;
        }
    }

    JsonDocument doc;
    doc["ssid"]     = newSsid;
    doc["password"] = newPass;
    doc["dhcp"]     = useDhcp;
    doc["ip"]       = request->arg("ip");
    doc["gw"]       = request->arg("gw");
    doc["sn"]       = request->arg("sn");
    doc["dns1"]     = request->arg("dns1");
    doc["dns2"]     = request->arg("dns2");
    doc["hostname"] = request->arg("hostname");
    doc["ntp1"]     = request->arg("ntp1");
    doc["ntp2"]     = request->arg("ntp2");

    String out;
    serializeJson(doc, out);
    if (!saveConfigJson(out)) { showErrorPage("Speichern fehlgeschlagen (NVS).", request); return; }

    const String successPath = htmlPathPrefix + "success.html";
    if (LittleFS.exists(successPath)) serveFileFromLittleFS(request, successPath, "text/html");
    else request->send(200, "text/html", "<h1>Gespeichert. Starte neu…</h1>");

    scheduleRestart();
}

// ── POST /erase ───────────────────────────────────────────────────────────────

void ESPWiFiManagerAP::handleErase(AsyncWebServerRequest* request) {
    if (captivePortalRedirect(request)) return;
    lastHttpAccess = millis();
    request->send(200, "application/json", "{\"ok\":true}");
    eraseCredentials();
    scheduleRestart();
}

// ── GET /scan – startet asynchronen Scan (202), Ergebnis via /scan/result ────
// Kein blockierender Scan im AsyncTCP-Task (sonst Watchdog-Gefahr).

bool ESPWiFiManagerAP::scanTimedOut() const {
    return asyncScanStarted && (long)(millis() - scanningUntil) > 0;
}

void ESPWiFiManagerAP::resetScan() {
    WiFi.scanDelete();
    asyncScanStarted = false;
    scanningUntil    = 0;
}

void ESPWiFiManagerAP::handleScan(AsyncWebServerRequest* request) {
    lastHttpAccess = millis();

    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        // Bereits laufender Scan (auch ein verwaister nach Timeout) → erneut mit Timeout überwachen
        if (!asyncScanStarted) {
            asyncScanStarted = true;
            scanningUntil    = millis() + SCAN_TIMEOUT_MS;
        }
        request->send(202, "application/json", "{\"scanning\":true}");
        return;
    }
    WiFi.scanDelete();
    if (!(WiFi.getMode() & WIFI_MODE_STA)) WiFi.mode(WIFI_AP_STA);   // Scan benötigt STA-Interface

    if (WiFi.scanNetworks(true /*async*/, false /*show_hidden*/) == WIFI_SCAN_FAILED) {
        AWM_LOGW("⚠️ Scan-Start fehlgeschlagen");
        request->send(503, "application/json", "{\"error\":\"scan failed\"}");
        return;
    }
    asyncScanStarted = true;
    scanningUntil    = millis() + SCAN_TIMEOUT_MS;
    request->send(202, "application/json", "{\"scanning\":true}");
}

void ESPWiFiManagerAP::handleScanResult(AsyncWebServerRequest* request) {
    lastHttpAccess = millis();

    // Timeout-Schutz gegen Hänger
    if (scanTimedOut()) {
        resetScan();
        AWM_LOGW("⏱️ Scan-Timeout");
        request->send(504, "application/json", "{\"error\":\"scan timeout\"}");
        return;
    }

    const int st = WiFi.scanComplete();

    // Nur auf einen von uns überwachten Scan warten (Start-Race: FAILED direkt nach dem Start)
    if (asyncScanStarted && (st == WIFI_SCAN_RUNNING || st == WIFI_SCAN_FAILED)) {
        request->send(202, "application/json", "{\"scanning\":true}");
        return;
    }

    asyncScanStarted = false;
    scanningUntil    = 0;
    sendScanResults(request, st);
}

void ESPWiFiManagerAP::sendScanResults(AsyncWebServerRequest* request, int count) {
    if (count <= 0) {
        request->send(200, "application/json", "[]");
        return;
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < count; ++i) {
        String s = WiFi.SSID(i);
        if (!s.length()) continue;
        JsonObject o = arr.add<JsonObject>();
        o["ssid"]   = s;
        o["rssi"]   = WiFi.RSSI(i);
        o["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();

    String out;
    serializeJson(arr, out);
    request->send(200, "application/json", out);
    AWM_LOGI("✅ Scan-Ergebnis: %d Netzwerke", (int)arr.size());
}

// ── GET /api/ap/config ────────────────────────────────────────────────────────

// Passwort wird nie ausgeliefert (jeder AP-Client könnte es sonst auslesen);
// stattdessen "hasPassword" – ein leeres Formularfeld behält das gespeicherte Passwort.
void ESPWiFiManagerAP::handleApiApConfig(AsyncWebServerRequest* request) {
    if (captivePortalRedirect(request)) return;
    lastHttpAccess = millis();
    JsonDocument doc;
    if (!loadConfigDoc(doc)) {
        request->send(200, "application/json", "{}");
        return;
    }
    const char* pw = doc["password"] | "";
    doc["hasPassword"] = (*pw != '\0');
    doc.remove("password");
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// ── WiFi STA-Verbindung (blockierend – nur in run()/setup() aufrufen) ─────────

bool ESPWiFiManagerAP::connectToWiFiSTA() {
    if (!hasCredentials()) return false;

    // DHCP-Hostname muss vor dem Start des STA-Interfaces gesetzt werden
    if (hostname.length()) WiFi.setHostname(hostname.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    applyStaConfigFromWifiJson();
    WiFi.begin(ssid.c_str(), password.c_str());
    AWM_LOGI("Verbinde mit \"%s\"…", ssid.c_str());

    const uint32_t TOUT_MS = 15000;
    const uint32_t t0      = millis();
    while (millis() - t0 < TOUT_MS) {
        if (WiFi.status() == WL_CONNECTED) {
            AWM_LOGI("✅ Verbunden. IP: %s", WiFi.localIP().toString().c_str());
            WiFi.setSleep(false);
            syncTimeDefault();
            startMDNS(hostname.length() ? hostname : String(WiFi.getHostname()));
            connected = true;
            return true;
        }
        delay(250);
    }
    AWM_LOGW("⏱️ Verbindungs-Timeout.");
    connected = false;
    return false;
}

// ── checkReconnect() – nicht-blockierende Zustandsmaschine ───────────────────

void ESPWiFiManagerAP::checkReconnect() {
    if (!autoReconnect) return;

    const unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED) {
        if (_reconnState == ReconnState::CONNECTING) {
            AWM_LOGI("🔌 Wiederverbunden mit WiFi.");
            syncTimeDefault(0);   // nur SNTP konfigurieren, nicht im loop() blockieren
            failCount = 0; failWindowStart = 0;
            _reconnState = ReconnState::IDLE;
        }
        connected = true;
        return;
    }
    connected = false;

    // CONNECTING: Warte auf Ergebnis (non-blocking)
    if (_reconnState == ReconnState::CONNECTING) {
        if ((now - _reconnT0) < reconnectAttemptMs) return;  // noch warten
        // Timeout
        AWM_LOGW("❌ Wiederverbindung fehlgeschlagen.");
        _reconnState        = ReconnState::IDLE;
        ultimoIntentoWiFi   = now;

        if (fallbackPolicy == FallbackPolicy::SMART_RETRIES) {
            if (failWindowStart == 0 || (now - failWindowStart) > failWindowMs) {
                failWindowStart = now; failCount = 0;
            }
            ++failCount;
            AWM_LOGD("📉 SMART: %u/%u Fehler", failCount, maxFailRetries);
            if (failCount >= maxFailRetries) {
                AWM_LOGW("🚪 SMART: Portal öffnen");
                startAP(); failCount = 0; failWindowStart = 0;
            }
        }
        return;
    }

    // IDLE: Backoff abgelaufen → neuen Versuch starten
    if ((now - ultimoIntentoWiFi) < reconnectBackoffMs) return;
    if (!hasCredentials()) return;

    if (hostname.length()) WiFi.setHostname(hostname.c_str());
    if (portalActive || externalApActive) WiFi.mode(WIFI_AP_STA);
    else                                  WiFi.mode(WIFI_STA);

    WiFi.disconnect(false, false);
    applyStaConfigFromWifiJson();
    WiFi.begin(ssid.c_str(), password.c_str());
    _reconnState = ReconnState::CONNECTING;
    _reconnT0    = now;
    AWM_LOGI("🔄 Wiederverbindungsversuch gestartet…");
}

// ── isConnected / getSignalStrength / getTimestamp ───────────────────────────

bool ESPWiFiManagerAP::isConnected() { connected = (WiFi.status() == WL_CONNECTED); return connected; }
int  ESPWiFiManagerAP::getSignalStrength() { return WiFi.RSSI(); }

uint64_t ESPWiFiManagerAP::getTimestamp() {
    const time_t now = time(nullptr);
    return (now > 100000) ? static_cast<uint64_t>(now) * 1000ULL : 0;
}

// ── hasInternet ───────────────────────────────────────────────────────────────

bool ESPWiFiManagerAP::hasInternet() {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    http.begin(client, "http://clients3.google.com/generate_204");
    http.setConnectTimeout(3000);
    const int code = http.GET();
    http.end();
    return (code == 204);
}

// ── forceReconnect ────────────────────────────────────────────────────────────

void ESPWiFiManagerAP::forceReconnect() {
    AWM_LOGI("🔄 Erzwinge Wiederverbindung…");
    if (portalActive || externalApActive) WiFi.mode(WIFI_AP_STA);
    else                                  WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    _reconnState        = ReconnState::CONNECTING;
    _reconnT0           = millis();
    ultimoIntentoWiFi   = _reconnT0;
}

// ── hasCredentials ────────────────────────────────────────────────────────────

bool ESPWiFiManagerAP::hasCredentials() const {
    return !ssid.isEmpty() && !password.isEmpty();
}

// ── Credential-Helfer ─────────────────────────────────────────────────────────

// Config nur einmal aus NVS lesen/parsen (SSID, Passwort, Hostname)
void ESPWiFiManagerAP::loadCredentials() {
    JsonDocument d;
    const bool ok = loadConfigDoc(d);
    const String s = ok ? String(d["ssid"] | "") : String();
    const String p = ok ? String(d["password"] | "") : String();
    if (s.isEmpty() || p.isEmpty()) { AWM_LOGI("ℹ️ Keine Credentials gefunden."); return; }
    ssid = s; password = p;
    AWM_LOGI("✅ Credentials geladen (SSID=\"%s\")", ssid.c_str());

    const String h = d["hostname"] | "";
    if (h.length()) hostname = h;
}

void ESPWiFiManagerAP::eraseCredentials()  { ::eraseCredentials(); AWM_LOGI("🧹 Credentials gelöscht."); }
void ESPWiFiManagerAP::setProtectedJsons(std::initializer_list<const char*> n) { ::setProtectedJsons(n); }

// ── LED ───────────────────────────────────────────────────────────────────────

void ESPWiFiManagerAP::setLedAuto(bool en)                  { ledAuto = en; if (en) ledSet(LedPattern::OFF); }
void ESPWiFiManagerAP::setLedPatternManual(LedPattern p)    { ledAuto = false; ledSet(p); }
void ESPWiFiManagerAP::ledSet(LedPattern p)                 { ledPat = p; ledStep = 0; ledT0 = millis(); }

void ESPWiFiManagerAP::ledAutoUpdate() {
    if (!ledAuto) return;
    LedPattern want = LedPattern::OFF;
    if (asyncScanStarted)                     want = LedPattern::BLINK_FAST;
    else if (portalActive)                    want = LedPattern::BLINK_SLOW;
    else if (WiFi.status() == WL_CONNECTED)   want = LedPattern::ON;
    if (want != ledPat) ledSet(want);
}

void ESPWiFiManagerAP::ledTask() {
    const unsigned long now = millis();
    auto write = [&](uint8_t v) { if (ledOut != v) { ledOut = v; digitalWrite(ledPin, v); } };

    switch (ledPat) {
    case LedPattern::OFF:        write(LOW);  break;
    case LedPattern::ON:         write(HIGH); break;
    case LedPattern::BLINK_SLOW: write(((now - ledT0) % 1000) < 500 ? HIGH : LOW); break;
    case LedPattern::BLINK_FAST: write(((now - ledT0) %  200) < 100 ? HIGH : LOW); break;
    case LedPattern::BLINK_DOUBLE: {
        static const uint16_t seq[] = { 120,120,120,640 };
        static const uint8_t  on[]  = {   1,  0,  1,  0 };
        if (now - ledT0 >= seq[ledStep]) { ledT0 = now; ledStep = (ledStep + 1) % 4; write(on[ledStep] ? HIGH : LOW); }
        break;
    }
    case LedPattern::BLINK_TRIPLE: {
        static const uint16_t seq[] = { 100,100,100,100,100,500 };
        static const uint8_t  on[]  = {   1,  0,  1,  0,  1,  0 };
        if (now - ledT0 >= seq[ledStep]) { ledT0 = now; ledStep = (ledStep + 1) % 6; write(on[ledStep] ? HIGH : LOW); }
        break;
    }
    }
}

// ── mDNS ─────────────────────────────────────────────────────────────────────

bool ESPWiFiManagerAP::startMDNS(const String& host) {
    const String h = host.length() ? host : String("esp32");
    if (mdnsStarted) { MDNS.end(); mdnsStarted = false; }
    if (!MDNS.begin(h.c_str())) { AWM_LOGW("⚠️ MDNS Start fehlgeschlagen (%s)", h.c_str()); return false; }
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    mdnsHost    = h;
    AWM_LOGI("✅ MDNS aktiv: %s.local", h.c_str());
    return true;
}

void ESPWiFiManagerAP::stopMDNS() {
    if (!mdnsStarted) return;
    MDNS.end();
    mdnsStarted = false;
    mdnsHost    = "";
    AWM_LOGI("✅ MDNS gestoppt");
}