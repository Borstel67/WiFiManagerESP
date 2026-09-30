# Copilot Instructions

## Projektrichtlinien
- Bevorzuge, dass gemeinsame Helper-Funktionen in `ESPWiFiManagerCommon.h/.cpp` verwendet werden; die gemeinsame Implementierung soll die erforderlichen Plattformabhängigen Headers (z.B. WiFi) selbst einbinden, wenn sie WiFi-APIs verwendet.