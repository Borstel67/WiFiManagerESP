// SPDX-License-Identifier: MIT
// WiFiManagerESP gemeinsame Utilities
#include "ESPWiFiManagerCommon.h"
#include "AWM_Logging.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <time.h>

static std::vector<String> _protectedExact;
static bool     _otaStarted = false;
static uint16_t _otaPort    = 3232;

static const char* kNvsNs  = "awm";
static const char* kNvsKey = "config";

// ── NVS-Helpers ─────────────────────────────────────────────────────────────

bool loadConfigJson(String& jsonOut) {
    Preferences p;
    if (!p.begin(kNvsNs, true)) return false;
    jsonOut = p.getString(kNvsKey, "");
    p.end();
    return jsonOut.length() > 0;
}

bool saveConfigJson(const String& jsonText) {
    Preferences p;
    if (!p.begin(kNvsNs, false)) return false;
    size_t w = p.putString(kNvsKey, jsonText);
    p.end();
    return (w > 0) || jsonText.isEmpty();
}

void eraseConfigJson() {
    Preferences p;
    if (p.begin(kNvsNs, false)) { p.remove(kNvsKey); p.end(); }
}

bool loadConfigDoc(JsonDocument& doc) {
    String json;
    if (!loadConfigJson(json)) return false;
    return !deserializeJson(doc, json);
}

// ── IP-Parsing ───────────────────────────────────────────────────────────────

// IPAddress::fromString prüft Oktett-Bereich (0–255) und Format
bool parseIpText(const String& text, IPAddress& out) {
    IPAddress tmp;
    if (!text.length() || !tmp.fromString(text)) return false;
    out = tmp;
    return true;
}

// ── STA-Netzwerkkonfiguration ─────────────────────────────────────────────────

void applyStaConfigFromWifiJson() {
    bool dhcp = true;
    IPAddress ip, gw, sn, dns1, dns2;

    JsonDocument doc;
    if (loadConfigDoc(doc)) {
        dhcp = doc["dhcp"] | true;
        parseIpText(doc["ip"]   | "", ip);
        parseIpText(doc["gw"]   | "", gw);
        parseIpText(doc["sn"]   | "", sn);
        parseIpText(doc["dns1"] | "", dns1);
        parseIpText(doc["dns2"] | "", dns2);
    }

    if (dhcp) {
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    } else if (ip != IPAddress(0, 0, 0, 0)) {
        if (!WiFi.config(ip, gw, sn, dns1, dns2))
            WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE); // Fallback DHCP
    }
}

// ── Credential-Storage ────────────────────────────────────────────────────────

void loadCredentials(String& ssid, String& password) {
    JsonDocument doc;
    if (!loadConfigDoc(doc)) return;
    ssid     = doc["ssid"]     | "";
    password = doc["password"] | "";
}

void saveCredentials(const String& ssid, const String& password) {
    JsonDocument doc;
    loadConfigDoc(doc);            // vorhandene Keys erhalten
    doc["ssid"]     = ssid;
    doc["password"] = password;
    String out;
    serializeJson(doc, out);
    saveConfigJson(out);
}

void eraseCredentials() {
    eraseConfigJson();
}

// ── Whitelist-Verwaltung ──────────────────────────────────────────────────────

void setProtectedJsons(std::initializer_list<const char*> names) {
    _protectedExact.clear();
    for (auto n : names) {
        String s(n ? n : "");
        if (!s.length()) continue;
        if (!s.startsWith("/")) s = "/" + s;
        _protectedExact.push_back(s);
    }
}

bool isProtectedJson(const String& name) {
    String n = name;
    if (!n.startsWith("/")) n = "/" + n;
    for (const auto& ex : _protectedExact)
        if (n.equalsIgnoreCase(ex)) return true;
    return false;
}

void eraseJsonInDir(const char* dirPath) {
    if (!dirPath || !*dirPath) return;
    File dir = LittleFS.open(dirPath);
    if (!dir || !dir.isDirectory()) return;
    String base = dirPath;
    if (!base.endsWith("/")) base += "/";
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name = f.name();
        String full = name.startsWith("/") ? name : base + name;
        const bool isDir = f.isDirectory();
        f.close();
        if (isDir) eraseJsonInDir(full.c_str());
        else if (full.endsWith(".json") && !isProtectedJson(full)) LittleFS.remove(full);
    }
    dir.close();
}

// ── NTP-Synchronisation ───────────────────────────────────────────────────────

