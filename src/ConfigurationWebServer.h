#pragma once

// WiFiManager.h must be included before ESPAsyncWebServer.h - ESPAsyncWebServer's
// HTTP method enum only reuses WebServer.h's definitions if WEBSERVER_H is already defined
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>

class ConfigurationWebServer {
private:
    AsyncWebServer server;
    Preferences prefs;
    WiFiManager* wifiManager = nullptr;

public:
    ConfigurationWebServer() : server(80), prefs() {}
    ConfigurationWebServer(int port) : server(port), prefs() {}

    void Initialise();
    [[nodiscard]] const String GetStoredString(const char* key);
    void SetStoredString(const char* key, const String& value);
    void AttachWiFiManager(WiFiManager& manager) { wifiManager = &manager; }
};