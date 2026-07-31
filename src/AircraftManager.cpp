#include "AircraftManager.h"

#include "DrawHelpers.h"

constexpr int SCREEN_SIZE = 240;
constexpr int SCREEN_SIZE_DIV_2 = (SCREEN_SIZE / 2);

constexpr int LOCK_BADGE_Y = 20;
constexpr int LIST_START_Y = 44;
constexpr int LIST_ROW_HEIGHT = 20;
constexpr int LIST_VISIBLE_ROWS = 6;

#include <ArduinoJson.h>
#include <algorithm>

namespace {

    std::vector<double> ParseZoomPresets(const String& csv)
    {
        std::vector<double> presets;
        int start = 0;
        while (start < (int)csv.length()) {
            int comma = csv.indexOf(',', start);
            if (comma == -1) comma = csv.length();

            String token = csv.substring(start, comma);
            token.trim();
            if (!token.isEmpty()) {
                double value = token.toDouble();
                if (value > 0.0) presets.push_back(value);
            }

            start = comma + 1;
        }
        std::sort(presets.begin(), presets.end());
        return presets;
    }

    enum class CategoryGroup { Standard, Heavy, Rotorcraft, Other };

    // OpenSky category field: https://openskynetwork.github.io/opensky-api - state vector index 17
    CategoryGroup ClassifyAircraftCategory(int category)
    {
        switch (category) {
            case 4: case 5: case 6: return CategoryGroup::Heavy;             // large / high vortex large / heavy
            case 8:                 return CategoryGroup::Rotorcraft;
            case 2: case 3: case 7: case 9: case 12: return CategoryGroup::Standard; // light/small/high-perf/glider/ultralight
            default: return CategoryGroup::Other;                            // unknown, UAV, surface vehicle, obstacle, etc.
        }
    }

}

void AircraftManager::Initialise()
{
    // get centre point + radius
    lat = configServer.GetStoredString("latitude").toDouble();
    lon = configServer.GetStoredString("longitude").toDouble();
    rad = configServer.GetStoredString("radius").toDouble();

    // configuration
    const String renderText = configServer.GetStoredString("infotext");
    const String renderTris = configServer.GetStoredString("triangle");
    if (!renderText.isEmpty()) displayInfoText = renderText == "true" ? true : false;
    if (!renderTris.isEmpty()) displayTriangles = renderTris == "true" ? true : false;

    const String zoomTapPref = configServer.GetStoredString("zoomtap");
    const String listSwipePref = configServer.GetStoredString("listswipe");
    const String lockPref = configServer.GetStoredString("lock");
    const String heatmapPref = configServer.GetStoredString("heatmap");
    const String typeIconPref = configServer.GetStoredString("typeicon");
    if (!zoomTapPref.isEmpty())   zoomTapEnabled = zoomTapPref == "true";
    if (!listSwipePref.isEmpty()) listSwipeEnabled = listSwipePref == "true";
    if (!lockPref.isEmpty())      lockEnabled = lockPref == "true";
    if (!heatmapPref.isEmpty())   heatmapEnabled = heatmapPref == "true";
    if (!typeIconPref.isEmpty())  typeIconEnabled = typeIconPref == "true";

    const String unitSystemPref = configServer.GetStoredString("unitsystem");
    if (unitSystemPref == "aviation")     unitSystem = UnitSystem::Aviation;
    else if (unitSystemPref == "none")    unitSystem = UnitSystem::None;
    else                                  unitSystem = UnitSystem::Metric;

    const String zoomPresetsPref = configServer.GetStoredString("zoompresets");
    zoomPresets = ParseZoomPresets(zoomPresetsPref.isEmpty() ? "0.5,1,2,2.49" : zoomPresetsPref);
    if (zoomPresets.empty()) zoomPresets = { 0.5, 1.0, 2.0, 2.49 };

    // calculate how often we can call OpenSky API before being rate limited
    constexpr int MS_PER_DAY = 24 * 60 * 60 * 1000;
    constexpr int ANONYMOUS_TOKENS_PER_DAY = 400;
    constexpr int AUTHED_TOKENS_PER_DAY = 4000;
    constexpr int TOKEN_BUFFER = 3;
    int dailyRequestBudget = ANONYMOUS_TOKENS_PER_DAY - TOKEN_BUFFER; // non-authed tokens minus buffer

    const String token = authHandler.GetValidToken(configServer.GetStoredString("opensky-id"), configServer.GetStoredString("opensky-secret"));
    if (!token.isEmpty())
        dailyRequestBudget = AUTHED_TOKENS_PER_DAY - TOKEN_BUFFER; // authed tokens minus buffer

    fetchInterval = MS_PER_DAY / dailyRequestBudget;
}

