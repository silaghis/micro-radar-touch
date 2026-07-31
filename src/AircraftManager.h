#pragma once

#include <map>
#include <vector>

#include "models/TrackedAircraft.h"
#include "ConfigurationWebServer.h"
#include "OpenSkyAuthTokenHandler.h"
#include "LGFX.h"

enum class Gesture
{
    None,
    Tap,
    DoubleTap,
    TripleTap,
    LongPress,
    SwipeUp,
    SwipeDown
};

enum class UiMode
{
    Radar,
    List,
    Detail
};

enum class UnitSystem
{
    Metric,
    Aviation,
    None
};

class AircraftManager
{
private:
    double lat = 0.0;
    double lon = 0.0;
    double rad = 0.2;
    std::map<String, TrackedAircraft> trackedAircraft;

    bool displayInfoText = true;
    bool displayTriangles = true;
    bool zoomTapEnabled = true;
    bool listSwipeEnabled = true;
    bool lockEnabled = true;
    bool heatmapEnabled = false;
    bool typeIconEnabled = false;
    UnitSystem unitSystem = UnitSystem::Metric;

    UiMode uiMode = UiMode::Radar;
    String selectedIcao;
    String lockedIcao;
    int listScrollIndex = 0;

    // gesture recognition
    bool touchWasDown = false;
    int32_t touchStartX = 0;
    int32_t touchStartY = 0;
    int32_t touchLastX = 0;
    int32_t touchLastY = 0;
    unsigned long touchStartTime = 0;
    int tapStreak = 0;
    unsigned long lastTapTime = 0;

    std::vector<double> zoomPresets;

    unsigned long fetchInterval = 0;
    unsigned long lastFetch = 999999;
    unsigned long lastManualRefresh = 0;
    unsigned long refreshFlashUntil = 0;
    unsigned long zoomLabelUntil = 0;

    ConfigurationWebServer& configServer;
    OpenSkyAuthTokenHandler& authHandler;
    HttpRequestManager& http;
    LGFX& tft;

    Gesture PollGesture();
    void HandleRadarGesture(Gesture gesture);
    void HandleListGesture(Gesture gesture);
    void HandleDetailGesture(Gesture gesture);
    void CycleZoom(bool zoomOut);
    void RequestManualRefresh();
    void MergeAircraftStates(const std::vector<Aircraft>& aircraft, unsigned long now);
    bool FetchAircraftStates(const std::vector<std::pair<String, String>>& params, const std::vector<std::pair<String, String>>& headers, std::vector<Aircraft>& outAircraft, const char* context);
    std::vector<String> GetSortedVisibleIcaos() const;
    int HitTestListRow(int touchY) const;
    bool TryHitLockBadge(int touchY) const;

    String FormatMeasurement(float metricValue, float aviationValue, const char* metricUnit, const char* aviationUnit) const;
    String FormatAltitude(float metres) const;
    String FormatSpeed(float metersPerSecond) const;
    String FormatVerticalRate(float metersPerSecond) const;
    String FormatRingDistance(double radiusDegrees) const;
    String FormatZoomLabel() const;

    void DrawRadarScreen(LGFX_Sprite& backbuffer);
    void DrawRadarCircles(LGFX_Sprite& backbuffer) const;
    std::pair<int, int> ProjectCoordinateToScreen(float predLat, float predLon) const;
    void DrawAircraftInfo(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const;
    void DrawAircraftMarker(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const;
    void DrawAircraftTriangle(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked, uint32_t color, float scale) const;
    void DrawLockBadge(LGFX_Sprite& backbuffer) const;
    bool TrySelectAircraftAtTouch(int touchX, int touchY);
    void DrawSelectedAircraftDetails(LGFX_Sprite& backbuffer, const TrackedAircraft& tracked, const String& icao) const;
    void DrawAircraftList(LGFX_Sprite& backbuffer) const;

public:
    AircraftManager(ConfigurationWebServer& config, OpenSkyAuthTokenHandler& auth, HttpRequestManager& httpManager, LGFX& tftGfx)
        : configServer(config), authHandler(auth), http(httpManager), tft(tftGfx)
    {
    }
    ~AircraftManager() = default;

    void Initialise();
    void Update();
    void Draw(LGFX_Sprite& backbuffer);
};
