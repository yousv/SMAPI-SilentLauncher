#include <windows.h>
#include <tlhelp32.h>
#include <string.h>
#include <stdio.h>

#define UI_TIMER_ID          1
#define OVERLAY_TICK_MS      16
#define POLL_INTERVAL_MS     250
#define FADE_DURATION_MS     350
#define LAUNCH_TIMEOUT_MS    30000
#define POPUP_DISPLAY_MS     5000
#define OVERLAY_WIDTH_PX     280
#define OVERLAY_HEIGHT_PX    60
#define CORNER_RADIUS_PX     10
#define FADE_ALPHA_MAX       215
#define TOP_OFFSET_PERCENT   10

typedef enum {
    OVERLAY_LAUNCHING,
    OVERLAY_SUCCESS,
    OVERLAY_WARNING,
    OVERLAY_CRASHED,
    OVERLAY_TIMED_OUT
} OverlayState;

static HWND        g_overlayWindow;
static OverlayState g_overlayState   = OVERLAY_LAUNCHING;
static wchar_t     g_overlayMessage[256] = L"Launching SMAPI";
static int         g_elapsedMs        = 0;
static int         g_currentDurationMs = LAUNCH_TIMEOUT_MS;
static HANDLE      g_smapiProcess     = NULL;
static DWORD       g_smapiProcessId   = 0;
static char        g_launcherDir[MAX_PATH];
static char        g_smapiExePath[MAX_PATH];
static char        g_launchArgs[32768] = "";
static int         g_alpha            = 0;
static BOOL        g_fadingOut        = FALSE;
static ULONGLONG   g_stateStartMs     = 0;
static ULONGLONG   g_fadeStartMs      = 0;
static ULONGLONG   g_pollAccumMs      = 0;

static void GetPathNextToLauncher(char *outPath, size_t outSize, const char *fileName) {
    char launcherPath[MAX_PATH];
    GetModuleFileNameA(NULL, launcherPath, MAX_PATH);
    char *lastSlash = strrchr(launcherPath, '\\');
    if (lastSlash) *lastSlash = '\0';
    snprintf(outPath, outSize, "%s\\%s", launcherPath, fileName);
}

static BOOL IsProcessRunning(const wchar_t *processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return FALSE;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    BOOL found = FALSE;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName) == 0) { found = TRUE; break; }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

static BOOL CALLBACK FindGameWindow(HWND window, LPARAM result) {
    if (!IsWindowVisible(window)) return TRUE;
    DWORD ownerProcessId = 0;
    GetWindowThreadProcessId(window, &ownerProcessId);
    if (ownerProcessId != g_smapiProcessId) return TRUE;
    wchar_t title[64];
    if (GetWindowTextW(window, title, 64) <= 0) return TRUE;
    if (wcsncmp(title, L"Stardew Valley", 14) == 0) {
        *((BOOL *)result) = TRUE;
        return FALSE;
    }
    return TRUE;
}

static BOOL GameWindowIsOpen(void) {
    BOOL found = FALSE;
    EnumWindows(FindGameWindow, (LPARAM)&found);
    return found;
}

static BOOL LaunchSmapi(const char *extraArgs, BOOL showConsole, HANDLE *outProcess, DWORD *outProcessId) {
    char commandLine[32768];
    snprintf(commandLine, sizeof(commandLine), "\"%s\" %s", g_smapiExePath, extraArgs);
    STARTUPINFOA startupInfo;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo;
    ZeroMemory(&processInfo, sizeof(processInfo));
    DWORD creationFlags = showConsole ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
    BOOL launched = CreateProcessA(
        g_smapiExePath, commandLine, NULL, NULL, FALSE,
        creationFlags, NULL, g_launcherDir, &startupInfo, &processInfo
    );
    if (!launched) return FALSE;
    CloseHandle(processInfo.hThread);
    *outProcess = processInfo.hProcess;
    *outProcessId = processInfo.dwProcessId;
    return TRUE;
}

static void SwitchOverlayState(HWND window, OverlayState newState, const wchar_t *message, int durationMs) {
    g_overlayState      = newState;
    wcsncpy(g_overlayMessage, message, 255);
    g_overlayMessage[255] = L'\0';
    g_elapsedMs         = 0;
    g_currentDurationMs = durationMs;
    g_stateStartMs      = GetTickCount64();
    g_pollAccumMs       = 0;
    InvalidateRect(window, NULL, TRUE);
}

static COLORREF GetAccentColor(OverlayState state) {
    if (state == OVERLAY_SUCCESS)   return RGB(120, 220, 140);
    if (state == OVERLAY_LAUNCHING) return RGB(110, 150, 220);
    return RGB(235, 120, 110);
}

static LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            RECT clientRect;
            GetClientRect(window, &clientRect);

            HBRUSH background = CreateSolidBrush(RGB(32, 32, 34));
            FillRect(dc, &clientRect, background);
            DeleteObject(background);

            COLORREF accent = GetAccentColor(g_overlayState);

            RECT textRect    = clientRect;
            textRect.bottom -= 5;
            SetTextColor(dc, accent);
            SetBkMode(dc, TRANSPARENT);

            HFONT font = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT previousFont = (HFONT)SelectObject(dc, font);
            DrawTextW(dc, g_overlayMessage, -1, &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_WORDBREAK);
            SelectObject(dc, previousFont);
            DeleteObject(font);

            int   barHeight  = 2;
            RECT  barTrack   = { 0, clientRect.bottom - barHeight, clientRect.right, clientRect.bottom };
            HBRUSH track     = CreateSolidBrush(RGB(45, 45, 48));
            FillRect(dc, &barTrack, track);
            DeleteObject(track);

            int remainingWidth = (int)((LONGLONG)clientRect.right *
                (g_currentDurationMs - g_elapsedMs) / g_currentDurationMs);
            if (remainingWidth > 0) {
                RECT barFill = { 0, clientRect.bottom - barHeight, remainingWidth, clientRect.bottom };
                HBRUSH fill  = CreateSolidBrush(accent);
                FillRect(dc, &barFill, fill);
                DeleteObject(fill);
            }

            EndPaint(window, &paint);
            return 0;
        }

        case WM_TIMER: {
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ULONGLONG now = GetTickCount64();

            if (g_fadingOut) {
                int fadeElapsed = (int)(now - g_fadeStartMs);
                int newAlpha    = FADE_ALPHA_MAX - (FADE_ALPHA_MAX * fadeElapsed / FADE_DURATION_MS);
                if (newAlpha <= 0) {
                    KillTimer(window, UI_TIMER_ID);
                    DestroyWindow(window);
                    return 0;
                }
                SetLayeredWindowAttributes(window, 0, (BYTE)newAlpha, LWA_ALPHA);
                return 0;
            }

            if (g_alpha < FADE_ALPHA_MAX) {
                int fadeElapsed = (int)(now - g_fadeStartMs);
                int newAlpha    = FADE_ALPHA_MAX * fadeElapsed / FADE_DURATION_MS;
                g_alpha = newAlpha > FADE_ALPHA_MAX ? FADE_ALPHA_MAX : newAlpha;
                SetLayeredWindowAttributes(window, 0, (BYTE)g_alpha, LWA_ALPHA);
            }

            g_elapsedMs = (int)(now - g_stateStartMs);
            if (g_elapsedMs > g_currentDurationMs) g_elapsedMs = g_currentDurationMs;

            if (g_overlayState != OVERLAY_LAUNCHING) {
                if (g_elapsedMs >= g_currentDurationMs) {
                    g_fadingOut   = TRUE;
                    g_fadeStartMs = now;
                    return 0;
                }
                InvalidateRect(window, NULL, TRUE);
                return 0;
            }

            g_pollAccumMs += OVERLAY_TICK_MS;
            if (g_pollAccumMs >= POLL_INTERVAL_MS) {
                g_pollAccumMs = 0;

                if (GameWindowIsOpen()) {
                    SwitchOverlayState(window, OVERLAY_SUCCESS, L"Launched successfully", POPUP_DISPLAY_MS);
                    return 0;
                }

                if (g_smapiProcess && WaitForSingleObject(g_smapiProcess, 0) == WAIT_OBJECT_0) {
                    SwitchOverlayState(window, OVERLAY_CRASHED, L"SMAPI closed before the game started", POPUP_DISPLAY_MS);
                    return 0;
                }

                if (g_elapsedMs >= LAUNCH_TIMEOUT_MS) {
                    TerminateProcess(g_smapiProcess, 0);
                    CloseHandle(g_smapiProcess);
                    g_smapiProcess = NULL;
                    HANDLE visibleProcess;
                    DWORD  visibleProcessId;
                    LaunchSmapi(g_launchArgs, TRUE, &visibleProcess, &visibleProcessId);
                    if (visibleProcess) CloseHandle(visibleProcess);
                    g_fadingOut   = TRUE;
                    g_fadeStartMs = now;
                    return 0;
                }
            }

            InvalidateRect(window, NULL, TRUE);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static void ShowOverlay(HINSTANCE instance, OverlayState initialState, const wchar_t *message, int durationMs) {
    g_overlayState      = initialState;
    wcsncpy(g_overlayMessage, message, 255);
    g_overlayMessage[255] = L'\0';
    g_elapsedMs         = 0;
    g_currentDurationMs = durationMs;
    g_alpha             = 0;
    g_fadingOut         = FALSE;
    g_stateStartMs      = GetTickCount64();
    g_fadeStartMs       = GetTickCount64();
    g_pollAccumMs       = 0;

    WNDCLASSW windowClass   = {0};
    windowClass.style       = CS_DROPSHADOW;
    windowClass.lpfnWndProc = OverlayWindowProc;
    windowClass.hInstance   = instance;
    windowClass.lpszClassName = L"SmapiLauncherOverlay";
    RegisterClassW(&windowClass);

    int screenWidth  = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int windowX      = (screenWidth - OVERLAY_WIDTH_PX) / 2;
    int windowY      = screenHeight * TOP_OFFSET_PERCENT / 100;

    g_overlayWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        L"SmapiLauncherOverlay", L"", WS_POPUP,
        windowX, windowY, OVERLAY_WIDTH_PX, OVERLAY_HEIGHT_PX,
        NULL, NULL, instance, NULL
    );

    HRGN rgn = CreateRoundRectRgn(0, 0, OVERLAY_WIDTH_PX + 1, OVERLAY_HEIGHT_PX + 1,
                                   CORNER_RADIUS_PX, CORNER_RADIUS_PX);
    SetWindowRgn(g_overlayWindow, rgn, FALSE);

    SetLayeredWindowAttributes(g_overlayWindow, 0, 0, LWA_ALPHA);
    ShowWindow(g_overlayWindow, SW_SHOWNOACTIVATE);
    SetTimer(g_overlayWindow, UI_TIMER_ID, OVERLAY_TICK_MS, NULL);

    MSG message_;
    while (GetMessageW(&message_, NULL, 0, 0)) {
        TranslateMessage(&message_);
        DispatchMessageW(&message_);
    }
}