bool syncTimeDefault(uint8_t tries) {
    String n1 = "pool.ntp.org", n2 = "time.nist.gov";
    JsonDocument d;
    if (loadConfigDoc(d)) {
        // Leere Strings (Formular ohne Eingabe) → Default behalten
        const String c1 = d["ntp1"] | "", c2 = d["ntp2"] | "";
        if (c1.length()) n1 = c1;
        if (c2.length()) n2 = c2;
    }
    configTime(0, 0, n1.c_str(), n2.c_str());
    for (uint8_t i = 0; i < tries; ++i) {
        if (time(nullptr) > 100000) return true;
        delay(200);
    }
    return false;
}

// ── ArduinoOTA ────────────────────────────────────────────────────────────────

void ensureArduinoOta(const String& hostname, uint16_t port, const String& password) {
    if (WiFi.getMode() == WIFI_OFF) return;
    if (_otaStarted) return;

    String host = hostname;
    if (!host.length()) host = WiFi.getHostname();
    if (!host.length()) host = "esp32";

    _otaPort = (port == 0) ? 3232 : port;
    ArduinoOTA.setPort(_otaPort);
    ArduinoOTA.setHostname(host.c_str());
    if (password.length()) ArduinoOTA.setPassword(password.c_str());

    ArduinoOTA.onStart([]()  { AWM_LOGI("[OTA] Start"); });
    ArduinoOTA.onEnd([]()    { AWM_LOGI("[OTA] Ende"); });
    ArduinoOTA.onError([](ota_error_t e) { AWM_LOGE("[OTA] Fehler %u", (unsigned)e); });

    ArduinoOTA.begin();
    _otaStarted = true;
    AWM_LOGI("[OTA] Bereit: %s:%u", host.c_str(), (unsigned)_otaPort);
}

void handleArduinoOta() {
    if (_otaStarted) ArduinoOTA.handle();
}

// ── Partition-Check ───────────────────────────────────────────────────────────

bool checkOtaAndDataPartitions(bool logDetails) {
    size_t otaCount = 0, dataFsCount = 0;
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot    = esp_ota_get_boot_partition();

    auto it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t* p = esp_partition_get(it);
        if (p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
            p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) otaCount++;
        if (logDetails)
            AWM_LOGI("[PART] APP  %-12s sub=0x%02x size=%.1fk @0x%06x %s%s",
                p->label, p->subtype, p->size / 1024.0f, p->address,
                p == running ? "(running)" : "", p == boot ? "(boot)" : "");
    }
    esp_partition_iterator_release(it);

    it = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t* p = esp_partition_get(it);
        const bool isFs = (p->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) ||
                          (p->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT)    ||
                          !strncmp(p->label, "littlefs", 8)                 ||
                          !strncmp(p->label, "spiffs", 6)                   ||
                          !strncmp(p->label, "ffat", 4);
        if (isFs) dataFsCount++;
        if (logDetails)
            AWM_LOGI("[PART] DATA %-12s sub=0x%02x size=%.1fk @0x%06x %s",
                p->label, p->subtype, p->size / 1024.0f, p->address, isFs ? "(FS)" : "");
    }
    esp_partition_iterator_release(it);

    const bool ok = (otaCount >= 2) && (dataFsCount >= 1);
    if (logDetails)
        AWM_LOGI("[PART] OTA-Slots=%u DATA-FS=%u → %s",
            (unsigned)otaCount, (unsigned)dataFsCount, ok ? "OK" : "NICHT OK");
    return ok;
}

// ── HTTP-Helpers ──────────────────────────────────────────────────────────────

// Gemeinsame 302-Umleitung auf Root (no-cache)
void redirectToRoot(AsyncWebServerRequest* request) {
    if (!request) return;
    auto* res = request->beginResponse(302, "text/plain", "");
    res->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    res->addHeader("Pragma",   "no-cache");
    res->addHeader("Expires",  "0");
    res->addHeader("Location", "/", true);
    request->send(res);
}

bool serveFileFromLittleFS(AsyncWebServerRequest* request, const String& path,
                           const String& contentType, bool cacheable) {
    if (!request || !LittleFS.exists(path)) return false;
    AsyncWebServerResponse* res = request->beginResponse(LittleFS, path, contentType);
    if (!res) return false;
    res->addHeader("Cache-Control",
        cacheable ? "public, max-age=86400" : "no-cache, no-store, must-revalidate");
    request->send(res);
    return true;
}