void AircraftManager::MergeAircraftStates(const std::vector<Aircraft>& aircraft, unsigned long now)
{
    for (auto& ac : aircraft) {
        auto it = trackedAircraft.find(ac.icao24);
        if (it == trackedAircraft.end())
            trackedAircraft.emplace(ac.icao24, TrackedAircraft{ ac, now });
        else
            it->second.Update(ac, now);
    }
}

void AircraftManager::Update()
{
    unsigned long now = millis();

    // fetch cycle
    if (now - lastFetch >= fetchInterval) {
        lastFetch = now;

        // auth
        const String token = authHandler.GetValidToken(
            configServer.GetStoredString("opensky-id"),
            configServer.GetStoredString("opensky-secret")
        );

        std::vector<std::pair<String, String>> headers = {};
        if (!token.isEmpty()) headers.push_back({ "Authorization", "Bearer " + token });

        // request
        HttpResult result = http.Get(
            "https://opensky-network.org/api/states/all",
            {
              {"lamin", String(lat - rad)},
              {"lamax", String(lat + rad)},
              {"lomin", String(lon - rad)},
              {"lomax", String(lon + rad)}
            },
            headers
        );

        // If request failed, skip this update
        if (!result.success) {
            Serial.print("[WARN] OpenSky API request failed: ");
            Serial.println(result.errorMessage);
            return;
        }

        // track
        JsonDocument doc;
        deserializeJson(doc, result.response);
        auto aircraft = JsonParser::ParseArray<Aircraft>(doc["states"]);
        now = millis(); // override with post-parse timestamp

        MergeAircraftStates(aircraft, now);

        // remove any planes that disappeared from the feed, except one actively locked -
        // that one is tracked separately below regardless of whether it's still in range
        for (auto it = trackedAircraft.begin(); it != trackedAircraft.end(); ) {
            if (it->first == lockedIcao) { ++it; continue; }

            bool aircraftPresent = std::any_of(aircraft.begin(), aircraft.end(), [&](const Aircraft& ac) { return ac.icao24 == it->first; });
            if (!aircraftPresent)
                it = trackedAircraft.erase(it);
            else
                ++it;
        }

        // locked aircraft may have left the configured radius - fetch it directly by icao24,
        // bypassing the bounding box (OpenSky ANDs bbox + icao24, so bbox must be omitted here)
        if (!lockedIcao.isEmpty()) {
            HttpResult lockResult = http.Get(
                "https://opensky-network.org/api/states/all",
                { {"icao24", lockedIcao} },
                headers
            );

            if (lockResult.success) {
                JsonDocument lockDoc;
                deserializeJson(lockDoc, lockResult.response);
                auto lockedAircraft = JsonParser::ParseArray<Aircraft>(lockDoc["states"]);
                MergeAircraftStates(lockedAircraft, millis());
            } else {
                Serial.print("[WARN] Locked aircraft fetch failed: ");
                Serial.println(lockResult.errorMessage);
            }
        }
    }
}

Gesture AircraftManager::PollGesture()
{
    constexpr unsigned long DOUBLE_TAP_MS = 350;
    constexpr unsigned long LONG_PRESS_MS = 600;
    constexpr int MOVE_TOLERANCE_SQ = 15 * 15;
    constexpr int SWIPE_THRESHOLD = 40;

    int32_t x = -1;
    int32_t y = -1;
    const bool touchDown = tft.getTouch(&x, &y);
    const unsigned long now = millis();

    if (touchDown && !touchWasDown) {
        touchStartX = touchLastX = x;
        touchStartY = touchLastY = y;
        touchStartTime = now;
    } else if (touchDown) {
        touchLastX = x;
        touchLastY = y;
    }

    Gesture gesture = Gesture::None;

    if (!touchDown && touchWasDown) {
        const int dx = touchLastX - touchStartX;
        const int dy = touchLastY - touchStartY;
        const unsigned long duration = now - touchStartTime;
        const int distSq = dx * dx + dy * dy;

        if (duration >= LONG_PRESS_MS && distSq <= MOVE_TOLERANCE_SQ) {
            gesture = Gesture::LongPress;
        } else if (abs(dy) >= SWIPE_THRESHOLD && abs(dy) > abs(dx)) {
            gesture = dy < 0 ? Gesture::SwipeUp : Gesture::SwipeDown;
        } else if (distSq <= MOVE_TOLERANCE_SQ) {
            gesture = Gesture::Tap;
        }
    }
    touchWasDown = touchDown;

    // double-tap only matters for zoom on the radar screen - elsewhere, fire taps immediately
    const bool needsDoubleTapDetection = zoomTapEnabled && uiMode == UiMode::Radar;

    if (gesture == Gesture::Tap && needsDoubleTapDetection) {
        const bool followsRecentTap = pendingTap && (now - lastTapTime) <= DOUBLE_TAP_MS;
        lastTapTime = now;
        pendingTap = !followsRecentTap;
        return followsRecentTap ? Gesture::DoubleTap : Gesture::None;
    }

    if (gesture != Gesture::None) {
        pendingTap = false;
        return gesture;
    }

    if (pendingTap && (now - lastTapTime) > DOUBLE_TAP_MS) {
        pendingTap = false;
        return Gesture::Tap;
    }

    return Gesture::None;
}

