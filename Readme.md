# SMAPI Silent Launcher

Tired of the console window sitting open every time you launch Stardew Valley with SMAPI? This replaces it with a small overlay that fades in while the game loads, then disappears. Nothing else changes.

**[> Download on Nexus Mods](https://www.nexusmods.com/stardewvalley/mods/48315)**

**Windows only. Requires SMAPI.**

---

## Features

- No console window on launch
- Small status overlay while the game loads, fades out automatically
- Steam achievements, overlay, and playtime tracking all work fine
- No dependencies, no installs, single exe
- Does not modify SMAPI, the game, or any mods

---

## Installation

Drop `SMAPI SilentLauncher.exe` into your Stardew Valley folder, the same folder as `StardewModdingAPI.exe`.

**Steam**

Go to your Stardew Valley properties in Steam, then Launch Options, and set:

```
"<GameDirectory>\SMAPI SilentLauncher.exe" %command%
```

Launching this way counts identically to launching SMAPI directly, so Steam achievements, overlay, and playtime all stay active.

**GOG**

Point GOG Galaxy at `SMAPI SilentLauncher.exe` instead of `StardewModdingAPI.exe`.

**Shortcut / other launcher**

Just point it at `SMAPI SilentLauncher.exe` and pass any arguments you normally would. They get forwarded to SMAPI as-is.

---

## Antivirus warnings

Windows SmartScreen may warn you on first run since the exe is not code-signed. Hit **More info > Run anyway**.

Some antivirus tools may also flag it. The launcher hides the console window using a technique that overlaps with how some malware behaves, even though nothing malicious is going on here. The full source is on GitHub if you want to read it or build it yourself.

---

## Errors

| Message | Fix |
|---|---|
| Wrong folder | Move `SMAPI SilentLauncher.exe` next to `Stardew Valley.exe` |
| SMAPI is not installed | Install SMAPI from [smapi.io](https://smapi.io) |
| Already running | Close the game or SMAPI first |
| Console appeared anyway | Launcher timed out, check SMAPI output for what went wrong |

---

Open source, MIT license.

---

## Building from source

Requires MSVC (`cl` and `rc` on your PATH — run from a Developer Command Prompt for VS).

```
rc launcher.rc
cl launcher.c launcher.res /link user32.lib gdi32.lib /subsystem:windows
```
