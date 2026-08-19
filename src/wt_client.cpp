// wt_client_new.cpp — polling implementation for localhost:8111 (WinHTTP).

#include "wt_client.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb/stb_image.h>

using json = nlohmann::json;
using clock_ = std::chrono::steady_clock;

static double nowSec()
{
    return std::chrono::duration<double>(clock_::now().time_since_epoch()).count();
}

// ----------------------------------------------------------------------------
// HTTP: a GET against the game's local server. Returns the body or nullopt.
// ----------------------------------------------------------------------------
static std::optional<std::string> httpGet(HINTERNET hConnect, const std::wstring& path)
{
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hReq) return std::nullopt;

    std::optional<std::string> out;
    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq, nullptr))
    {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            std::string body;
            DWORD avail = 0;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(hReq, &avail)) break;
                if (avail == 0) break;
                size_t off = body.size();
                body.resize(off + avail);
                DWORD read = 0;
                if (!WinHttpReadData(hReq, body.data() + off, avail, &read)) break;
                body.resize(off + read);
            } while (avail > 0);
            out = std::move(body);
        }
    }
    WinHttpCloseHandle(hReq);
    return out;
}

static std::optional<json> httpGetJson(HINTERNET hConnect, const std::wstring& path)
{
    auto body = httpGet(hConnect, path);
    if (!body) return std::nullopt;
    json j = json::parse(*body, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

// ----------------------------------------------------------------------------
// parsing helpers
// ----------------------------------------------------------------------------
static std::optional<double> optd(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return std::nullopt;
    return it->get<double>();
}

static float optf(const json& j, const char* key, float def = 0.f)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<float>() : def;
}

// first numeric value whose key starts with the given prefix
static std::optional<double> optPrefix(const json& j, const char* prefix)
{
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.key().rfind(prefix, 0) == 0 && it->is_number())
            return it->get<double>();
    return std::nullopt;
}

// "#rrggbb" -> 0xAABBGGRR (ImU32/IM_COL32 compatible)
static uint32_t parseColor(const std::string& s)
{
    if (s.size() < 7 || s[0] != '#') return 0xFFFFFFFFu;
    auto hex = [](char c) -> uint32_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return 0;
    };
    uint32_t r = hex(s[1]) * 16 + hex(s[2]);
    uint32_t g = hex(s[3]) * 16 + hex(s[4]);
    uint32_t b = hex(s[5]) * 16 + hex(s[6]);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// heuristic: red channel clearly dominant -> enemy marker
static bool colorIsEnemy(uint32_t abgr)
{
    float r = float(abgr & 0xFF), g = float((abgr >> 8) & 0xFF), b = float((abgr >> 16) & 0xFF);
    return r > 140.f && r > g * 1.5f && r > b * 1.5f;
}

static std::wstring exeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

// ----------------------------------------------------------------------------
// lifecycle & snapshots
// ----------------------------------------------------------------------------
void WTClient::start()
{
    quit_ = false;
    th_ = std::thread(&WTClient::run, this);
}

void WTClient::stop()
{
    quit_ = true;
    if (th_.joinable()) th_.join();
}

Telemetry WTClient::telemetry()               { std::lock_guard<std::mutex> l(mx_); return tele_; }
std::vector<MapObj> WTClient::mapObjects()    { std::lock_guard<std::mutex> l(mx_); return objs_; }
std::optional<MapObj> WTClient::player()      { std::lock_guard<std::mutex> l(mx_); return player_; }
std::deque<DmgMsg> WTClient::damageLog()      { std::lock_guard<std::mutex> l(mx_); return dmg_; }
std::deque<ChatMsg> WTClient::chatLog()       { std::lock_guard<std::mutex> l(mx_); return chat_; }
std::vector<Objective> WTClient::objectives() { std::lock_guard<std::mutex> l(mx_); return obj_; }

std::vector<Ghost> WTClient::ghosts()
{
    std::lock_guard<std::mutex> l(mx_);
    std::vector<Ghost> out;
    double now = nowSec();
    for (const auto& t : tracks_) {
        double age = now - t.lastSeen;
        if (age > 0.7 && age <= 45.0)
            out.push_back({t.x, t.y, t.color, t.isEnemy, float(age)});
    }
    return out;
}

std::vector<PlayerScore> WTClient::scoreboard()
{
    std::lock_guard<std::mutex> l(mx_);
    std::vector<PlayerScore> v;
    v.reserve(scores_.size());
    for (const auto& kv : scores_) v.push_back(kv.second);
    return v;
}

void WTClient::clearScores()
{
    std::lock_guard<std::mutex> l(mx_);
    scores_.clear();
}