static BOOL FolderContainsDll(const char *folderPath) {
    char searchPattern[MAX_PATH];
    snprintf(searchPattern, sizeof(searchPattern), "%s\\*.dll", folderPath);
    WIN32_FIND_DATAA findData;
    HANDLE findHandle = FindFirstFileA(searchPattern, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) return FALSE;
    FindClose(findHandle);
    return TRUE;
}

static int CountModsInFolder(const char *modsFolderPath) {
    char searchPattern[MAX_PATH];
    snprintf(searchPattern, sizeof(searchPattern), "%s\\*", modsFolderPath);
    WIN32_FIND_DATAA findData;
    HANDLE findHandle = FindFirstFileA(searchPattern, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) return 0;

    int modCount = 0;
    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0) continue;

        char subFolderPath[MAX_PATH];
        snprintf(subFolderPath, sizeof(subFolderPath), "%s\\%s", modsFolderPath, findData.cFileName);
        if (FolderContainsDll(subFolderPath)) modCount++;
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);
    return modCount;
}

static int CountInstalledMods(void) {
    char modsFolderPath[MAX_PATH];

    GetPathNextToLauncher(modsFolderPath, sizeof(modsFolderPath), "Mods");
    if (GetFileAttributesA(modsFolderPath) != INVALID_FILE_ATTRIBUTES) {
        return CountModsInFolder(modsFolderPath);
    }

    GetPathNextToLauncher(modsFolderPath, sizeof(modsFolderPath), "mods");
    if (GetFileAttributesA(modsFolderPath) != INVALID_FILE_ATTRIBUTES) {
        return CountModsInFolder(modsFolderPath);
    }

    return 0;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR commandLineArgs, int showCommand) {
    GetPathNextToLauncher(g_launcherDir, sizeof(g_launcherDir), "");
    g_launcherDir[strlen(g_launcherDir) - 1] = '\0';

    char gameExePath[MAX_PATH];
    GetPathNextToLauncher(gameExePath, sizeof(gameExePath), "Stardew Valley.exe");
    if (GetFileAttributesA(gameExePath) == INVALID_FILE_ATTRIBUTES) {
        ShowOverlay(instance, OVERLAY_WARNING, L"Wrong folder: Stardew Valley.exe not found here", POPUP_DISPLAY_MS);
        return 1;
    }

    GetPathNextToLauncher(g_smapiExePath, sizeof(g_smapiExePath), "StardewModdingAPI.exe");
    if (GetFileAttributesA(g_smapiExePath) == INVALID_FILE_ATTRIBUTES) {
        ShowOverlay(instance, OVERLAY_WARNING, L"SMAPI is not installed", POPUP_DISPLAY_MS);
        return 1;
    }

    if (IsProcessRunning(L"StardewModdingAPI.exe") || IsProcessRunning(L"Stardew Valley.exe")) {
        ShowOverlay(instance, OVERLAY_WARNING, L"SMAPI or the game is already running", POPUP_DISPLAY_MS);
        return 1;
    }

    int modCount = CountInstalledMods();
    wchar_t launchingMessage[64];
    swprintf(launchingMessage, 64, L"Launching SMAPI with %d mods", modCount);

    strncpy(g_launchArgs, commandLineArgs, sizeof(g_launchArgs) - 1);

    if (!LaunchSmapi(commandLineArgs, FALSE, &g_smapiProcess, &g_smapiProcessId)) {
        return 1;
    }

    ShowOverlay(instance, OVERLAY_LAUNCHING, launchingMessage, LAUNCH_TIMEOUT_MS);

    if (g_smapiProcess) CloseHandle(g_smapiProcess);
    return 0;
}