@echo off
rc launcher.rc
cl launcher.c launcher.res "/Fe:SMAPI SilentLauncher.exe" /link user32.lib gdi32.lib /subsystem:windows
echo Done. SMAPISilentLauncher.exe is ready.
pause
