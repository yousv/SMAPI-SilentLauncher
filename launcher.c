#include <windows.h>
#include <tlhelp32.h>
#include <string.h>
#include <stdio.h>
#include <commctrl.h>

#define UI_TIMER_ID          1
#define OVERLAY_TICK_MS      16
#define POLL_INTERVAL_MS     250
#define FADE_DURATION_MS     350
#define LAUNCH_TIMEOUT_MS    60000
#define POPUP_DISPLAY_MS     5000
#define OVERLAY_WIDTH_PX     280
#define OVERLAY_HEIGHT_PX    80
#define CORNER_RADIUS_PX     10
#define FADE_ALPHA_MAX       215
#define TOP_OFFSET_PERCENT   10
#define PROGRESS_BAR_HEIGHT_PX 4

typedef enum {
    OVERLAY_LAUNCHING,
    OVERLAY_SUCCESS,
    OVERLAY_WARNING,
    OVERLAY_CRASHED,
    OVERLAY_TIMED_OUT
} OverlayState;

static HWND        g_overlayWindow;
static OverlayState g_overlayState        = OVERLAY_LAUNCHING;
static wchar_t     g_overlayMessage[256]  = L"Launching SMAPI";
static int         g_elapsedMs            = 0;
static int         g_currentDurationMs    = LAUNCH_TIMEOUT_MS;
static int         g_timeoutMs            = LAUNCH_TIMEOUT_MS;
static HANDLE      g_smapiProcess         = NULL;
static DWORD       g_smapiProcessId       = 0;
static char        g_launcherDir[MAX_PATH];
static char        g_smapiExePath[MAX_PATH];
static char        g_launchArgs[32768]    = "";
static char        g_modsPath[MAX_PATH]   = "";
static int         g_alpha                = 0;
static BOOL        g_fadingOut            = FALSE;
static ULONGLONG   g_stateStartMs         = 0;
static ULONGLONG   g_fadeStartMs          = 0;
static ULONGLONG   g_pollAccumMs          = 0;
static HFONT       g_messageFont          = NULL;

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

static BOOL DirHasDllRecursive(const char *dirPath) {
    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*", dirPath);

    WIN32_FIND_DATAA findData;
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) return FALSE;

    BOOL found = FALSE;
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0) continue;
            char subDirPath[MAX_PATH];
            snprintf(subDirPath, sizeof(subDirPath), "%s\\%s", dirPath, findData.cFileName);
            if (DirHasDllRecursive(subDirPath)) { found = TRUE; break; }
        } else {
            const char *extension = strrchr(findData.cFileName, '.');
            if (extension && _stricmp(extension, ".dll") == 0) { found = TRUE; break; }
        }
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);
    return found;
}

static void CountMods(const char *modsDir, int *outModCount, int *outContentPackCount) {
    *outModCount = 0;
    *outContentPackCount = 0;

    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*", modsDir);

    WIN32_FIND_DATAA findData;
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) return;

    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0) continue;

        if (strstr(findData.cFileName, "[CP]")) {
            (*outContentPackCount)++;
            continue;
        }

        char modPath[MAX_PATH];
        snprintf(modPath, sizeof(modPath), "%s\\%s", modsDir, findData.cFileName);
        if (DirHasDllRecursive(modPath)) {
            (*outModCount)++;
        }
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);
}

static void SwitchOverlayState(HWND window, OverlayState newState, const wchar_t *message, int durationMs) {
    g_overlayState = newState;
    wcsncpy(g_overlayMessage, message, 255);
    g_overlayMessage[255] = L'\0';
    g_elapsedMs = 0;
    g_currentDurationMs = durationMs;
    g_stateStartMs = GetTickCount64();
    g_pollAccumMs = 0;
    InvalidateRect(window, NULL, TRUE);
}

static COLORREF GetAccentColor(OverlayState state) {
    if (state == OVERLAY_SUCCESS)   return RGB(120, 220, 140);
    if (state == OVERLAY_WARNING)   return RGB(235, 200, 80);
    if (state != OVERLAY_LAUNCHING) return RGB(235, 120, 110);
    return RGB(110, 150, 220);
}

static LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            RECT clientRect;
            GetClientRect(window, &clientRect);
            int width = clientRect.right;
            int height = clientRect.bottom;

            HDC memDC = CreateCompatibleDC(dc);
            HBITMAP memBitmap = CreateCompatibleBitmap(dc, width, height);
            HGDIOBJ previousBitmap = SelectObject(memDC, memBitmap);

            HBRUSH backgroundBrush = CreateSolidBrush(RGB(32, 32, 34));
            FillRect(memDC, &clientRect, backgroundBrush);
            DeleteObject(backgroundBrush);

            COLORREF accentColor = GetAccentColor(g_overlayState);

            RECT textRect = { 5, 5, width - 5, height - 28 };
            SetTextColor(memDC, accentColor);
            SetBkMode(memDC, TRANSPARENT);

            HFONT previousFont = (HFONT)SelectObject(memDC, g_messageFont);
            DrawTextW(memDC, g_overlayMessage, -1, &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_WORDBREAK);

            if (g_overlayState == OVERLAY_LAUNCHING) {
                int remainingSeconds = (g_currentDurationMs - g_elapsedMs + 999) / 1000;
                if (remainingSeconds < 0) remainingSeconds = 0;

                wchar_t timerText[16];
                swprintf(timerText, 16, L"%ds", remainingSeconds);

                RECT timerRect = { 5, height - 25, width - 5, height - 7 };
                SetTextColor(memDC, RGB(160, 160, 165));
                DrawTextW(memDC, timerText, -1, &timerRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            SelectObject(memDC, previousFont);

            RECT barTrackRect = { 0, height - PROGRESS_BAR_HEIGHT_PX, width, height };
            HBRUSH trackBrush = CreateSolidBrush(RGB(45, 45, 48));
            FillRect(memDC, &barTrackRect, trackBrush);
            DeleteObject(trackBrush);

            int remainingWidth = (int)((LONGLONG)width *
                (g_currentDurationMs - g_elapsedMs) / g_currentDurationMs);
            if (remainingWidth > 0) {
                RECT barFillRect = { 0, height - PROGRESS_BAR_HEIGHT_PX, remainingWidth, height };
                HBRUSH fillBrush = CreateSolidBrush(accentColor);
                FillRect(memDC, &barFillRect, fillBrush);
                DeleteObject(fillBrush);
            }

            BitBlt(dc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, previousBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_TIMER: {
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ULONGLONG now = GetTickCount64();

            if (g_fadingOut) {
                int fadeElapsed = (int)(now - g_fadeStartMs);
                int newAlpha = FADE_ALPHA_MAX - (FADE_ALPHA_MAX * fadeElapsed / FADE_DURATION_MS);
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
                int newAlpha = FADE_ALPHA_MAX * fadeElapsed / FADE_DURATION_MS;
                g_alpha = newAlpha > FADE_ALPHA_MAX ? FADE_ALPHA_MAX : newAlpha;
                SetLayeredWindowAttributes(window, 0, (BYTE)g_alpha, LWA_ALPHA);
            }

            g_elapsedMs = (int)(now - g_stateStartMs);
            if (g_elapsedMs > g_currentDurationMs) g_elapsedMs = g_currentDurationMs;

            if (g_overlayState != OVERLAY_LAUNCHING) {
                if (g_elapsedMs >= g_currentDurationMs) {
                    g_fadingOut = TRUE;
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

                if (g_elapsedMs >= g_timeoutMs) {
                    if (g_smapiProcess) {
                        TerminateProcess(g_smapiProcess, 0);
                        CloseHandle(g_smapiProcess);
                        g_smapiProcess = NULL;
                    }
                    SwitchOverlayState(window, OVERLAY_TIMED_OUT, L"Launch timed out", POPUP_DISPLAY_MS);
                    return 0;
                }

                InvalidateRect(window, NULL, TRUE);
            }
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursor(NULL, IDC_NO));
                return TRUE;
            }
            return DefWindowProcW(window, message, wParam, lParam);

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_RBUTTONDOWN:
            SwitchOverlayState(window, OVERLAY_CRASHED, L"Launch aborted", POPUP_DISPLAY_MS);
            if (g_smapiProcess) {
                TerminateProcess(g_smapiProcess, 0);
                CloseHandle(g_smapiProcess);
                g_smapiProcess = NULL;
            }
            return 0;

        case WM_DESTROY:
            if (g_messageFont) DeleteObject(g_messageFont);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static void ShowOverlay(HINSTANCE instance, OverlayState initialState, const wchar_t *message, int durationMs) {
    g_overlayState = initialState;
    wcsncpy(g_overlayMessage, message, 255);
    g_overlayMessage[255] = L'\0';
    g_elapsedMs = 0;
    g_currentDurationMs = durationMs;
    g_alpha = 0;
    g_fadingOut = FALSE;
    g_stateStartMs = GetTickCount64();
    g_fadeStartMs = GetTickCount64();
    g_pollAccumMs = 0;

    WNDCLASSW windowClass     = {0};
    windowClass.style         = CS_DROPSHADOW;
    windowClass.hInstance     = instance;
    windowClass.lpfnWndProc   = OverlayWindowProc;
    windowClass.lpszClassName = L"SmapiLauncherOverlay";
    RegisterClassW(&windowClass);

    int screenWidth  = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int windowX = (screenWidth - OVERLAY_WIDTH_PX) / 2;
    int windowY = screenHeight * TOP_OFFSET_PERCENT / 100;

    g_overlayWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        L"SmapiLauncherOverlay", L"", WS_POPUP,
        windowX, windowY, OVERLAY_WIDTH_PX, OVERLAY_HEIGHT_PX,
        NULL, NULL, instance, NULL
    );

    HRGN roundedRegion = CreateRoundRectRgn(0, 0, OVERLAY_WIDTH_PX + 1, OVERLAY_HEIGHT_PX + 1,
                                             CORNER_RADIUS_PX, CORNER_RADIUS_PX);
    SetWindowRgn(g_overlayWindow, roundedRegion, FALSE);

    g_messageFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    SetLayeredWindowAttributes(g_overlayWindow, 0, 0, LWA_ALPHA);
    ShowWindow(g_overlayWindow, SW_SHOWNOACTIVATE);
    SetTimer(g_overlayWindow, UI_TIMER_ID, OVERLAY_TICK_MS, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void ExtractModsPath(LPSTR cmdLine) {
    g_modsPath[0] = '\0';
    char *modsFlag = strstr(cmdLine, "--mods-path");
    if (!modsFlag) modsFlag = strstr(cmdLine, "-mods-path");
    if (!modsFlag) return;

    char *valueStart = strchr(modsFlag, ' ') ? strchr(modsFlag, ' ') + 1 : NULL;
    if (!valueStart) return;
    while (*valueStart == ' ' || *valueStart == '\t') valueStart++;
    if (!*valueStart) return;

    int length = 0;
    if (*valueStart == '"') {
        valueStart++;
        while (*valueStart && *valueStart != '"' && length < MAX_PATH - 1) g_modsPath[length++] = *valueStart++;
    } else {
        while (*valueStart && *valueStart != ' ' && *valueStart != '\t' && length < MAX_PATH - 1) g_modsPath[length++] = *valueStart++;
    }
    g_modsPath[length] = '\0';
}

static int ParseTimeoutFromArgs(LPSTR cmdLine, char *cleanOut, size_t cleanSize) {
    int timeoutSec = 60;
    int outPos = 0;
    int i = 0;

    while (cmdLine[i]) {
        while (cmdLine[i] == ' ' || cmdLine[i] == '\t') {
            if (outPos < (int)cleanSize - 1) cleanOut[outPos++] = cmdLine[i++];
        }
        if (!cmdLine[i]) break;

        if (cmdLine[i] == '-' || cmdLine[i] == '/') {
            const char *flag = cmdLine + i;
            if (flag[0] == '-' && flag[1] == '-') flag += 2;
            else flag += 1;

            if (strncmp(flag, "timeout", 7) == 0) {
                flag += 7;
                if (*flag == '=' || *flag == ':') flag++;
                while (*flag == ' ') flag++;

                int value = 0;
                while (*flag >= '0' && *flag <= '9') {
                    value = value * 10 + (*flag - '0');
                    flag++;
                }
                if (value > 0) timeoutSec = value;
                i = (int)(flag - cmdLine);
                continue;
            }
        }

        if (cmdLine[i] == '"') {
            if (outPos < (int)cleanSize - 1) cleanOut[outPos++] = cmdLine[i++];
            while (cmdLine[i] && cmdLine[i] != '"') {
                if (outPos < (int)cleanSize - 1) cleanOut[outPos++] = cmdLine[i++];
            }
            if (cmdLine[i]) {
                if (outPos < (int)cleanSize - 1) cleanOut[outPos++] = cmdLine[i++];
            }
        } else {
            while (cmdLine[i] && cmdLine[i] != ' ' && cmdLine[i] != '\t') {
                if (outPos < (int)cleanSize - 1) cleanOut[outPos++] = cmdLine[i++];
            }
        }
    }

    cleanOut[outPos] = '\0';
    return timeoutSec * 1000;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR commandLineArgs, int showCommand) {
    (void)previousInstance;
    (void)showCommand;

    INITCOMMONCONTROLSEX commonControls = { sizeof(commonControls), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&commonControls);

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

    ExtractModsPath(commandLineArgs);
    g_timeoutMs = ParseTimeoutFromArgs(commandLineArgs, g_launchArgs, sizeof(g_launchArgs));

    char fullModsPath[MAX_PATH];
    if (g_modsPath[0]) {
        if (strchr(g_modsPath, '\\') || strchr(g_modsPath, '/') || g_modsPath[1] == ':') {
            strncpy(fullModsPath, g_modsPath, MAX_PATH - 1);
            fullModsPath[MAX_PATH - 1] = '\0';
        } else {
            snprintf(fullModsPath, MAX_PATH - 1, "%s\\%s", g_launcherDir, g_modsPath);
        }
    } else {
        snprintf(fullModsPath, MAX_PATH - 1, "%s\\Mods", g_launcherDir);
    }

    int modCount = 0, contentPackCount = 0;
    CountMods(fullModsPath, &modCount, &contentPackCount);

    char commandLine[32768];
    snprintf(commandLine, sizeof(commandLine), "\"%s\" %s", g_smapiExePath, g_launchArgs);

    STARTUPINFOA startupInfo;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION processInfo;
    ZeroMemory(&processInfo, sizeof(processInfo));

    if (!CreateProcessA(g_smapiExePath, commandLine, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, g_launcherDir, &startupInfo, &processInfo)) {
        ShowOverlay(instance, OVERLAY_WARNING, L"Failed to launch SMAPI", POPUP_DISPLAY_MS);
        return 1;
    }
    CloseHandle(processInfo.hThread);
    g_smapiProcess = processInfo.hProcess;
    g_smapiProcessId = processInfo.dwProcessId;

    wchar_t launchMessage[256];
    if (modCount > 0 || contentPackCount > 0) {
        if (contentPackCount > 0)
            swprintf(launchMessage, 256, L"Launching SMAPI with %d mods, %d content packs", modCount, contentPackCount);
        else
            swprintf(launchMessage, 256, L"Launching SMAPI with %d mods", modCount);
    } else {
        swprintf(launchMessage, 256, L"Launching SMAPI...");
    }

    ShowOverlay(instance, OVERLAY_LAUNCHING, launchMessage, g_timeoutMs);

    if (g_smapiProcess) CloseHandle(g_smapiProcess);
    return 0;
}
