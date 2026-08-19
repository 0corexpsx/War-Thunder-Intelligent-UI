// main.cpp — WT Second Screen (ImGui + DirectX 11)
// War Thunder tactical map and instruments on a second monitor.

#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <nlohmann/json.hpp>

#include "wt_client.h"

#pragma comment(lib, "d3d11.lib")

// ----------------------------------- palette -----------------------------------
static const ImVec4 COL_INK   = ImVec4(0.851f, 0.867f, 0.800f, 1.f);
static const ImVec4 COL_DIM   = ImVec4(0.510f, 0.545f, 0.455f, 1.f);
static const ImVec4 COL_GREEN = ImVec4(0.561f, 0.702f, 0.353f, 1.f);
static const ImVec4 COL_RED   = ImVec4(0.816f, 0.333f, 0.271f, 1.f);

// ------------------------------------ state ------------------------------------
static ID3D11Device*           g_dev = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain*         g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;

static ID3D11ShaderResourceView* g_mapSRV = nullptr;
static int g_mapW = 0, g_mapH = 0;

static ImFont* g_fontUI  = nullptr;
static ImFont* g_fontBig = nullptr;

struct MonitorRect { RECT rc; };
static std::vector<MonitorRect> g_monitors;

struct AppCfg {
    int  monitor = -1;   // -1 = auto (second monitor if present)
    bool onTop = false;
    // window visibility
    bool showMap = true, showSpeed = true, showAlt = true, showHeading = true,
         showVario = true, showFuel = true, showThrottle = false,
         showVehicle = false, showDamage = true, showScore = false,
         showChat = false, showObj = false, showEngine = false;
    // map
    float markerScale = 1.f;
    bool  follow = true;
    bool  showDist = true;      // distance labels on spotted enemies
    bool  showRings = false;    // range rings around the player
    bool  showGhosts = true;    // last-known-position markers
    // alerts
    bool  alarmOn = false;
    float alarmMeters = 500.f;
    // input
    bool  hotkeys = false;      // global hotkeys (numpad)
    // theme
    float colBg[3]     = { 0.063f, 0.078f, 0.055f };
    float colAccent[3] = { 0.878f, 0.639f, 0.231f };
};
static AppCfg g_cfg;
static bool g_cfgDirty = false;
static bool g_showSettings = false;
static bool g_showWidgets  = false;

// map view
static float g_zoom = 1.f;
static float g_cx = .5f, g_cy = .5f;

// global hotkey requests (set in WndProc, consumed in the frame)
static bool g_hkZoomIn = false, g_hkZoomOut = false, g_hkFollow = false;
enum { HK_ZOOM_IN = 1, HK_ZOOM_OUT = 2, HK_FOLLOW = 3 };

// ------------------------------- config on disk --------------------------------
static std::wstring ConfigPath()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t slash = p.find_last_of(L"\\/");
    return p.substr(0, slash + 1) + L"wt_config.json";
}

static void LoadCfg()
{
    std::ifstream f(ConfigPath());
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    g_cfg.monitor     = j.value("monitor", -1);
    g_cfg.onTop       = j.value("on_top", false);
    g_cfg.showMap     = j.value("map", true);
    g_cfg.showSpeed   = j.value("speed", true);
    g_cfg.showAlt     = j.value("alt", true);
    g_cfg.showHeading = j.value("heading", true);
    g_cfg.showVario   = j.value("vario", true);
    g_cfg.showFuel    = j.value("fuel", true);
    g_cfg.showThrottle= j.value("throttle", false);
    g_cfg.showVehicle = j.value("vehicle", false);
    g_cfg.showDamage  = j.value("damage", true);
    g_cfg.showScore   = j.value("score", false);
    g_cfg.showChat    = j.value("chat", false);
    g_cfg.showObj     = j.value("objectives", false);
    g_cfg.showEngine  = j.value("engine", false);
    g_cfg.markerScale = j.value("marker_scale", 1.f);
    if (g_cfg.markerScale < 0.5f || g_cfg.markerScale > 3.f) g_cfg.markerScale = 1.f;
    g_cfg.follow      = j.value("follow", true);
    g_cfg.showDist    = j.value("distances", true);
    g_cfg.showRings   = j.value("rings", false);
    g_cfg.showGhosts  = j.value("ghosts", true);
    g_cfg.alarmOn     = j.value("alarm", false);
    g_cfg.alarmMeters = j.value("alarm_m", 500.f);
    g_cfg.alarmMeters = std::clamp(g_cfg.alarmMeters, 100.f, 2000.f);
    g_cfg.hotkeys     = j.value("hotkeys", false);
    if (auto it = j.find("col_bg"); it != j.end() && it->is_array() && it->size() == 3)
        for (int i = 0; i < 3; ++i) g_cfg.colBg[i] = (*it)[i].get<float>();
    if (auto it = j.find("col_accent"); it != j.end() && it->is_array() && it->size() == 3)
        for (int i = 0; i < 3; ++i) g_cfg.colAccent[i] = (*it)[i].get<float>();
}

static void SaveCfg()
{
    nlohmann::json j = {
        {"monitor", g_cfg.monitor}, {"on_top", g_cfg.onTop},
        {"map", g_cfg.showMap}, {"speed", g_cfg.showSpeed}, {"alt", g_cfg.showAlt},
        {"heading", g_cfg.showHeading}, {"vario", g_cfg.showVario},
        {"fuel", g_cfg.showFuel}, {"throttle", g_cfg.showThrottle},
        {"vehicle", g_cfg.showVehicle}, {"damage", g_cfg.showDamage},
        {"score", g_cfg.showScore}, {"chat", g_cfg.showChat},
        {"objectives", g_cfg.showObj}, {"engine", g_cfg.showEngine},
        {"marker_scale", g_cfg.markerScale}, {"follow", g_cfg.follow},
        {"distances", g_cfg.showDist}, {"rings", g_cfg.showRings},
        {"ghosts", g_cfg.showGhosts},
        {"alarm", g_cfg.alarmOn}, {"alarm_m", g_cfg.alarmMeters},
        {"hotkeys", g_cfg.hotkeys},
        {"col_bg", {g_cfg.colBg[0], g_cfg.colBg[1], g_cfg.colBg[2]}},
        {"col_accent", {g_cfg.colAccent[0], g_cfg.colAccent[1], g_cfg.colAccent[2]}},
    };
    std::ofstream f(ConfigPath());
    if (f) f << j.dump(2);
    g_cfgDirty = false;
}

