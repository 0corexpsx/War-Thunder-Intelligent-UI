# War Thunder Intelligent UI
Initial release — alpha 0.1

Disclaimer

This is an unofficial fan-made tool, not affiliated with, endorsed by, or connected to Gaijin Entertainment. War Thunder is a trademark of Gaijin Entertainment. This tool only reads the localhost interface that the game itself provides for second-screen use.
War Thunder Intelligent UI

![alt text]([http://url/to/img.png](https://github.com/0corexpsx/War-Thunder-Intelligent-UI/blob/main/1Enz61M71v.jpg))

Tactical map, instruments, live scoreboard, ghosts and proximity alerts for War Thunder — on your second monitor.

Native Windows app (C++ / Dear ImGui / DirectX 11). It reads the game's official local telemetry server (localhost:8111): no memory reading, no injection, no ban risk. It only shows enemies the game has already spotted — the same information as the in-game minimap.

Why not just open localhost:8111 in a browser?

War Thunder ships a basic web page on localhost:8111 (map + simple flight instruments). This app uses the same data source, but turns it into an actual second-screen tool:

	Stock 8111 web page	War Thunder Intelligent UI
Tactical map	basic, fixed view	zoom, pan, FOLLOW your vehicle, marker size control
Enemy info	current markers only	ghosts (last known position with age), distance labels in meters
Measuring	—	right-click ruler, 200/500/1000 m range rings
Alerts	—	proximity alarm (red flash + beep at your distance threshold)
Scoreboard	—	live kills/deaths/leavers, auto-reset every round, CSV match history
Instruments	aircraft gauges page	speed, altitude, heading, climb, fuel bar, throttle, engine temps + G + estimated fuel time
Extra windows	—	damage feed, game chat, mission objectives
Layout	fixed webpage	every window drag/resize/toggle, saved automatically
Look	fixed	window + accent color themes
Second monitor	manual browser window	borderless fullscreen on the monitor you pick, always-on-top
Controls in game	—	global hotkeys (Numpad +/− zoom, * FOLLOW) that work while the game has focus
Runs as	browser tab	tiny native exe (~1 MB)
Features
Tactical map: zoom, pan, FOLLOW mode, auto-reload every match
Ghosts: fading last-known-position markers when a spotted enemy disappears
Real distances: meter labels on enemies, right-click ruler, range rings
Proximity alarm: border flash + beep when an enemy is spotted within a set distance
Live scoreboard: kills / deaths / active players / leavers, auto-reset every round, every match archived to session_stats.csv
Instruments: speed, altitude, heading, climb rate, fuel with bar, throttle, vehicle, engine temps + G load + estimated fuel time (aircraft)
Damage feed, game chat, mission objectives
Global hotkeys, customizable colors, marker size, monitor selection, always-on-top
Building from source

Requires Visual Studio 2022/2026 with the Desktop development with C++ workload. Open War Thunder Intelligent UI.sln, select x64 / Release, build and run — or open the folder as a CMake project and use the x64-release preset.

All dependencies (Dear ImGui, nlohmann/json, stb_image) are vendored in third_party/ — no package manager needed.

Support

If this tool helps you, you can buy me a coffee via PayPal ☕