void AircraftManager::HandleRadarGesture(Gesture gesture)
{
    switch (gesture) {
        case Gesture::Tap:
            if (TryHitLockBadge(touchLastX, touchLastY)) {
                selectedIcao = lockedIcao;
                uiMode = UiMode::Detail;
            } else if (TrySelectAircraftAtTouch(touchLastX, touchLastY)) {
                uiMode = UiMode::Detail;
            }
            return;

        case Gesture::DoubleTap:
            if (zoomTapEnabled) CycleZoom();
            return;

        case Gesture::SwipeUp:
        case Gesture::SwipeDown:
            if (listSwipeEnabled) {
                listScrollIndex = 0;
                uiMode = UiMode::List;
            }
            return;

        case Gesture::LongPress:
            RequestManualRefresh();
            return;

        default:
            return;
    }
}

void AircraftManager::HandleListGesture(Gesture gesture)
{
    const auto icaos = GetSortedVisibleIcaos();

    switch (gesture) {
        case Gesture::SwipeUp:
            listScrollIndex = min(listScrollIndex + 1, max(0, (int)icaos.size() - LIST_VISIBLE_ROWS));
            return;

        case Gesture::SwipeDown:
            listScrollIndex = max(listScrollIndex - 1, 0);
            return;

        case Gesture::Tap: {
            const int row = HitTestListRow(touchLastY, icaos.size());
            const int index = listScrollIndex + row;
            if (row >= 0 && index < (int)icaos.size()) {
                selectedIcao = icaos[index];
                uiMode = UiMode::Detail;
            } else {
                uiMode = UiMode::Radar;
            }
            return;
        }

        default:
            return;
    }
}

void AircraftManager::HandleDetailGesture(Gesture gesture)
{
    switch (gesture) {
        case Gesture::Tap:
            selectedIcao = "";
            uiMode = UiMode::Radar;
            return;

        case Gesture::LongPress:
            if (lockEnabled)
                lockedIcao = (lockedIcao == selectedIcao) ? "" : selectedIcao;
            return;

        default:
            return;
    }
}

void AircraftManager::CycleZoom()
{
    if (zoomPresets.empty()) return;

    constexpr double EPSILON = 0.001;

    double next = zoomPresets.front();
    for (double preset : zoomPresets) {
        if (preset > rad + EPSILON) {
            next = preset;
            break;
        }
    }

    rad = next;
    configServer.SetStoredString("radius", String(rad, 6));
    zoomLabelUntil = millis() + 1000;
}

void AircraftManager::RequestManualRefresh()
{
    constexpr unsigned long REFRESH_COOLDOWN_MS = 10000;
    constexpr unsigned long FLASH_DURATION_MS = 300;

    const unsigned long now = millis();
    if (now - lastManualRefresh < REFRESH_COOLDOWN_MS) return;

    lastManualRefresh = now;
    lastFetch = 0; // forces Update() to fetch immediately, bypassing the rate-limit interval
    refreshFlashUntil = now + FLASH_DURATION_MS;
}

std::vector<String> AircraftManager::GetSortedVisibleIcaos() const
{
    std::vector<String> result;
    for (auto& [icao, tracked] : trackedAircraft) {
        if (!tracked.state.onGround)
            result.push_back(icao);
    }
    return result;
}

