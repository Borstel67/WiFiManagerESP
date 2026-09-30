// SPDX-License-Identifier: MIT
// WiFiManagerESP STA-Webserver-Implementierung:
// - Start nur bei bestehender STA-Verbindung
// - Routen für UI/Assets und API (/api/sta/config)
// - OTA via ElegantOTA
#include "ESPWiFiManagerSTA.h"
#include "AWM_Logging.h"
#include "ESPWiFiManagerCommon.h"
#include <ArduinoJson.h>

#ifndef ELEGANTOTA_USE_ASYNC_WEBSERVER
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#endif
#include <ElegantOTA.h>

ESPWiFiManagerSTA::ESPWiFiManagerSTA()
	: staServer(80)
	, htmlPathPrefix("/")
	, serverPort(80)
	, serverRunning(false)
	, routesReady(false)
	, restartPending(false)
	, restartReqAt(0)
	, dhcpEnabled(true)
	, staticIP(0, 0, 0, 0)
	, staticGW(0, 0, 0, 0)
	, staticSN(0, 0, 0, 0)
	, staticDNS1(0, 0, 0, 0)
	, staticDNS2(0, 0, 0, 0)
	, ntp1("")
	, ntp2("") {}

void ESPWiFiManagerSTA::setHtmlPathPrefix(const String& prefix) { htmlPathPrefix = prefix.endsWith("/") ? prefix : prefix + "/"; }
void ESPWiFiManagerSTA::setHostname(const String& host) { hostname = host; }
void ESPWiFiManagerSTA::enableDhcp(bool enabled) { dhcpEnabled = enabled; }
void ESPWiFiManagerSTA::setStaticIP(const IPAddress& ip, const IPAddress& gateway, const IPAddress& subnet,
const IPAddress& dns1, const IPAddress& dns2) {
	staticIP = ip; staticGW = gateway; staticSN = subnet; staticDNS1 = dns1; staticDNS2 = dns2; dhcpEnabled = false;
}

// Startet den STA-Webserver (nur wenn bereits verbunden); richtet Routen und OTA ein.
// Hinweis: Der Port ist durch den Konstruktor auf 80 festgelegt (AsyncWebServer-Limit).
bool ESPWiFiManagerSTA::startSTA(uint16_t port) {
	if (serverRunning) return true;
	if (WiFi.status() != WL_CONNECTED) { AWM_LOGE("❌ Nicht mit WiFi verbunden. STA-Server kann nicht gestartet werden."); return false; }

	loadCredentials();   // NTP-Sync erfolgt bereits beim Verbindungsaufbau

	serverPort = port;
	if (!routesReady) {
		setupHTTPRoutes();
		ElegantOTA.begin(&staServer);
		routesReady = true;
	}
	staServer.begin();

	serverRunning = true;
	AWM_LOGI("🌐 STA HTTP-Server gestartet auf http://%s:%u", WiFi.localIP().toString().c_str(), (unsigned)serverPort);
	AWM_LOGI("🛠  ElegantOTA aktiv (STA) unter /update");
	return true;
}

// Stoppt den Webserver (gibt Port 80 z. B. für das Captive-Portal frei)
void ESPWiFiManagerSTA::stopSTA() {
	if (!serverRunning) return;
	staServer.end();
	serverRunning = false;
	AWM_LOGI("✅ STA HTTP-Server gestoppt");
}

// Im loop() aufrufen: führt verzögerten Neustart nach /erase aus
void ESPWiFiManagerSTA::handleClient() {
	if (restartPending && millis() - restartReqAt >= 500) ESP.restart();
}