bool WTClient::takeMapImage(std::vector<uint8_t>& rgba, int& w, int& h)
{
    std::lock_guard<std::mutex> l(mx_);
    if (!mapDirty_) return false;
    rgba = std::move(mapRGBA_);
    w = mapW_; h = mapH_;
    mapRGBA_.clear();
    mapDirty_ = false;
    return true;
}

// ----------------------------------------------------------------------------
// scoreboard: derived from the event feed. Players appear as "Name (Vehicle)";
// AI units have no parentheses and are ignored.
// ----------------------------------------------------------------------------
void WTClient::parseScore(const std::string& msg)
{
    auto trim = [](std::string s) {
        while (!s.empty() && s.back() == ' ')  s.pop_back();
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        return s;
    };
    auto playerName = [&](const std::string& part) -> std::optional<std::string> {
        size_t p = part.find(" (");
        if (p == std::string::npos) return std::nullopt;  // AI unit
        std::string n = trim(part.substr(0, p));
        if (n.empty()) return std::nullopt;
        return n;
    };

    static const char* killVerbs[] = { " destroyed ", " shot down " };
    for (const char* v : killVerbs) {
        size_t p = msg.find(v);
        if (p != std::string::npos) {
            auto a = playerName(msg.substr(0, p));
            auto b = playerName(msg.substr(p + strlen(v)));
            if (a) { auto& e = scores_[*a]; e.name = *a; e.kills++; }
            if (b) { auto& e = scores_[*b]; e.name = *b; e.deaths++; }
            return;
        }
    }

    size_t p = msg.find(" has disconnected from the game");
    if (p != std::string::npos) {
        std::string n = trim(msg.substr(0, p));
        if (!n.empty()) { auto& e = scores_[n]; e.name = n; e.left = true; }
        return;
    }

    p = msg.find(" has crashed");
    if (p != std::string::npos) {
        if (auto a = playerName(msg.substr(0, p))) {
            auto& e = scores_[*a]; e.name = *a; e.deaths++;
        }
    }
}

// Appends the current board to session_stats.csv next to the exe.
void WTClient::dumpScoresCsv()
{
    if (scores_.empty()) return;
    std::wstring path = exeDir() + L"session_stats.csv";
    bool fresh = !std::ifstream(path).good();
    std::ofstream f(path, std::ios::app);
    if (!f) return;
    if (fresh) f << "datetime,match,name,kills,deaths,left\n";

    SYSTEMTIME st; GetLocalTime(&st);
    char ts[32];
    snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

    for (const auto& kv : scores_) {
        const auto& p = kv.second;
        std::string name = p.name;
        for (char& c : name) if (c == ',') c = ';';   // keep the CSV sane
        f << ts << ',' << mapGen_ << ',' << name << ',' << p.kills << ','
          << p.deaths << ',' << (p.left ? 1 : 0) << '\n';
    }
}

