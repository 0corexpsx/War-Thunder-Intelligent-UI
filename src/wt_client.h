// wt_client.h — client for War Thunder's local server (localhost:8111).
// A background thread polls the game's endpoints and publishes thread-safe
// snapshots for the render thread.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class ConnState {
    Offline,   // game closed: localhost:8111 not responding
    Menu,      // game running but out of match (hangar/menu)
    InMatch,   // in match: valid data
};

// Tactical map object (from /map_obj.json), pre-digested.
struct MapObj {
    enum Kind { Point, Unit, Airfield, Zone, Respawn };
    Kind     kind     = Point;
    uint32_t color    = 0xFFFFFFFF;  // IM_COL32 format (ABGR)
    bool     blink    = false;
    bool     isPlayer = false;
    bool     isEnemy  = false;       // heuristic: red-ish marker color
    // normalized 0..1 coordinates on the map image
    float x = 0, y = 0;
    float dx = 0, dy = 0;
    float sx = 0, sy = 0, ex = 0, ey = 0;
    float r = 0;
};

// Last-known-position marker for a unit that vanished from the map.
struct Ghost {
    float    x = 0, y = 0;
    uint32_t color = 0xFFFFFFFF;
    bool     isEnemy = false;
    float    ageSec = 0;   // seconds since last seen
};

struct DmgMsg {
    int         id    = 0;
    int         time  = 0;    // mission seconds
    bool        enemy = false;
    std::string msg;
};

struct ChatMsg {
    int         id    = 0;
    bool        enemy = false;
    std::string sender, msg;
};

struct Objective {
    bool        primary = false;
    std::string status;   // in_progress / completed / failed
    std::string text;
};

// Scoreboard entry derived from the event feed.
// NOTE: the game API does not expose the real scoreboard; kills/deaths/leavers
// are counted from feed messages ("destroyed", "shot down",
// "has disconnected from the game", "has crashed") — English client.
struct PlayerScore {
    std::string name;
    int  kills  = 0;
    int  deaths = 0;
    bool left   = false;
};

struct Telemetry {
    bool valid = false;
    std::optional<double> ias, tas;      // km/h (aircraft)
    std::optional<double> alt;           // m
    std::optional<double> vy;            // m/s
    std::optional<double> fuel, fuel0;   // kg
    std::optional<double> throttle;      // %
    std::optional<double> compass;       // deg
    std::optional<double> speedInd;      // km/h from /indicators (tanks/fallback)
    std::optional<double> ny;            // current G load
    std::optional<double> oilTemp;       // °C
    std::optional<double> waterTemp;     // °C (or head temp on radials)
    std::string vehicle;
    std::string army;                    // "air" / "tank"
};

class WTClient {
public:
    ~WTClient() { stop(); }

    void start();
    void stop();

    ConnState                conn() const { return conn_.load(); }
    Telemetry                telemetry();
    std::vector<MapObj>      mapObjects();
    std::optional<MapObj>    player();
    std::vector<Ghost>       ghosts();
    std::deque<DmgMsg>       damageLog();
    std::deque<ChatMsg>      chatLog();
    std::vector<Objective>   objectives();
    std::vector<PlayerScore> scoreboard();
    void                     clearScores();

    // Real map size in meters (from /map_info.json). 0 if unknown.
    float mapWidthMeters()  const { return mapWm_.load(); }
    float mapHeightMeters() const { return mapHm_.load(); }

    // Moves a freshly decoded map image (RGBA8) into `rgba` and returns true.
    // w==0 means "map no longer available" (match ended).
    bool takeMapImage(std::vector<uint8_t>& rgba, int& w, int& h);

private:
    struct Track {
        float x = 0, y = 0;
        uint32_t color = 0;
        bool isEnemy = false;
        double lastSeen = 0;   // seconds, monotonic
    };

    void run();
    void parseScore(const std::string& msg);   // call with mx_ held
    void dumpScoresCsv();                      // call with mx_ held

    std::thread       th_;
    std::atomic<bool> quit_{false};
    std::atomic<ConnState> conn_{ConnState::Offline};
    std::atomic<float> mapWm_{0.f}, mapHm_{0.f};

    std::mutex mx_;
    Telemetry             tele_;
    std::vector<MapObj>   objs_;
    std::optional<MapObj> player_;
    std::vector<Track>    tracks_;
    std::deque<DmgMsg>    dmg_;
    std::deque<ChatMsg>   chat_;
    std::vector<Objective> obj_;
    std::map<std::string, PlayerScore> scores_;
    int lastEvt_ = 0, lastDmg_ = 0, lastChat_ = 0;
    int scoreGenDumped_ = -2;   // last map generation we archived+reset for

    int  mapGen_   = -1;
    bool mapDirty_ = false;
    int  mapW_ = 0, mapH_ = 0;
    std::vector<uint8_t> mapRGBA_;
};