int AircraftManager::HitTestListRow(int touchY, int rowCount) const
{
    constexpr int HALF_ROW = LIST_ROW_HEIGHT / 2;
    if (touchY < LIST_START_Y - HALF_ROW) return -1;

    const int row = (touchY - (LIST_START_Y - HALF_ROW)) / LIST_ROW_HEIGHT;
    if (row < 0 || row >= LIST_VISIBLE_ROWS) return -1;
    return row;
}

bool AircraftManager::TryHitLockBadge(int touchX, int touchY) const
{
    if (lockedIcao.isEmpty()) return false;
    return touchY >= (LOCK_BADGE_Y - 10) && touchY <= (LOCK_BADGE_Y + 14);
}

bool AircraftManager::TrySelectAircraftAtTouch(int touchX, int touchY)
{
    constexpr int TOUCH_RADIUS = 14;
    constexpr int TOUCH_RADIUS_SQUARED = TOUCH_RADIUS * TOUCH_RADIUS;

    for (auto& [icao, tracked] : trackedAircraft) {
        if (tracked.state.onGround) continue;

        tracked.Tick();
        auto [predLat, predLon] = tracked.GetDisplayPosition();
        auto [x, y] = ProjectCoordinateToScreen(predLat, predLon);

        const int dx = touchX - x;
        const int dy = touchY - y;
        if ((dx * dx + dy * dy) <= TOUCH_RADIUS_SQUARED) {
            selectedIcao = icao;
            return true;
        }
    }

    return false;
}

void AircraftManager::Draw(LGFX_Sprite& backbuffer)
{
    if (uiMode == UiMode::Detail && trackedAircraft.find(selectedIcao) == trackedAircraft.end()) {
        selectedIcao = "";
        uiMode = UiMode::Radar;
    }

    const Gesture gesture = PollGesture();
    switch (uiMode) {
        case UiMode::Radar:  HandleRadarGesture(gesture); break;
        case UiMode::List:   HandleListGesture(gesture); break;
        case UiMode::Detail: HandleDetailGesture(gesture); break;
    }

    switch (uiMode) {
        case UiMode::Detail: {
            auto it = trackedAircraft.find(selectedIcao);
            it->second.Tick();
            DrawSelectedAircraftDetails(backbuffer, it->second, selectedIcao);
            break;
        }

        case UiMode::List:
            DrawAircraftList(backbuffer);
            break;

        case UiMode::Radar:
        default:
            DrawRadarScreen(backbuffer);
            break;
    }
}

void AircraftManager::DrawRadarScreen(LGFX_Sprite& backbuffer)
{
    DrawRadarCircles(backbuffer);

    for (auto& [icao, tracked] : trackedAircraft) {
        if (tracked.state.onGround) continue;

        tracked.Tick();
        auto [predLat, predLon] = tracked.GetDisplayPosition();
        auto [x, y] = ProjectCoordinateToScreen(predLat, predLon);

        if (displayInfoText)
            DrawAircraftInfo(backbuffer, x, y, tracked);

        DrawAircraftMarker(backbuffer, x, y, tracked);
    }

    if (!lockedIcao.isEmpty())
        DrawLockBadge(backbuffer);

    if (millis() < zoomLabelUntil) {
        backbuffer.setTextSize(2);
        backbuffer.setTextColor(lgfx::color888(0, 255, 0));
        backbuffer.drawCentreString(FormatZoomLabel(), SCREEN_SIZE_DIV_2, SCREEN_SIZE_DIV_2);
    }
}

void AircraftManager::DrawRadarCircles(LGFX_Sprite& backbuffer) const
{
    constexpr int CENTRE = SCREEN_SIZE_DIV_2 - 1;
    constexpr int OUTER = SCREEN_SIZE_DIV_2 - 1;
    constexpr int MID = (OUTER / 3) * 2;
    constexpr int INNER = OUTER / 3;
    constexpr int LABEL_INSET = 10;

    const bool flashing = millis() < refreshFlashUntil;
    const uint32_t ringColor = flashing ? lgfx::color888(255, 255, 255) : lgfx::color888(0, 200, 0);

    backbuffer.drawCircle(CENTRE, CENTRE, OUTER, ringColor);
    backbuffer.drawCircle(CENTRE, CENTRE, MID, flashing ? ringColor : lgfx::color888(0, 64, 0));
    backbuffer.drawCircle(CENTRE, CENTRE, INNER, flashing ? ringColor : lgfx::color888(0, 32, 0));

    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(0, 180, 0));
    backbuffer.drawCentreString(FormatRingDistance(rad), CENTRE, CENTRE - OUTER + LABEL_INSET);
    backbuffer.drawCentreString(FormatRingDistance(rad * MID / OUTER), CENTRE, CENTRE - MID + LABEL_INSET);
    backbuffer.drawCentreString(FormatRingDistance(rad * INNER / OUTER), CENTRE, CENTRE - INNER + LABEL_INSET);
}