// ---------------------------------- monitors ------------------------------------
static BOOL CALLBACK MonEnumProc(HMONITOR, HDC, LPRECT rc, LPARAM)
{
    g_monitors.push_back({*rc});
    return TRUE;
}

static void EnumMonitors()
{
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonEnumProc, 0);
    std::sort(g_monitors.begin(), g_monitors.end(),
              [](const MonitorRect& a, const MonitorRect& b) {
                  return a.rc.left != b.rc.left ? a.rc.left < b.rc.left
                                                : a.rc.top < b.rc.top;
              });
}

static void ApplyMonitor(HWND hwnd)
{
    if (g_monitors.empty()) return;
    int idx = g_cfg.monitor;
    if (idx < 0) idx = g_monitors.size() > 1 ? 1 : 0;
    idx = std::clamp(idx, 0, int(g_monitors.size()) - 1);
    const RECT& r = g_monitors[idx].rc;
    SetWindowPos(hwnd, g_cfg.onTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static void ApplyHotkeys(HWND hwnd)
{
    UnregisterHotKey(hwnd, HK_ZOOM_IN);
    UnregisterHotKey(hwnd, HK_ZOOM_OUT);
    UnregisterHotKey(hwnd, HK_FOLLOW);
    if (g_cfg.hotkeys) {
        RegisterHotKey(hwnd, HK_ZOOM_IN,  0, VK_ADD);
        RegisterHotKey(hwnd, HK_ZOOM_OUT, 0, VK_SUBTRACT);
        RegisterHotKey(hwnd, HK_FOLLOW,   0, VK_MULTIPLY);
    }
}

// ------------------------------------ D3D11 -------------------------------------
static void CreateRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool CreateDeviceD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL lvl;
    const D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, lvls, 2,
        D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &lvl, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, lvls, 2,
            D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &lvl, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_mapSRV) { g_mapSRV->Release(); g_mapSRV = nullptr; }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx)  { g_ctx->Release();  g_ctx = nullptr; }
    if (g_dev)  { g_dev->Release();  g_dev = nullptr; }
}

static void UploadMapTexture(const std::vector<uint8_t>& rgba, int w, int h)
{
    if (g_mapSRV) { g_mapSRV->Release(); g_mapSRV = nullptr; }
    g_mapW = w; g_mapH = h;
    if (w <= 0 || h <= 0) return;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd{ rgba.data(), UINT(w * 4), 0 };
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &tex)) && tex) {
        g_dev->CreateShaderResourceView(tex, nullptr, &g_mapSRV);
        tex->Release();
    }
}

// ------------------------------------ style -------------------------------------
static ImVec4 Accent(float a = 1.f)
{
    return ImVec4(g_cfg.colAccent[0], g_cfg.colAccent[1], g_cfg.colAccent[2], a);
}
static ImU32 AccentU32(float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(Accent(a)); }

static ImVec4 BgShade(float k, float a = 1.f)
{
    auto c = [&](float v) { return std::clamp(v * k, 0.f, 1.f); };
    return ImVec4(c(g_cfg.colBg[0]), c(g_cfg.colBg[1]), c(g_cfg.colBg[2]), a);
}

static void ApplyStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = s.FrameRounding = s.PopupRounding = s.GrabRounding = 0.f;
    s.WindowBorderSize = 1.f;
    s.WindowTitleAlign = ImVec2(0.02f, 0.5f);

    ImVec4 accent = Accent();
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]           = COL_INK;
    c[ImGuiCol_TextDisabled]   = COL_DIM;
    c[ImGuiCol_WindowBg]       = BgShade(1.f);
    c[ImGuiCol_PopupBg]        = BgShade(1.f, 0.98f);
    c[ImGuiCol_Border]         = BgShade(2.6f);
    c[ImGuiCol_FrameBg]        = BgShade(0.65f);
    c[ImGuiCol_FrameBgHovered] = BgShade(1.7f);
    c[ImGuiCol_TitleBg]        = BgShade(1.3f);
    c[ImGuiCol_TitleBgActive]  = BgShade(1.75f);
    c[ImGuiCol_MenuBarBg]      = BgShade(0.85f);
    c[ImGuiCol_Button]         = BgShade(1.3f);
    c[ImGuiCol_ButtonHovered]  = BgShade(2.1f);
    c[ImGuiCol_ButtonActive]   = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_Header]         = BgShade(2.1f);
    c[ImGuiCol_HeaderHovered]  = ImVec4(accent.x, accent.y, accent.z, 0.28f);
    c[ImGuiCol_HeaderActive]   = ImVec4(accent.x, accent.y, accent.z, 0.40f);
    c[ImGuiCol_CheckMark]      = accent;
    c[ImGuiCol_SliderGrab]     = accent;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_PlotHistogram]  = COL_GREEN;
    c[ImGuiCol_ResizeGrip]     = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_ScrollbarBg]    = BgShade(0.65f);
    c[ImGuiCol_ScrollbarGrab]  = BgShade(2.6f);
    c[ImGuiCol_TableRowBgAlt]  = BgShade(1.35f, 0.55f);
    c[ImGuiCol_TableHeaderBg]  = BgShade(1.5f);
    c[ImGuiCol_TableBorderLight]  = BgShade(2.2f);
    c[ImGuiCol_TableBorderStrong] = BgShade(2.6f);
}

