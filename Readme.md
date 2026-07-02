# SMAPI Silent Launcher
Replaces the SMAPI console window with a small overlay that fades in while the game loads, then disappears. Nothing else changes.
**[> Download on Nexus Mods](https://www.nexusmods.com/stardewvalley/mods/48315)**
**Windows only. Requires SMAPI.**
---
## Features
- No console window on launch
- Status overlay shows mod and content pack counts, with a live countdown timer
- Right-click the overlay to abort the launch
- Configurable timeout via `--timeout=N`
- Detects SMAPI crashes during launch instead of hanging
- Refuses to launch if SMAPI or the game is already running
- Steam achievements, overlay, and playtime tracking all work fine
- No dependencies, no installs, single exe
- Does not modify SMAPI, the game, or any mods
---
## Installation
Drop `SMAPI SilentLauncher.exe` into your Stardew Valley folder, the same folder as `StardewModdingAPI.exe`.
**Steam**
Go to your Stardew Valley properties in Steam, then Launch Options, and set:
```
"<GameDirectory>\SMAPI SilentLauncher.exe" --mods-path "mods" --timeout 60 %command%
```
Example (change "mods" to your modpack's folder name if it differs):
```
"D:\Steam\steamapps\common\Stardew Valley\SMAPI SilentLauncher.exe" --mods-path "mods" --timeout 60 %command%
```
Launching this way counts identically to launching SMAPI directly, so Steam achievements, overlay, and playtime all stay active.
**Shortcut / other launcher**
Point it at `SMAPI SilentLauncher.exe` and pass any arguments you normally would. They get forwarded to SMAPI as-is. 
Supports `--mods-path` and `--timeout=N`.
---
## Antivirus warnings
Windows SmartScreen may warn you on first run since the exe is not code-signed. Hit **More info > Run anyway**.
Some antivirus tools may also flag it. The launcher hides the console window using a technique that overlaps with how some malware behaves, even though nothing malicious is going on here. Full source on GitHub.
---
## Errors
| Message | Fix |
|---|---|
| Wrong folder | Move `SMAPI SilentLauncher.exe` next to `Stardew Valley.exe` |
| SMAPI is not installed | Install SMAPI from [smapi.io](https://smapi.io) |
| Already running | Close the game or SMAPI first |
| SMAPI closed before the game started | SMAPI crashed or exited during launch, check SMAPI's log for the cause |
| Launch timed out | The 60s default timeout expired, increase it with `-timeout N` if you have a large modpack |
---
Open source, MIT license.
---
## Building from source
Requires MSVC (`cl` and `rc` on your PATH, run from a Developer Command Prompt for VS).
```
rc launcher.rc
cl launcher.c launcher.res "/Fe:SMAPI SilentLauncher.exe" /link user32.lib gdi32.lib /subsystem:windows
```