std::pair<int, int> AircraftManager::ProjectCoordinateToScreen(float predLat, float predLon) const
{
    const float dLon = predLon - lon;
    const float dLat = predLat - lat;

    const float normLon = (dLon + rad) / (2.0f * rad);
    const float normLat = (dLat + rad) / (2.0f * rad);

    const int x = static_cast<int>(normLon * SCREEN_SIZE);
    const int y = static_cast<int>(SCREEN_SIZE - (normLat * SCREEN_SIZE));

    return { x, y };
}

String AircraftManager::FormatAltitude(float metres) const
{
    if (unitSystem == UnitSystem::Aviation) return String((int)(metres * 3.28084f)) + " ft";
    return String(metres) + " m";
}

String AircraftManager::FormatSpeed(float metersPerSecond) const
{
    if (unitSystem == UnitSystem::Aviation) return String((int)(metersPerSecond * 1.94384f)) + " kt";
    return String(metersPerSecond) + " m/s";
}

String AircraftManager::FormatVerticalRate(float metersPerSecond) const
{
    if (unitSystem == UnitSystem::Aviation) return String((int)(metersPerSecond * 196.850394f)) + " ft/min";
    return String(metersPerSecond) + " m/s";
}

String AircraftManager::FormatRingDistance(double radiusDegrees) const
{
    if (unitSystem == UnitSystem::Aviation) return String((int)(radiusDegrees * 60.0)) + "nm";
    return String((int)(radiusDegrees * 111.32)) + "km";
}

String AircraftManager::FormatZoomLabel() const
{
    String value = String(rad, 2);
    while (value.endsWith("0")) value.remove(value.length() - 1);
    if (value.endsWith(".")) value.remove(value.length() - 1);
    return value + " deg";
}

void AircraftManager::DrawAircraftInfo(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const
{
    const int lineHeight = tft.fontHeight() + 1;

    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(0, 128, 0));
    backbuffer.drawString(tracked.state.callsign, x + 5, y + 5);

    if (unitSystem == UnitSystem::None) return;

    backbuffer.drawString(FormatSpeed(tracked.state.velocity), x + 5, y + 5 + lineHeight);
    backbuffer.drawString(FormatAltitude(tracked.state.baroAltitude), x + 5, y + 5 + lineHeight * 2);
}

void AircraftManager::DrawAircraftMarker(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const
{
    const uint32_t color = heatmapEnabled ? AltitudeToColor(tracked.state.baroAltitude) : lgfx::color888(0, 255, 0);

    if (typeIconEnabled) {
        switch (ClassifyAircraftCategory(tracked.state.category)) {
            case CategoryGroup::Rotorcraft:
                backbuffer.fillCircle(x, y, 4, color);
                return;
            case CategoryGroup::Heavy:
                DrawAircraftTriangle(backbuffer, x, y, tracked, color, 1.6f);
                return;
            case CategoryGroup::Other:
                backbuffer.fillRect(x - 3, y - 3, 6, 6, color);
                return;
            case CategoryGroup::Standard:
                break; // falls through to the default marker below
        }
    }

    if (displayTriangles)
        DrawAircraftTriangle(backbuffer, x, y, tracked, color, 1.0f);
    else
        backbuffer.fillCircle(x, y, 3, color);
}

void AircraftManager::DrawAircraftTriangle(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked, uint32_t color, float scale) const
{
    const float dx = std::sin(radians(tracked.state.trueTrack));
    const float dy = -std::cos(radians(tracked.state.trueTrack));
    const float px = -dy;
    const float py = dx;

    const float TRIANGLE_LENGTH = 6.0f * scale;
    const float TRIANGLE_WIDTH = 3.0f * scale;

    const float tipX = x + dx * TRIANGLE_LENGTH;
    const float tipY = y + dy * TRIANGLE_LENGTH;
    const float leftX = x - dx * TRIANGLE_LENGTH * 0.5f + px * TRIANGLE_WIDTH * 0.5f;
    const float leftY = y - dy * TRIANGLE_LENGTH * 0.5f + py * TRIANGLE_WIDTH * 0.5f;
    const float rightX = x - dx * TRIANGLE_LENGTH * 0.5f - px * TRIANGLE_WIDTH * 0.5f;
    const float rightY = y - dy * TRIANGLE_LENGTH * 0.5f - py * TRIANGLE_WIDTH * 0.5f;

    backbuffer.fillTriangle(tipX, tipY, leftX, leftY, rightX, rightY, color);
}

void AircraftManager::DrawLockBadge(LGFX_Sprite& backbuffer) const
{
    auto it = trackedAircraft.find(lockedIcao);
    if (it == trackedAircraft.end()) return;

    const String label = "LOCK " + (it->second.state.callsign.isEmpty() ? lockedIcao : it->second.state.callsign);
    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(0, 255, 255));
    backbuffer.drawCentreString(label, SCREEN_SIZE_DIV_2, LOCK_BADGE_Y);
}