// HTTP-Routen für UI/Assets/API
void ESPWiFiManagerSTA::setupHTTPRoutes() {
	staServer.on("/", HTTP_GET, [this](AsyncWebServerRequest* r) { handleRoot(r); });
	staServer.on("/WifiManager.css", HTTP_GET, [this](AsyncWebServerRequest* r) { handleCss(r); });
	staServer.on("/settings", HTTP_GET, [this](AsyncWebServerRequest* r) { handleSettings(r); });
	staServer.on("/settingsSTA.html", HTTP_GET, [this](AsyncWebServerRequest* r) { handleSettings(r); });
	staServer.on("/api/sta/config", HTTP_GET, [this](AsyncWebServerRequest* r) { handleStaConfigGet(r); });
	staServer.on("/api/sta/config", HTTP_POST, [this](AsyncWebServerRequest* r) { handleStaConfigSave(r); });

	staServer.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r) {
		AsyncWebServerResponse* res = r->beginResponse(204);
		res->addHeader("Cache-Control", "public, max-age=86400");
		r->send(res);
	});

	staServer.on("/erase", HTTP_POST, [this](AsyncWebServerRequest* r) { handleErase(r); });
	staServer.onNotFound([this](AsyncWebServerRequest* r) { handleNotFound(r); });
	AWM_LOGD("HTTP-Routen für STA-Server eingerichtet");
}

// Liefert indexSTA.html
void ESPWiFiManagerSTA::handleRoot(AsyncWebServerRequest* request) {
	String path = htmlPathPrefix + "indexSTA.html";
	if (!serveFileFromLittleFS(request, path, "text/html")) {
		AWM_LOGE("❌ Datei nicht gefunden/öffnen: %s", path.c_str());
		request->send(500, "text/html", "<h1>Fehler: indexSTA.html nicht gefunden</h1>");
		return;
	}
}

// Liefert settings.html (gemeinsam für AP und STA)
void ESPWiFiManagerSTA::handleSettings(AsyncWebServerRequest* request) {
	String path = htmlPathPrefix + "settings.html";
	if (!serveFileFromLittleFS(request, path, "text/html")) {
		request->send(500, "text/html", "<h1>Fehler: settings.html nicht gefunden</h1>");
		return;
	}
}

// Liefert WifiManager.css (oder 404)
void ESPWiFiManagerSTA::handleCss(AsyncWebServerRequest* request) {
	String pathcss = htmlPathPrefix + "WifiManager.css";
	if (!serveFileFromLittleFS(request, pathcss, "text/css", true)) {
		request->send(404, "text/css", "");
		return;
	}
}

// 404-Handler mit Basisdiagnose
void ESPWiFiManagerSTA::handleNotFound(AsyncWebServerRequest* request) {
	String message = "404 - Seite nicht gefunden\n\n";
	message += "URI: " + request->url() + "\n";
	message += "Methode: ";
	message += (request->method() == HTTP_GET) ? "GET" : "POST";
	message += "\n";
	request->send(404, "text/plain", message);
	AWM_LOGW("⚠️ 404: %s", request->url().c_str());
}

// Lädt STA-bezogene Parameter aus /config.json (DHCP/static, DNS, Hostname, NTP)
void ESPWiFiManagerSTA::loadCredentials() {
	JsonDocument doc;
	if (!loadConfigDoc(doc)) { AWM_LOGI("ℹ️ Keine gültige Config in NVS."); return; }

	if (doc["dhcp"].is<bool>()) dhcpEnabled = doc["dhcp"].as<bool>();

	String ip = doc["ip"] | "";
	String gw = doc["gw"] | "";
	String sn = doc["sn"] | "";
	String dns1 = doc["dns1"] | "";
	String dns2 = doc["dns2"] | "";

	if (ip.length())   parseIP(ip, staticIP);
	if (gw.length())   parseIP(gw, staticGW);
	if (sn.length())   parseIP(sn, staticSN);
	if (dns1.length()) parseIP(dns1, staticDNS1);
	if (dns2.length()) parseIP(dns2, staticDNS2);

	String h = doc["hostname"] | "";
	if (h.length()) hostname = h;

	ntp1 = doc["ntp1"] | "";
	ntp2 = doc["ntp2"] | "";

	AWM_LOGI("⚙️ STA Netzwerk: DHCP=%s IP=%s", dhcpEnabled ? "ja" : "nein", staticIP.toString().c_str());
}

// Delegiert IP-Parsing an Common-Helper
bool ESPWiFiManagerSTA::parseIP(const String& text, IPAddress& out) { return ::parseIpText(text, out); }