// -------------------------------- widget helper ---------------------------------
static void BigValueWindow(const char* title, bool* open, const char* value,
                           const char* unit, bool warn,
                           ImVec2 defPos, ImVec2 defSize)
{
    if (!*open) return;
    ImGui::SetNextWindowPos(defPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(defSize, ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title, open)) {
        ImVec2 win = ImGui::GetWindowSize();
        ImGui::PushFont(g_fontBig);
        ImVec2 ts = ImGui::CalcTextSize(value);
        float scale = 1.f;
        if (ts.x > win.x - 24.f) scale = (win.x - 24.f) / ts.x;
        ImGui::SetWindowFontScale(scale);
        ts = ImGui::CalcTextSize(value);
        ImGui::SetCursorPos(ImVec2((win.x - ts.x) * .5f, (win.y - ts.y) * .5f - 6.f));
        ImGui::TextColored(warn ? COL_RED : COL_INK, "%s", value);
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopFont();

        ImVec2 us = ImGui::CalcTextSize(unit);
        ImGui::SetCursorPos(ImVec2((win.x - us.x) * .5f,
                                   (win.y - ts.y * scale) * .5f + ts.y * scale - 2.f));
        ImGui::TextDisabled("%s", unit);
    }
    ImGui::End();
}

// ------------------------------------- map --------------------------------------
static void DrawMapWindow(WTClient& client, ImVec2 defPos, ImVec2 defSize)
{
    if (!g_cfg.showMap) return;
    ImGui::SetNextWindowPos(defPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(defSize, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("TACTICAL MAP", &g_cfg.showMap)) { ImGui::End(); return; }

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 60.f || avail.y < 60.f) { ImGui::End(); return; }
    ImVec2 p1(p0.x + avail.x, p0.y + avail.y);

    // AllowOverlap: without it, this full-size canvas steals clicks from the
    // FOLLOW/FIT buttons drawn on top of it (that was the reported bug).
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("canvas", avail);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(10, 12, 9, 255));

    ImGuiIO& io = ImGui::GetIO();

    // global hotkeys (work even while the game window has focus)
    if (g_hkZoomIn)  { g_zoom = std::clamp(g_zoom * 1.25f, 0.6f, 14.f); g_hkZoomIn = false; }
    if (g_hkZoomOut) { g_zoom = std::clamp(g_zoom / 1.25f, 0.6f, 14.f); g_hkZoomOut = false; }
    if (g_hkFollow)  { g_cfg.follow = !g_cfg.follow; g_cfgDirty = true; g_hkFollow = false; }

    if (g_mapSRV && g_mapW > 0) {
        if (hovered && io.MouseWheel != 0.f) {
            float f = io.MouseWheel > 0 ? 1.18f : 1.f / 1.18f;
            g_zoom = std::clamp(g_zoom * f, 0.6f, 14.f);
        }
        float S = std::min(avail.x / g_mapW, avail.y / g_mapH) * g_zoom;
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (g_cfg.follow) { g_cfg.follow = false; g_cfgDirty = true; }
            g_cx -= io.MouseDelta.x / (S * g_mapW);
            g_cy -= io.MouseDelta.y / (S * g_mapH);
        }
        auto pl = client.player();
        if (g_cfg.follow && pl) { g_cx = pl->x; g_cy = pl->y; }

        ImVec2 ctr(p0.x + avail.x * .5f, p0.y + avail.y * .5f);
        auto toX = [&](float nx) { return ctr.x + (nx - g_cx) * g_mapW * S; };
        auto toY = [&](float ny) { return ctr.y + (ny - g_cy) * g_mapH * S; };
        auto fromScreen = [&](ImVec2 sp) {
            return ImVec2(g_cx + (sp.x - ctr.x) / (g_mapW * S),
                          g_cy + (sp.y - ctr.y) / (g_mapH * S));
        };

        float Wm = client.mapWidthMeters(), Hm = client.mapHeightMeters();
        auto distMeters = [&](float ax, float ay, float bx, float by) -> float {
            if (Wm <= 0.f || Hm <= 0.f) return -1.f;
            return hypotf((ax - bx) * Wm, (ay - by) * Hm);
        };

        dl->PushClipRect(p0, p1, true);
        dl->AddImage((ImTextureID)(intptr_t)g_mapSRV,
                     ImVec2(toX(0.f), toY(0.f)), ImVec2(toX(1.f), toY(1.f)));

        float pulse = .55f + .45f * sinf(float(ImGui::GetTime()) * 4.5f);
        float ms = g_cfg.markerScale;
        auto withAlpha = [](uint32_t col, float a) {
            uint32_t A = uint32_t(std::clamp(a, 0.f, 1.f) * 255.f);
            return (col & 0x00FFFFFFu) | (A << 24);
        };

        // ---- range rings around the player ----
        if (g_cfg.showRings && pl && Wm > 0.f) {
            static const float rings[] = { 200.f, 500.f, 1000.f };
            for (float m : rings) {
                float rp = m / Wm * g_mapW * S;
                if (rp < 12.f) continue;
                ImVec2 c(toX(pl->x), toY(pl->y));
                dl->AddCircle(c, rp, AccentU32(0.28f), 0, 1.f);
                char lbl[16]; snprintf(lbl, sizeof lbl, "%.0fm", m);
                dl->AddText(ImVec2(c.x + 3.f, c.y - rp - 14.f), AccentU32(0.55f), lbl);
            }
        }

        // ---- ghosts: last known position, fading with age ----
        if (g_cfg.showGhosts) {
            for (const auto& gh : client.ghosts()) {
                float a = std::clamp(0.85f - gh.ageSec / 45.f * 0.7f, 0.15f, 0.85f);
                ImVec2 c(toX(gh.x), toY(gh.y));
                dl->AddCircle(c, 5.5f * ms, withAlpha(gh.color, a), 0, 1.6f);
                dl->AddCircleFilled(c, 1.8f * ms, withAlpha(gh.color, a));
                char lbl[16]; snprintf(lbl, sizeof lbl, "%.0fs", gh.ageSec);
                dl->AddText(ImVec2(c.x + 7.f * ms, c.y - 7.f * ms),
                            withAlpha(0xFFFFFFFFu, a * 0.9f), lbl);
            }
        }

        // ---- live objects ----
        float nearestEnemy = -1.f;
        for (const auto& o : client.mapObjects()) {
            float a = o.blink ? pulse : 1.f;
            uint32_t col = withAlpha(o.color, a);

            switch (o.kind) {
            case MapObj::Airfield:
                dl->AddLine(ImVec2(toX(o.sx), toY(o.sy)),
                            ImVec2(toX(o.ex), toY(o.ey)), col, 4.f * ms);
                break;
            case MapObj::Zone: {
                float x = toX(o.x), y = toY(o.y);
                float r = std::max(6.f * ms, o.r * g_mapW * S);
                dl->AddCircleFilled(ImVec2(x, y), r, withAlpha(o.color, a * .15f));
                dl->AddCircle(ImVec2(x, y), r, col, 0, 1.6f);
                break;
            }
            case MapObj::Respawn: {
                float x = toX(o.x), y = toY(o.y), r = 4.5f * ms;
                ImVec2 q[4] = { {x, y - r}, {x + r, y}, {x, y + r}, {x - r, y} };
                dl->AddQuadFilled(q[0], q[1], q[2], q[3], col);
                break;
            }
            case MapObj::Unit: {
                float x = toX(o.x), y = toY(o.y);
                float r = (o.isPlayer ? 9.f : 6.5f) * ms;
                float ang = atan2f(o.dy, o.dx), cs = cosf(ang), sn = sinf(ang);
                auto rot = [&](float px, float py) {
                    return ImVec2(x + px * cs - py * sn, y + px * sn + py * cs);
                };
                ImVec2 tip = rot(r, 0), l = rot(-.7f * r, .62f * r),
                       mid = rot(-.35f * r, 0), rr = rot(-.7f * r, -.62f * r);
                uint32_t fill = o.isPlayer ? withAlpha(0xFFFFFFFFu, a) : col;
                dl->AddTriangleFilled(tip, l, mid, fill);
                dl->AddTriangleFilled(tip, mid, rr, fill);
                if (o.isPlayer) {
                    ImVec2 poly[4] = { tip, l, mid, rr };
                    dl->AddPolyline(poly, 4, AccentU32(), ImDrawFlags_Closed, 1.6f);
                }
                // distance label on spotted enemies
                if (o.isEnemy && pl) {
                    float d = distMeters(o.x, o.y, pl->x, pl->y);
                    if (d >= 0.f) {
                        if (nearestEnemy < 0.f || d < nearestEnemy) nearestEnemy = d;
                        if (g_cfg.showDist) {
                            char lbl[16];
                            snprintf(lbl, sizeof lbl, d >= 1000.f ? "%.1fkm" : "%.0fm",
                                     d >= 1000.f ? d / 1000.f : d);
                            dl->AddText(ImVec2(x + 8.f * ms, y - 8.f * ms),
                                        withAlpha(0xFFFFFFFFu, 0.85f), lbl);
                        }
                    }
                }
                break;
            }
            case MapObj::Point:
                dl->AddCircleFilled(ImVec2(toX(o.x), toY(o.y)), 3.5f * ms, col);
                break;
            }
        }

        // ---- ruler: hold RIGHT mouse button to measure ----
        static bool measuring = false;
        static ImVec2 rulerStart;   // normalized coords
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            rulerStart = fromScreen(io.MousePos);
            measuring = true;
        }
        if (measuring && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            ImVec2 cur = fromScreen(io.MousePos);
            ImVec2 a(toX(rulerStart.x), toY(rulerStart.y));
            ImVec2 b(toX(cur.x), toY(cur.y));
            dl->AddLine(a, b, AccentU32(0.9f), 2.f);
            dl->AddCircleFilled(a, 3.f, AccentU32());
            float d = distMeters(rulerStart.x, rulerStart.y, cur.x, cur.y);
            char lbl[24];
            if (d >= 0.f)
                snprintf(lbl, sizeof lbl, d >= 1000.f ? "%.2f km" : "%.0f m",
                         d >= 1000.f ? d / 1000.f : d);
            else
                snprintf(lbl, sizeof lbl, "n/a");
            dl->AddText(ImVec2(b.x + 10.f, b.y - 6.f), AccentU32(), lbl);
        } else {
            measuring = false;
        }

        // ---- proximity alarm ----
        if (g_cfg.alarmOn && nearestEnemy >= 0.f && nearestEnemy <= g_cfg.alarmMeters) {
            float fl = .35f + .45f * (0.5f + 0.5f * sinf(float(ImGui::GetTime()) * 9.f));
            dl->AddRect(p0, p1, IM_COL32(208, 85, 69, int(fl * 255)), 0.f, 0, 4.f);
            static double lastBeep = 0.0;
            double tnow = ImGui::GetTime();
            if (tnow - lastBeep > 3.0) {
                lastBeep = tnow;
                std::thread([] { Beep(880, 140); }).detach();
            }
        }

        dl->PopClipRect();
    } else {
        const char* msg = "NO MAP SIGNAL — JOIN A MATCH";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (avail.x - ts.x) * .5f, p0.y + (avail.y - ts.y) * .5f),
                    ImGui::GetColorU32(COL_DIM), msg);
    }

    // overlay controls (bottom right)
    ImVec2 btnPos(p0.x + avail.x - 130.f, p0.y + avail.y - 30.f);
    ImGui::SetCursorScreenPos(btnPos);
    if (g_cfg.follow) ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.35f));
    if (ImGui::SmallButton("FOLLOW")) { g_cfg.follow = !g_cfg.follow; g_cfgDirty = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep the map centered on your vehicle");
    if (g_cfg.follow) ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("FIT")) {
        g_zoom = 1.f; g_cx = g_cy = .5f;
        if (g_cfg.follow) { g_cfg.follow = false; g_cfgDirty = true; }
    }

    ImGui::End();
}

