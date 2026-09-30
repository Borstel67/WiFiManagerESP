// SPDX-License-Identifier: MIT
// WiFiManagerESP AP-/Captive-Portal-Manager:
// - SoftAP + DNS-Catch-All + HTTP-Routen für Provisioning
// - Fallback-Strategien und nicht-blockierender Auto-Reconnect
// - Button-/LED-Handling, asynchroner WiFi-Scan, OTA (ElegantOTA), mDNS
#ifndef WIFI_MANAGER_AP_H
#define WIFI_MANAGER_AP_H

#define AWM_VERSION        "2.0.4"
#define AWM_VERSION_MAJOR  2
#define AWM_VERSION_MINOR  0
#define AWM_VERSION_PATCH  4

#include <Arduino.h>
#if defined(ESP32)
#  include <WiFi.h>
#  include <ESPAsyncWebServer.h>
#  include <AsyncTCP.h>
#else
#  error "Plattform nicht unterstützt (nur ESP32)"
#endif
#include <FS.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <vector>
#include <initializer_list>

class ESPWiFiManagerAP {
public:
    enum class FallbackPolicy : uint8_t {
        ON_FAIL, NO_CREDENTIALS_ONLY, SMART_RETRIES, BUTTON_ONLY, NEVER
    };
    enum class LedPattern : uint8_t {
        OFF, ON, BLINK_SLOW, BLINK_FAST, BLINK_DOUBLE, BLINK_TRIPLE
    };

    explicit ESPWiFiManagerAP(uint8_t ledPin = 2, uint8_t buttonPin = 0);

    // Lebenszyklus
    void begin();
    void run();
    void update();

    // Konfiguration
    void setHtmlPathPrefix(const String& prefix);
    void setHostname(const String& host);
    void setAPCredentials(const String& ssid, const String& pass);
    void setCaptivePortal(bool enabled);
    void setPortalTimeout(uint32_t seconds);
    void setAPClientCheck(bool enabled);
    void setWebClientCheck(bool enabled);
    void setFallbackPolicy(FallbackPolicy p);
    void setSmartRetries(uint8_t maxRetries, uint32_t windowMs);
    void enableButtonPortal(bool enable);
    void setAutoReconnect(bool enable);
    void setReconnectBackoffMs(uint32_t ms);
    void setReconnectAttemptMs(uint32_t ms);
    void setExternalApActive(bool active);

    // Portal-Steuerung
    void openPortal();
    void closePortal();
    bool isPortalActive() const;
    bool isExternalApActive() const;

    // Verbindungsstatus
    bool     isConnected();
    int      getSignalStrength();
    uint64_t getTimestamp();
    bool     hasInternet();
    bool     hasCredentials() const;
    bool     connectToWiFiSTA();
    void     checkReconnect();
    void     forceReconnect();

    // LED
    void setLedAuto(bool enable);
    void setLedPatternManual(LedPattern p);

    // Datei-Schutz
    void setProtectedJsons(std::initializer_list<const char*> names);

    // mDNS
    bool startMDNS(const String& host);
    void stopMDNS();

private:
    // ── Reconnect-Zustandsmaschine ──
    enum class ReconnState : uint8_t { IDLE, CONNECTING };
    ReconnState   _reconnState   = ReconnState::IDLE;
    uint32_t      _reconnT0      = 0;

    // ── Deferred Restart (aus Async-Handlern angefordert, in update() ausgeführt) ──
    volatile bool     _restartPending = false;
    volatile uint32_t _restartReqAt   = 0;
    void scheduleRestart();

    // ── AP / DNS ──
    void setupAP();
    void startAP();
    void stopAP();
    void setupHTTPRoutes();
    void startDNS();
    void stopDNS();
    bool captivePortalRedirect(AsyncWebServerRequest* request);
    bool portalHasTimedOut();
    uint8_t softAPStationCount();

    // ── HTTP-Handler ──
    void handleRoot(AsyncWebServerRequest* request);
    void handleSettings(AsyncWebServerRequest* request);
    void handleSave(AsyncWebServerRequest* request);
    void handleScan(AsyncWebServerRequest* request);
    void handleScanResult(AsyncWebServerRequest* request);
    void sendScanResults(AsyncWebServerRequest* request, int count);
    void handleApiApConfig(AsyncWebServerRequest* request);
    void handleNotFound(AsyncWebServerRequest* request);
    void handleCss(AsyncWebServerRequest* request);
    void handleErase(AsyncWebServerRequest* request);
    void showErrorPage(const String& message, AsyncWebServerRequest* request);

    // ── Credentials ──
    void loadCredentials();
    void eraseCredentials();

    // ── LED ──
    void ledAutoUpdate();
    void ledTask();
    void ledSet(LedPattern p);

    // ── Member-Variablen ──
    String ssid, password;
    String htmlPathPrefix = "/";

    AsyncWebServer apServer{ 80 };
    DNSServer      dns;
    bool portalActive = false;
    bool dnsRunning   = false;
    bool routesReady  = false;   // Routen nur einmal registrieren (sonst Duplikate je Portal-Start)

    IPAddress apIP{ 192,168,4,1 }, apGW{ 192,168,4,1 }, apSN{ 255,255,255,0 };
    String hostname;
    String apSSID = "WiFi Manager";
    String apPASS = "123456789";

    bool     captiveEnabled  = true;
    uint32_t portalTimeoutMs = 0;
    bool     apClientCheck   = false;
    bool     webClientCheck  = true;
    unsigned long portalStart    = 0;
    unsigned long lastHttpAccess = 0;

    FallbackPolicy fallbackPolicy    = FallbackPolicy::NO_CREDENTIALS_ONLY;
    bool           allowButtonPortal = true;
    uint8_t        maxFailRetries    = 3;
    uint32_t       failWindowMs      = 60000;
    uint8_t        failCount         = 0;
    unsigned long  failWindowStart   = 0;

    bool          connected          = false;
    bool          autoReconnect      = true;
    unsigned long ultimoIntentoWiFi  = 0;

    // ── Async-Scan ──
    volatile bool asyncScanStarted   = false;
    volatile unsigned long scanningUntil = 0;   // Timeout des laufenden Async-Scans (0 = kein Scan)
    static constexpr unsigned long SCAN_TIMEOUT_MS  = 10000;
    bool scanTimedOut() const;
    void resetScan();

    // ── LED ──
    uint8_t       ledPin, buttonPin;
    bool          ledAuto  = true;
    LedPattern    ledPat   = LedPattern::OFF;
    uint8_t       ledOut   = LOW;
    uint8_t       ledStep  = 0;
    unsigned long ledT0    = 0;

    uint32_t reconnectBackoffMs  = 10000;
    uint32_t reconnectAttemptMs  = 5000;
    bool     externalApActive    = false;

    // ── mDNS ──
    bool   mdnsStarted = false;
    String mdnsHost;
};

#endif // WIFI_MANAGER_AP_H