// GET /api/sta/config: liefert aktuelle STA-Einstellungen
void ESPWiFiManagerSTA::handleStaConfigGet(AsyncWebServerRequest* request) {
	loadCredentials();

	// Nicht gesetzte Adressen als "" liefern (sonst steht "0.0.0.0" im Formular)
	auto ipStr = [](const IPAddress& a) { return a == IPAddress(0, 0, 0, 0) ? String() : a.toString(); };

	JsonDocument doc;
	doc["ssid"] = WiFi.SSID();
	doc["rssi"] = WiFi.RSSI();
	doc["dhcp"] = dhcpEnabled;
	doc["ip"] = ipStr(staticIP);
	doc["gw"] = ipStr(staticGW);
	doc["sn"] = ipStr(staticSN);
	doc["dns1"] = ipStr(staticDNS1);
	doc["dns2"] = ipStr(staticDNS2);
	doc["hostname"] = hostname;
	doc["ntp1"] = ntp1;
	doc["ntp2"] = ntp2;

	String out;
	serializeJson(doc, out);
	request->send(200, "application/json", out);
}

// POST /api/sta/config: validiert Eingaben, speichert nach /config.json
void ESPWiFiManagerSTA::handleStaConfigSave(AsyncWebServerRequest* request) {
	String dhcpRaw = request->arg("dhcp");
	dhcpRaw.toLowerCase();
	const bool useDhcp = (dhcpRaw == "1" || dhcpRaw == "true" || dhcpRaw == "on");

	String ip = request->arg("ip");
	String gw = request->arg("gw");
	String sn = request->arg("sn");
	String dns1 = request->arg("dns1");
	String dns2 = request->arg("dns2");
	String host = request->arg("hostname");
	String ntp1v = request->arg("ntp1");
	String ntp2v = request->arg("ntp2");

	if (!useDhcp) {
		IPAddress tmp;
		if (!parseIP(ip, tmp) || !parseIP(gw, tmp) || !parseIP(sn, tmp)) {
			request->send(400, "application/json", "{\"ok\":false,\"error\":\"Ungültige IP/Gateway/Subnetz\"}");
			return;
		}
		if (dns1.length() && !parseIP(dns1, tmp)) { request->send(400, "application/json", "{\"ok\":false,\"error\":\"Ungültige DNS1\"}"); return; }
		if (dns2.length() && !parseIP(dns2, tmp)) { request->send(400, "application/json", "{\"ok\":false,\"error\":\"Ungültige DNS2\"}"); return; }
	}

	if (!saveStaConfigToJson(useDhcp, ip, gw, sn, dns1, dns2, host, ntp1v, ntp2v)) {
		request->send(500, "application/json", "{\"ok\":false,\"error\":\"Speichern fehlgeschlagen\"}");
		return;
	}

	request->send(200, "application/json", "{\"ok\":true}");
}

// Persistiert STA-Parameter in /config.json; erhält andere Keys falls vorhanden
bool ESPWiFiManagerSTA::saveStaConfigToJson(bool useDhcp,
	const String& ip,
	const String& gw,
	const String& sn,
	const String& dns1,
	const String& dns2,
	const String& host,
	const String& ntp1v,
	const String& ntp2v) {

	JsonDocument doc;
	loadConfigDoc(doc);   // vorhandene Keys (ssid/password) erhalten

	doc["dhcp"] = useDhcp;
	if (useDhcp) {
		doc.remove("ip"); doc.remove("gw"); doc.remove("sn"); doc.remove("dns1"); doc.remove("dns2");
	} else {
		doc["ip"] = ip; doc["gw"] = gw; doc["sn"] = sn; doc["dns1"] = dns1; doc["dns2"] = dns2;
	}
	doc["hostname"] = host;
	doc["ntp1"] = ntp1v;
	doc["ntp2"] = ntp2v;

	String out;
	serializeJson(doc, out);
	if (!saveConfigJson(out)) { AWM_LOGE("❌ Konnte /config.json nicht schreiben"); return false; }
	AWM_LOGI("✅ STA-Netzwerkparameter in /config.json gespeichert (DHCP=%s)", useDhcp ? "ja" : "nein");
	return true;
}

// POST /erase: nutzt Common-Erase; Neustart erfolgt verzögert in handleClient(),
// damit die Antwort noch ausgeliefert wird (kein delay() im AsyncTCP-Task)
void ESPWiFiManagerSTA::handleErase(AsyncWebServerRequest* request) {
	request->send(200, "application/json", "{\"ok\":true}");
	::eraseCredentials();
	restartReqAt = millis();
	restartPending = true;
}