// --------------------------------- instruments ----------------------------------
static std::string PrettyVehicle(const std::string& id)
{
    std::string s = id;
    for (char& c : s) { if (c == '_') c = ' '; else c = char(toupper((unsigned char)c)); }
    return s.empty() ? "—" : s;
}

static void DrawInstruments(WTClient& client, const ImVec2& vp)
{
    Telemetry t = client.telemetry();
    char buf[64];

    float colX = vp.x * .60f, colW = vp.x * .19f, rowH = vp.y * .17f;

    // SPEED
    {
        double v = -1; const char* unit = "KM/H";
        if (t.ias)      { v = *t.ias; unit = "IAS · KM/H"; }
        else if (t.tas) { v = *t.tas; unit = "TAS · KM/H"; }
        else if (t.speedInd) v = *t.speedInd;
        if (v >= 0) snprintf(buf, sizeof buf, "%.0f", v); else snprintf(buf, sizeof buf, "—");
        BigValueWindow("SPEED", &g_cfg.showSpeed, buf, unit, false,
                       ImVec2(colX, vp.y * .05f), ImVec2(colW, rowH));
    }
    // ALTITUDE
    {
        if (t.alt) snprintf(buf, sizeof buf, "%.0f", *t.alt); else snprintf(buf, sizeof buf, "—");
        BigValueWindow("ALTITUDE", &g_cfg.showAlt, buf, "M", false,
                       ImVec2(colX + colW + 10.f, vp.y * .05f), ImVec2(colW, rowH));
    }
    // HEADING
    {
        double h = 0; bool hasH = false;
        if (t.compass) { h = *t.compass; hasH = true; }
        else if (auto pl = client.player(); pl && (pl->dx != 0.f || pl->dy != 0.f)) {
            h = atan2(pl->dx, -pl->dy) * 180.0 / 3.14159265358979;
            hasH = true;
        }
        const char* card = "—";
        if (hasH) {
            h = fmod(fmod(h, 360.0) + 360.0, 360.0);
            static const char* dirs[16] = { "N","NNE","NE","ENE","E","ESE","SE","SSE",
                                            "S","SSW","SW","WSW","W","WNW","NW","NNW" };
            card = dirs[int(h / 22.5 + .5) % 16];
            snprintf(buf, sizeof buf, "%03.0f\xC2\xB0", h);
        } else snprintf(buf, sizeof buf, "—");
        BigValueWindow("HEADING", &g_cfg.showHeading, buf, card, false,
                       ImVec2(colX, vp.y * .24f), ImVec2(colW * 2.f + 10.f, vp.y * .13f));
    }
    // CLIMB RATE
    {
        bool warn = t.vy && *t.vy < -20.0;
        if (t.vy) snprintf(buf, sizeof buf, "%+.1f", *t.vy); else snprintf(buf, sizeof buf, "—");
        BigValueWindow("CLIMB", &g_cfg.showVario, buf, "M/S", warn,
                       ImVec2(colX, vp.y * .39f), ImVec2(colW, vp.y * .13f));
    }
    // THROTTLE
    {
        if (t.throttle) snprintf(buf, sizeof buf, "%.0f", *t.throttle);
        else snprintf(buf, sizeof buf, "—");
        BigValueWindow("THROTTLE", &g_cfg.showThrottle, buf, "%", false,
                       ImVec2(colX, vp.y * .54f), ImVec2(colW, vp.y * .13f));
    }
    // FUEL (with bar)
    if (g_cfg.showFuel) {
        ImGui::SetNextWindowPos(ImVec2(colX + colW + 10.f, vp.y * .39f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(colW, vp.y * .13f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("FUEL", &g_cfg.showFuel)) {
            float frac = 0.f;
            bool low = false;
            if (t.fuel && t.fuel0 && *t.fuel0 > 0) {
                frac = float(std::clamp(*t.fuel / *t.fuel0, 0.0, 1.0));
                low = frac < .18f;
                snprintf(buf, sizeof buf, "%.0f", *t.fuel);
            } else snprintf(buf, sizeof buf, "—");

            ImVec2 win = ImGui::GetWindowSize();
            ImGui::PushFont(g_fontBig);
            ImVec2 ts = ImGui::CalcTextSize(buf);
            ImGui::SetCursorPos(ImVec2((win.x - ts.x) * .5f, (win.y - ts.y) * .5f - 12.f));
            ImGui::TextColored(low ? COL_RED : COL_INK, "%s", buf);
            ImGui::PopFont();

            ImGui::SetCursorPos(ImVec2(win.x * .1f, win.y - 26.f));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, low ? COL_RED : COL_GREEN);
            ImGui::ProgressBar(frac, ImVec2(win.x * .8f, 8.f), "");
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }
    // VEHICLE
    {
        std::string name = PrettyVehicle(t.vehicle);
        BigValueWindow("VEHICLE", &g_cfg.showVehicle, name.c_str(),
                       t.army.empty() ? "—" : t.army.c_str(), false,
                       ImVec2(colX, vp.y * .69f), ImVec2(colW * 2.f + 10.f, vp.y * .11f));
    }
    // ENGINE (planes): temps, G, estimated fuel time
    if (g_cfg.showEngine) {
        ImGui::SetNextWindowPos(ImVec2(colX, vp.y * .69f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(colW * 2.f + 10.f, vp.y * .14f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("ENGINE", &g_cfg.showEngine)) {
            // fuel-burn estimate over a sliding 30 s window
            static std::deque<std::pair<double, double>> samples;   // (time, fuel kg)
            static double lastSample = 0.0;
            double tnow = ImGui::GetTime();
            if (t.fuel && tnow - lastSample > 1.0) {
                lastSample = tnow;
                samples.emplace_back(tnow, *t.fuel);
                while (!samples.empty() && tnow - samples.front().first > 30.0)
                    samples.pop_front();
            }
            if (!t.fuel) samples.clear();

            if (ImGui::BeginTable("eng", 4, ImGuiTableFlags_SizingStretchSame)) {
                auto cell = [&](const char* label, const char* val, bool warn) {
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", label);
                    ImGui::TextColored(warn ? COL_RED : COL_INK, "%s", val);
                };
                char v[32];
                if (t.oilTemp) snprintf(v, sizeof v, "%.0f\xC2\xB0", *t.oilTemp);
                else snprintf(v, sizeof v, "—");
                cell("OIL", v, t.oilTemp && *t.oilTemp > 110.0);

                if (t.waterTemp) snprintf(v, sizeof v, "%.0f\xC2\xB0", *t.waterTemp);
                else snprintf(v, sizeof v, "—");
                cell("WATER", v, t.waterTemp && *t.waterTemp > 115.0);

                if (t.ny) snprintf(v, sizeof v, "%+.1f", *t.ny);
                else snprintf(v, sizeof v, "—");
                cell("G", v, t.ny && (*t.ny > 9.0 || *t.ny < -3.0));

                bool haveEst = false;
                if (t.fuel && samples.size() >= 5) {
                    double dt = samples.back().first - samples.front().first;
                    double df = samples.front().second - samples.back().second;
                    if (dt > 5.0 && df > 0.01) {
                        double minutes = *t.fuel / (df / dt) / 60.0;
                        if (minutes < 600.0) {
                            snprintf(v, sizeof v, "%.0f min", minutes);
                            haveEst = true;
                            cell("FUEL TIME", v, minutes < 5.0);
                        }
                    }
                }
                if (!haveEst) cell("FUEL TIME", "—", false);
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
    // DAMAGE FEED
    if (g_cfg.showDamage) {
        ImGui::SetNextWindowPos(ImVec2(colX, vp.y * .55f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(colW * 2.f + 10.f, vp.y * .40f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("DAMAGE FEED", &g_cfg.showDamage)) {
            static size_t prevCount = 0;
            auto log = client.damageLog();
            ImGui::BeginChild("scroll", ImVec2(0, 0), ImGuiChildFlags_None);
            if (log.empty()) ImGui::TextDisabled("NO EVENTS YET");
            for (const auto& m : log) {
                ImGui::TextDisabled("%d:%02d", m.time / 60, m.time % 60);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text,
                    m.enemy ? ImVec4(0.788f, 0.643f, 0.541f, 1.f) : COL_DIM);
                ImGui::TextWrapped("%s", m.msg.c_str());
                ImGui::PopStyleColor();
            }
            if (log.size() != prevCount) {
                ImGui::SetScrollHereY(1.f);
                prevCount = log.size();
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }
}

// ---------------------------------- scoreboard ----------------------------------
static void DrawScoreboard(WTClient& client, const ImVec2& vp)
{
    if (!g_cfg.showScore) return;
    ImGui::SetNextWindowPos(ImVec2(vp.x * .02f, vp.y * .06f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(vp.x * .28f, vp.y * .45f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("SCOREBOARD", &g_cfg.showScore)) {
        auto sb = client.scoreboard();
        int active = 0, gone = 0;
        for (const auto& p : sb) (p.left ? gone : active)++;

        ImGui::TextColored(COL_GREEN, "ACTIVE: %d", active);
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine(); ImGui::TextColored(gone > 0 ? COL_RED : COL_DIM, "LEFT: %d", gone);
        ImGui::SameLine(ImGui::GetWindowWidth() - 66.f);
        if (ImGui::SmallButton("Clear")) client.clearScores();

        std::sort(sb.begin(), sb.end(), [](const PlayerScore& a, const PlayerScore& b) {
            if (a.kills != b.kills) return a.kills > b.kills;
            if (a.deaths != b.deaths) return a.deaths < b.deaths;
            return a.name < b.name;
        });

        if (sb.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Waiting for match events...");
        } else if (ImGui::BeginTable("score", 3,
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                       ImGuiTableFlags_BordersInnerH,
                       ImVec2(0.f, -22.f))) {
            ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("KILLS",  ImGuiTableColumnFlags_WidthFixed, 52.f);
            ImGui::TableSetupColumn("DEATHS", ImGuiTableColumnFlags_WidthFixed, 56.f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const auto& p : sb) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (p.left) ImGui::TextDisabled("%s (left)", p.name.c_str());
                else        ImGui::TextUnformatted(p.name.c_str());
                ImGui::TableNextColumn();
                if (p.kills > 0) ImGui::TextColored(COL_GREEN, "%d", p.kills);
                else             ImGui::TextDisabled("0");
                ImGui::TableNextColumn();
                if (p.deaths > 0) ImGui::TextColored(COL_RED, "%d", p.deaths);
                else              ImGui::TextDisabled("0");
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("From the event feed · saved to session_stats.csv");
    }
    ImGui::End();
}

// ------------------------------------- chat -------------------------------------
static void DrawChat(WTClient& client, const ImVec2& vp)
{
    if (!g_cfg.showChat) return;
    ImGui::SetNextWindowPos(ImVec2(vp.x * .02f, vp.y * .55f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(vp.x * .28f, vp.y * .38f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("GAME CHAT", &g_cfg.showChat)) {
        static size_t prevCount = 0;
        auto log = client.chatLog();
        ImGui::BeginChild("chatscroll", ImVec2(0, 0), ImGuiChildFlags_None);
        if (log.empty()) ImGui::TextDisabled("NO MESSAGES YET");
        for (const auto& m : log) {
            if (!m.sender.empty()) {
                ImGui::TextColored(m.enemy ? COL_RED : COL_GREEN, "%s:", m.sender.c_str());
                ImGui::SameLine();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, COL_INK);
            ImGui::TextWrapped("%s", m.msg.c_str());
            ImGui::PopStyleColor();
        }
        if (log.size() != prevCount) {
            ImGui::SetScrollHereY(1.f);
            prevCount = log.size();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// ---------------------------------- objectives ----------------------------------
static void DrawObjectives(WTClient& client, const ImVec2& vp)
{
    if (!g_cfg.showObj) return;
    ImGui::SetNextWindowPos(ImVec2(vp.x * .32f, vp.y * .06f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(vp.x * .26f, vp.y * .25f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("OBJECTIVES", &g_cfg.showObj)) {
        auto list = client.objectives();
        if (list.empty()) {
            ImGui::TextDisabled("NO ACTIVE OBJECTIVES");
        } else {
            for (const auto& o : list) {
                ImVec4 col = COL_INK;
                const char* mark = ">";
                if (o.status == "completed") { col = COL_GREEN; mark = "+"; }
                else if (o.status == "failed") { col = COL_RED; mark = "x"; }
                ImGui::TextColored(o.primary ? Accent() : COL_DIM, "%s", mark);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextWrapped("%s", o.text.c_str());
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::End();
}

// ------------------------------- widgets window ---------------------------------
static void DrawWidgetsWindow()
{
    if (!g_showWidgets) return;
    if (ImGui::Begin("WIDGETS", &g_showWidgets, ImGuiWindowFlags_AlwaysAutoResize)) {
        bool ch = false;
        ImGui::TextDisabled("MAP");
        ch |= ImGui::Checkbox("Tactical map",   &g_cfg.showMap);
        ImGui::Spacing();
        ImGui::TextDisabled("INSTRUMENTS");
        ch |= ImGui::Checkbox("Speed",          &g_cfg.showSpeed);
        ch |= ImGui::Checkbox("Altitude",       &g_cfg.showAlt);
        ch |= ImGui::Checkbox("Heading",        &g_cfg.showHeading);
        ch |= ImGui::Checkbox("Climb rate",     &g_cfg.showVario);
        ch |= ImGui::Checkbox("Fuel",           &g_cfg.showFuel);
        ch |= ImGui::Checkbox("Throttle",       &g_cfg.showThrottle);
        ch |= ImGui::Checkbox("Vehicle",        &g_cfg.showVehicle);
        ch |= ImGui::Checkbox("Engine",         &g_cfg.showEngine);
        ImGui::Spacing();
        ImGui::TextDisabled("MATCH");
        ch |= ImGui::Checkbox("Damage feed",    &g_cfg.showDamage);
        ch |= ImGui::Checkbox("Scoreboard",     &g_cfg.showScore);
        ch |= ImGui::Checkbox("Game chat",      &g_cfg.showChat);
        ch |= ImGui::Checkbox("Objectives",     &g_cfg.showObj);
        if (ch) g_cfgDirty = true;
    }
    ImGui::End();
}

// --------------------------------- settings -------------------------------------
static void DrawSettings(HWND hwnd)
{
    if (!g_showSettings) return;
    if (ImGui::Begin("SETTINGS", &g_showSettings,
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        bool ch = false;

        ImGui::TextDisabled("APPEARANCE");
        ch |= ImGui::ColorEdit3("Window color", g_cfg.colBg,
                                ImGuiColorEditFlags_NoInputs);
        ch |= ImGui::ColorEdit3("Accent color", g_cfg.colAccent,
                                ImGuiColorEditFlags_NoInputs);

        ImGui::Spacing();
        ImGui::TextDisabled("MAP");
        ch |= ImGui::SliderFloat("Marker size", &g_cfg.markerScale,
                                 0.5f, 3.f, "%.1fx");
        if (ImGui::Checkbox("Follow my vehicle", &g_cfg.follow)) ch = true;
        ch |= ImGui::Checkbox("Distance labels on enemies", &g_cfg.showDist);
        ch |= ImGui::Checkbox("Range rings (200/500/1000 m)", &g_cfg.showRings);
        ch |= ImGui::Checkbox("Last-known-position ghosts", &g_cfg.showGhosts);
        ImGui::TextDisabled("Hold RIGHT mouse button on the map to measure.");

        ImGui::Spacing();
        ImGui::TextDisabled("ALERTS");
        ch |= ImGui::Checkbox("Proximity alarm", &g_cfg.alarmOn);
        if (g_cfg.alarmOn)
            ch |= ImGui::SliderFloat("Alarm distance", &g_cfg.alarmMeters,
                                     100.f, 2000.f, "%.0f m");

        ImGui::Spacing();
        ImGui::TextDisabled("INPUT");
        if (ImGui::Checkbox("Global hotkeys", &g_cfg.hotkeys)) {
            ApplyHotkeys(hwnd);
            ch = true;
        }
        ImGui::TextDisabled("Numpad + / -  zoom map   Numpad *  toggle follow");
        ImGui::TextDisabled("(work even while the game has focus)");

        ImGui::Spacing();
        if (ImGui::Button("Restore defaults")) {
            const AppCfg d;
            for (int i = 0; i < 3; ++i) {
                g_cfg.colBg[i] = d.colBg[i];
                g_cfg.colAccent[i] = d.colAccent[i];
            }
            g_cfg.markerScale = d.markerScale;
            g_cfg.showDist = d.showDist;
            g_cfg.showRings = d.showRings;
            g_cfg.showGhosts = d.showGhosts;
            g_cfg.alarmOn = d.alarmOn;
            g_cfg.alarmMeters = d.alarmMeters;
            ch = true;
        }

        if (ch) { ApplyStyle(); g_cfgDirty = true; }
    }
    ImGui::End();
}

// ---------------------------------- menu bar ------------------------------------
static void DrawMenuBar(WTClient& client, HWND hwnd, bool& quit)
{
    if (!ImGui::BeginMainMenuBar()) return;

    ImGui::TextColored(Accent(), "WT SECOND SCREEN");
    ImGui::Separator();

    switch (client.conn()) {
    case ConnState::InMatch: ImGui::TextColored(COL_GREEN, "IN MATCH"); break;
    case ConnState::Menu:    ImGui::TextColored(Accent(),  "IN HANGAR / MENU"); break;
    default:                 ImGui::TextColored(COL_DIM,   "GAME NOT DETECTED"); break;
    }
    ImGui::Separator();

    if (ImGui::MenuItem("Widgets"))  g_showWidgets = !g_showWidgets;
    if (ImGui::MenuItem("Settings")) g_showSettings = !g_showSettings;

    if (ImGui::BeginMenu("Monitor")) {
        for (int i = 0; i < int(g_monitors.size()); ++i) {
            const RECT& r = g_monitors[i].rc;
            char lbl[64];
            snprintf(lbl, sizeof lbl, "Monitor %d — %ldx%ld", i + 1,
                     r.right - r.left, r.bottom - r.top);
            int cur = g_cfg.monitor < 0 ? (g_monitors.size() > 1 ? 1 : 0) : g_cfg.monitor;
            if (ImGui::MenuItem(lbl, nullptr, cur == i)) {
                g_cfg.monitor = i;
                ApplyMonitor(hwnd);
                g_cfgDirty = true;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Always on top", nullptr, &g_cfg.onTop)) {
            ApplyMonitor(hwnd);
            g_cfgDirty = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("App")) {
        ImGui::TextDisabled("Drag windows anywhere:");
        ImGui::TextDisabled("the layout saves itself.");
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) quit = true;
        ImGui::EndMenu();
    }

    // ---- right side: Donate button + version tag ----
    const char* verTxt = "ALPHA 0.1";
    float verW = ImGui::CalcTextSize(verTxt).x;
    float btnW = ImGui::CalcTextSize("Donate").x +
                 ImGui::GetStyle().FramePadding.x * 2.f;
    ImGui::SameLine(ImGui::GetWindowWidth() - verW - btnW - 26.f);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.95f, 0.55f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.66f, 0.20f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.85f, 0.47f, 0.05f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.08f, 0.06f, 0.02f, 1.f));
    if (ImGui::SmallButton("Donate"))
        ShellExecuteW(nullptr, L"open", L"https://paypal.me/izansorce86",
                      nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Support the project on PayPal");
    ImGui::SameLine(ImGui::GetWindowWidth() - verW - 10.f);
    ImGui::TextDisabled("%s", verTxt);

    ImGui::EndMainMenuBar();
}

// ------------------------------------ WndProc ------------------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_resizeW = LOWORD(lParam);
            g_resizeH = HIWORD(lParam);
        }
        return 0;
    case WM_HOTKEY:
        if (wParam == HK_ZOOM_IN)  g_hkZoomIn = true;
        if (wParam == HK_ZOOM_OUT) g_hkZoomOut = true;
        if (wParam == HK_FOLLOW)   g_hkFollow = true;
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;  // no ALT menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ------------------------------------ WinMain ------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    LoadCfg();
    EnumMonitors();

    int mi = g_cfg.monitor;
    if (mi < 0) mi = g_monitors.size() > 1 ? 1 : 0;
    mi = std::clamp(mi, 0, std::max(0, int(g_monitors.size()) - 1));
    RECT mr = g_monitors.empty() ? RECT{0, 0, 1280, 720} : g_monitors[mi].rc;

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst,
                       nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr,
                       L"WTSecondScreen", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(
        g_cfg.onTop ? WS_EX_TOPMOST : 0, wc.lpszClassName, L"WT Second Screen",
        WS_POPUP, mr.left, mr.top, mr.right - mr.left, mr.bottom - mr.top,
        nullptr, nullptr, hInst, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInst);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    ApplyHotkeys(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyStyle();

    g_fontUI  = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 17.f);
    g_fontBig = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolab.ttf", 46.f);
    if (!g_fontUI)  g_fontUI  = io.Fonts->AddFontDefault();
    if (!g_fontBig) g_fontBig = g_fontUI;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    WTClient client;
    client.start();

    bool quit = false;
    while (!quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) quit = true;
        }
        if (quit) break;

        if (g_resizeW != 0 && g_resizeH != 0) {
            CleanupRenderTarget();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        {
            std::vector<uint8_t> rgba; int w = 0, h = 0;
            if (client.takeMapImage(rgba, w, h)) UploadMapTexture(rgba, w, h);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImVec2 vp = ImGui::GetMainViewport()->Size;
        DrawMenuBar(client, hwnd, quit);
        DrawMapWindow(client, ImVec2(vp.x * .01f, vp.y * .05f),
                      ImVec2(vp.x * .57f, vp.y * .92f));
        DrawInstruments(client, vp);
        DrawScoreboard(client, vp);
        DrawChat(client, vp);
        DrawObjectives(client, vp);
        DrawWidgetsWindow();
        DrawSettings(hwnd);

        if (g_cfgDirty) SaveCfg();

        ImGui::Render();
        const float clear[4] = { 0.043f, 0.051f, 0.039f, 1.f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    SaveCfg();
    client.stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