// ----------------------------------------------------------------------------
// polling thread
// ----------------------------------------------------------------------------
void WTClient::run()
{
    HINTERNET hSession = WinHttpOpen(L"WTSecondScreen/2.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return;
    WinHttpSetTimeouts(hSession, 400, 400, 400, 800);

    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", 8111, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return; }

    auto past = clock_::now() - std::chrono::seconds(10);
    auto tFast = past, tMap = past, tInfo = past, tDmg = past, tChat = past, tObj = past;

    while (!quit_) {
        auto now = clock_::now();
        auto due = [&](clock_::time_point& t, int ms) {
            if (now - t >= std::chrono::milliseconds(ms)) { t = now; return true; }
            return false;
        };

        // -------- /state + /indicators (200 ms) --------
        if (due(tFast, 200)) {
            auto st = httpGetJson(hConnect, L"/state");
            auto in = httpGetJson(hConnect, L"/indicators");

            Telemetry t;
            bool httpOk = st.has_value() || in.has_value();
            bool anyValid = false;

            if (in && in->is_object() && in->value("valid", false)) {
                anyValid = true;
                t.valid   = true;
                t.vehicle = in->value("type", std::string{});
                t.army    = in->value("army", std::string{});
                t.speedInd = optd(*in, "speed");
                t.compass  = optd(*in, "compass");
            }
            if (st && st->is_object() && st->value("valid", false)) {
                anyValid = true;
                t.valid = true;
                t.ias      = optd(*st, "IAS, km/h");
                t.tas      = optd(*st, "TAS, km/h");
                t.alt      = optd(*st, "H, m");
                t.vy       = optd(*st, "Vy, m/s");
                t.fuel     = optd(*st, "Mfuel, kg");
                t.fuel0    = optd(*st, "Mfuel0, kg");
                t.throttle = optd(*st, "throttle 1, %");
                t.ny       = optd(*st, "Ny");
                t.oilTemp  = optPrefix(*st, "oil temp");
                t.waterTemp = optPrefix(*st, "water temp");
                if (!t.waterTemp) t.waterTemp = optPrefix(*st, "head temp");
            }

            ConnState newConn = !httpOk ? ConnState::Offline
                              : anyValid ? ConnState::InMatch : ConnState::Menu;
            if (newConn == ConnState::Offline && conn_ != ConnState::Offline) {
                // game closed: its feed buffers (and their ids) are gone
                std::lock_guard<std::mutex> l(mx_);
                lastDmg_ = lastEvt_ = lastChat_ = 0;
            }
            conn_ = newConn;

            std::lock_guard<std::mutex> l(mx_);
            tele_ = std::move(t);
        }

        // -------- /map_obj.json (300 ms) --------
        if (due(tMap, 300)) {
            std::vector<MapObj> objs;
            std::optional<MapObj> pl;
            auto mo = httpGetJson(hConnect, L"/map_obj.json");
            if (mo && mo->is_array()) {
                for (const auto& o : *mo) {
                    if (!o.is_object()) continue;
                    MapObj m;
                    m.color = parseColor(o.value("color", std::string{}));
                    m.isEnemy = colorIsEnemy(m.color);
                    m.blink = o.value("blink", 0) != 0;
                    std::string type = o.value("type", std::string{});
                    std::string icon = o.value("icon", std::string{});
                    m.isPlayer = (icon == "Player");

                    if (type == "airfield" && o.contains("sx") && o.contains("ex")) {
                        m.kind = MapObj::Airfield;
                        m.sx = optf(o, "sx"); m.sy = optf(o, "sy");
                        m.ex = optf(o, "ex"); m.ey = optf(o, "ey");
                    } else if (o.contains("x")) {
                        m.x = optf(o, "x"); m.y = optf(o, "y");
                        float r = optf(o, "r", -1.f);
                        if (r > 0.f) {
                            m.kind = MapObj::Zone; m.r = r;
                        } else if (type.rfind("respawn", 0) == 0) {
                            m.kind = MapObj::Respawn;
                        } else if (o.contains("dx")) {
                            m.kind = MapObj::Unit;
                            m.dx = optf(o, "dx"); m.dy = optf(o, "dy");
                        } else {
                            m.kind = MapObj::Point;
                        }
                    } else {
                        continue;
                    }
                    if (m.isPlayer) pl = m;
                    objs.push_back(m);
                }
            }

            std::lock_guard<std::mutex> l(mx_);

            // ---- last-known-position tracking ("ghosts") ----
            double tnow = nowSec();
            for (const auto& m : objs) {
                if (m.kind != MapObj::Unit || m.isPlayer) continue;
                Track* best = nullptr;
                float bestD = 0.03f;   // matching radius in normalized coords
                for (auto& tr : tracks_) {
                    if (tr.color != m.color) continue;
                    float d = std::hypot(tr.x - m.x, tr.y - m.y);
                    if (d < bestD) { bestD = d; best = &tr; }
                }
                if (best) {
                    best->x = m.x; best->y = m.y; best->lastSeen = tnow;
                } else {
                    tracks_.push_back({m.x, m.y, m.color, m.isEnemy, tnow});
                }
            }
            tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                [&](const Track& tr) { return tnow - tr.lastSeen > 45.0; }),
                tracks_.end());

            objs_ = std::move(objs);
            player_ = pl;
        }

        // -------- /map_info.json + /map.img (2 s) --------
        if (due(tInfo, 2000)) {
            auto mi = httpGetJson(hConnect, L"/map_info.json");
            if (mi && mi->is_object() && mi->value("valid", false)) {
                // real map size in meters
                auto mn = mi->find("map_min");
                auto mx = mi->find("map_max");
                if (mn != mi->end() && mx != mi->end() &&
                    mn->is_array() && mx->is_array() &&
                    mn->size() >= 2 && mx->size() >= 2) {
                    mapWm_ = float((*mx)[0].get<double>() - (*mn)[0].get<double>());
                    mapHm_ = float((*mx)[1].get<double>() - (*mn)[1].get<double>());
                }

                int gen = mi->value("map_generation", 0);
                if (gen != mapGen_) {
                    // New match: archive and reset session data ONCE per generation.
                    // Feed cursors are NOT reset — stale messages from the previous
                    // match keep their old ids and are filtered out (see hudmsg poll).
                    {
                        std::lock_guard<std::mutex> l(mx_);
                        if (scoreGenDumped_ != gen) {
                            dumpScoresCsv();
                            scores_.clear();
                            dmg_.clear();
                            chat_.clear();
                            tracks_.clear();
                            scoreGenDumped_ = gen;
                        }
                    }
                    auto img = httpGet(hConnect, L"/map.img?gen=" + std::to_wstring(gen));
                    if (img && !img->empty()) {
                        int w = 0, h = 0, comp = 0;
                        unsigned char* px = stbi_load_from_memory(
                            reinterpret_cast<const unsigned char*>(img->data()),
                            static_cast<int>(img->size()), &w, &h, &comp, 4);
                        if (px) {
                            std::lock_guard<std::mutex> l(mx_);
                            mapRGBA_.assign(px, px + size_t(w) * h * 4);
                            mapW_ = w; mapH_ = h;
                            mapDirty_ = true;
                            mapGen_ = gen;
                            stbi_image_free(px);
                        }
                    }
                }
            } else if (mapGen_ != -1) {
                // match over: archive and flag "no map"
                std::lock_guard<std::mutex> l(mx_);
                dumpScoresCsv();
                mapGen_ = -1;
                mapW_ = mapH_ = 0;
                mapRGBA_.clear();
                mapDirty_ = true;
                mapWm_ = mapHm_ = 0.f;
            }
        }

        // -------- /hudmsg (1 s) --------
        // Always fetch the FULL buffer (lastDmg=0) and filter by our own cursor.
        // If the max id in the buffer drops below our cursor, the game restarted
        // its log (new mission/session): resync from zero. This guarantees every
        // kill is counted exactly once, in every reset scenario.
        if (due(tDmg, 1000)) {
            auto d = httpGetJson(hConnect, L"/hudmsg?lastEvt=0&lastDmg=0");
            if (d && d->is_object()) {
                std::lock_guard<std::mutex> l(mx_);
                if (auto it = d->find("damage"); it != d->end() && it->is_array()) {
                    int maxId = 0;
                    for (const auto& m : *it)
                        maxId = std::max(maxId, m.value("id", 0));
                    int cursor = lastDmg_;
                    if (maxId > 0 && maxId < cursor) cursor = 0;   // log restarted

                    for (const auto& m : *it) {
                        int id = m.value("id", 0);
                        if (id <= cursor) continue;               // already ingested
                        DmgMsg dm;
                        dm.id    = id;
                        dm.time  = m.value("time", 0);
                        dm.enemy = m.value("enemy", false);
                        dm.msg   = m.value("msg", std::string{});
                        parseScore(dm.msg);
                        dmg_.push_back(std::move(dm));
                    }
                    lastDmg_ = std::max(cursor, maxId);
                    while (dmg_.size() > 200) dmg_.pop_front();
                }
                if (auto it = d->find("events"); it != d->end() && it->is_array()) {
                    for (const auto& m : *it) {
                        int id = m.value("id", 0);
                        if (id > lastEvt_) lastEvt_ = id;
                    }
                }
            }
        }

        // -------- /gamechat (1 s) --------
        // Same full-buffer + cursor strategy as hudmsg.
        if (due(tChat, 1000)) {
            auto c = httpGetJson(hConnect, L"/gamechat?lastId=0");
            if (c && c->is_array()) {
                std::lock_guard<std::mutex> l(mx_);
                int maxId = 0;
                for (const auto& m : *c)
                    if (m.is_object()) maxId = std::max(maxId, m.value("id", 0));
                int cursor = lastChat_;
                if (maxId > 0 && maxId < cursor) cursor = 0;       // log restarted

                for (const auto& m : *c) {
                    if (!m.is_object()) continue;
                    int id = m.value("id", 0);
                    if (id <= cursor) continue;
                    ChatMsg cm;
                    cm.id     = id;
                    cm.enemy  = m.value("enemy", false);
                    cm.sender = m.value("sender", std::string{});
                    cm.msg    = m.value("msg", std::string{});
                    chat_.push_back(std::move(cm));
                }
                lastChat_ = std::max(cursor, maxId);
                while (chat_.size() > 200) chat_.pop_front();
            }
        }

        // -------- /mission.json (3 s) --------
        if (due(tObj, 3000)) {
            auto m = httpGetJson(hConnect, L"/mission.json");
            std::vector<Objective> list;
            if (m && m->is_object()) {
                if (auto it = m->find("objectives"); it != m->end() && it->is_array()) {
                    for (const auto& o : *it) {
                        if (!o.is_object()) continue;
                        Objective ob;
                        ob.primary = o.value("primary", false);
                        ob.status  = o.value("status", std::string{});
                        ob.text    = o.value("text", std::string{});
                        if (!ob.text.empty()) list.push_back(std::move(ob));
                    }
                }
            }
            std::lock_guard<std::mutex> l(mx_);
            obj_ = std::move(list);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
}