void AircraftManager::DrawSelectedAircraftDetails(LGFX_Sprite& backbuffer, const TrackedAircraft& tracked, const String& icao) const
{
    constexpr int CENTRE_X = SCREEN_SIZE_DIV_2;
    const bool isLocked = icao == lockedIcao;

    backbuffer.fillScreen(lgfx::color888(0, 0, 8));
    backbuffer.setTextSize(2);
    backbuffer.setTextColor(isLocked ? lgfx::color888(0, 255, 255) : lgfx::color888(0, 255, 0));
    backbuffer.drawCentreString("Aircraft", CENTRE_X, 32);

    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(0, 200, 0));

    const int lineHeight = tft.fontHeight() + 4;
    int y = 60;

    auto DrawLine = [&](const String& label, const String& value) {
        backbuffer.drawCentreString(label + value, CENTRE_X, y);
        y += lineHeight;
    };

    DrawLine("ICAO: ", icao);
    DrawLine("Callsign: ", tracked.state.callsign.isEmpty() ? String("N/A") : tracked.state.callsign);
    DrawLine("Country: ", tracked.state.originCountry.isEmpty() ? String("N/A") : tracked.state.originCountry);
    if (unitSystem != UnitSystem::None) {
        DrawLine("Alt: ", FormatAltitude(tracked.state.baroAltitude));
        DrawLine("Speed: ", FormatSpeed(tracked.state.velocity));
    }
    DrawLine("Heading: ", String(tracked.state.trueTrack) + " deg");
    if (unitSystem != UnitSystem::None)
        DrawLine("Vert: ", FormatVerticalRate(tracked.state.verticalRate));
    DrawLine("Ground: ", tracked.state.onGround ? String("yes") : String("no"));

    backbuffer.setTextColor(lgfx::color888(0, 128, 0));
    if (lockEnabled)
        backbuffer.drawCentreString(isLocked ? "Locked - hold to unlock" : "Hold to lock", CENTRE_X, SCREEN_SIZE - 50);
    backbuffer.drawCentreString("Tap to return to radar", CENTRE_X, SCREEN_SIZE - 34);
}

void AircraftManager::DrawAircraftList(LGFX_Sprite& backbuffer) const
{
    constexpr int CENTRE_X = SCREEN_SIZE_DIV_2;

    backbuffer.fillScreen(lgfx::color888(0, 0, 8));
    backbuffer.setTextSize(2);
    backbuffer.setTextColor(lgfx::color888(0, 255, 0));
    backbuffer.drawCentreString("Aircraft", CENTRE_X, 20);

    backbuffer.setTextSize(1);

    const auto icaos = GetSortedVisibleIcaos();
    if (icaos.empty()) {
        backbuffer.setTextColor(lgfx::color888(0, 150, 0));
        backbuffer.drawCentreString("No aircraft in range", CENTRE_X, SCREEN_SIZE_DIV_2);
        return;
    }

    for (int row = 0; row < LIST_VISIBLE_ROWS; ++row) {
        const int index = listScrollIndex + row;
        if (index >= (int)icaos.size()) break;

        const String& icao = icaos[index];
        const TrackedAircraft& tracked = trackedAircraft.at(icao);
        const String name = tracked.state.callsign.isEmpty() ? icao : tracked.state.callsign;
        const String label = unitSystem == UnitSystem::None ? name : name + "  " + FormatAltitude(tracked.state.baroAltitude);

        backbuffer.setTextColor(icao == lockedIcao ? lgfx::color888(0, 255, 255) : lgfx::color888(0, 200, 0));
        backbuffer.drawCentreString(label, CENTRE_X, LIST_START_Y + row * LIST_ROW_HEIGHT);
    }
}
