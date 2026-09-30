// SPDX-License-Identifier: MIT
// WiFiManagerESP STA-Webserver für Betrieb im verbundenen STA-Modus:
// - HTTP-Routen für UI/Assets
// - API für STA-Konfiguration (DHCP/static, DNS, Hostname, NTP)
// - OTA via ElegantOTA
#ifndef ESP_WIFI_MANAGER_STA_H
#define ESP_WIFI_MANAGER_STA_H

#include <Arduino.h>

#if defined(ESP32)
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#else
#error "Plattform nicht unterstützt (nur ESP32)"
#endif

#include <FS.h>
#include <LittleFS.h>

class ESPWiFiManagerSTA {
public:
	ESPWiFiManagerSTA();

	bool startSTA(uint16_t port = 80);
	void stopSTA();
	void handleClient();

	void setHtmlPathPrefix(const String& prefix);
	void setHostname(const String& host);
	void enableDhcp(bool enabled);
	void setStaticIP(const IPAddress& ip, const IPAddress& gateway, const IPAddress& subnet,
		const IPAddress& dns1 = IPAddress(0, 0, 0, 0), const IPAddress& dns2 = IPAddress(0, 0, 0, 0));

	bool isRunning() const { return serverRunning; }
	uint16_t getPort() const { return serverPort; }

private:
	void setupHTTPRoutes();
	void handleRoot(AsyncWebServerRequest* request);
	void handleCss(AsyncWebServerRequest* request);
	void handleNotFound(AsyncWebServerRequest* request);
	void handleSettings(AsyncWebServerRequest* request);

	void handleStaConfigGet(AsyncWebServerRequest* request);
	void handleStaConfigSave(AsyncWebServerRequest* request);
	bool saveStaConfigToJson(bool useDhcp,
		const String& ip,
		const String& gw,
		const String& sn,
		const String& dns1,
		const String& dns2,
		const String& host,
		const String& ntp1,
		const String& ntp2);
	void handleErase(AsyncWebServerRequest* request);

	void loadCredentials();
	bool parseIP(const String& text, IPAddress& out);

	AsyncWebServer   staServer;

	String  htmlPathPrefix;
	String  hostname;
	uint16_t serverPort;
	bool    serverRunning;
	bool    routesReady;

	// Deferred Restart (aus Async-Handler angefordert, in handleClient() ausgeführt)
	volatile bool     restartPending;
	volatile uint32_t restartReqAt;

	bool     dhcpEnabled;
	IPAddress staticIP;
	IPAddress staticGW;
	IPAddress staticSN;
	IPAddress staticDNS1;
	IPAddress staticDNS2;
	String ntp1;
	String ntp2;
};

#endif // ESP_WIFI_MANAGER_STA_H
