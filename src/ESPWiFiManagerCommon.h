// SPDX-License-Identifier: MIT
// Gemeinsame Utilities für WiFiManagerESP (AP- und STA-Manager):
// - IP-Parsing und STA-Netzkonfiguration aus NVS (/config)
// - Credential-Storage (load/save/erase) in NVS
// - Geschützte JSON-Whitelist und rekursive Bereinigung (LittleFS)
// - NTP-Synchronisation mit Fallback-Servern
// - Gemeinsamer File-Streamer von LittleFS zu AsyncWebServer-Instanzen
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <IPAddress.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <vector>
#include <initializer_list>

// IP-Parsing aus Text ("a.b.c.d") nach IPAddress
bool parseIpText(const String& text, IPAddress& out);

// STA-Konfiguration aus NVS anwenden (DHCP oder statische IP)
void applyStaConfigFromWifiJson();

// Zugangsdaten (SSID/PW) laden/speichern/löschen in NVS
void loadCredentials(String& ssid, String& password);
void saveCredentials(const String& ssid, const String& password);
void eraseCredentials();

// NVS-Storage für komplette Config (JSON-String)
bool loadConfigJson(String& jsonOut);
bool saveConfigJson(const String& jsonText);
void eraseConfigJson();
// Config aus NVS laden und deserialisieren; false bei fehlender/ungültiger Config
bool loadConfigDoc(JsonDocument& doc);

// Geschützte JSON-Dateien setzen/prüfen und .json rekursiv löschen
void setProtectedJsons(std::initializer_list<const char*> names);
bool isProtectedJson(const String& name);
void eraseJsonInDir(const char* dirPath);

// NTP-Sync mit Standard- oder in NVS hinterlegten Servern
bool syncTimeDefault(uint8_t tries = 20);

// ArduinoOTA: Lazy-Init/Handler sowie Partition-Check
void ensureArduinoOta(const String& hostname, uint16_t port, const String& password);
void handleArduinoOta();
bool checkOtaAndDataPartitions(bool logDetails = false);

// HTTP 302 → Root (no-cache); gemeinsam für AP und STA
void redirectToRoot(AsyncWebServerRequest* request);

// AsyncWebServer-Variante: Datei aus LittleFS streamen; optional Cache-Header
bool serveFileFromLittleFS(AsyncWebServerRequest* request, const String& path,
                           const String& contentType, bool cacheable = false);

// Template-Variante für klassischen WebServer
template<typename TServer>
bool serveFileFromLittleFS(TServer& server, const String& path,
                           const String& contentType, bool cacheable = false) {
    if (!LittleFS.exists(path)) return false;
    File file = LittleFS.open(path, "r");
    if (!file || file.isDirectory()) return false;
    server.sendHeader("Cache-Control",
        cacheable ? "public, max-age=86400" : "no-cache, no-store, must-revalidate");
    server.streamFile(file, contentType);
    file.close();
    return true;
}
