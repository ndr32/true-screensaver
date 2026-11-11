// True Screensaver
// Double-click the tray icon to turn every screen pure black (OLED pixels off).
// Press any key or click any mouse button to come back.
//
// Plain C + Win32. While idle there are no hooks, timers or polling; the mouse and
// keyboard are only hooked while the screen is covered.

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>

#define APP_NAME      L"TrueScreensaver"
#define RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_ARG   L"--startup"
#define MAIN_CLASS    L"TrueScreensaverMain"
#define BLACK_CLASS   L"TrueScreensaverBlack"

#define WM_TRAY       (WM_APP + 1)
#define WM_RESTORE    (WM_APP + 3)
#define WM_UNHOOK     (WM_APP + 4)

#define TIMER_TOPMOST 1   // only runs while blacked out
#define TIMER_UNHOOK  2   // fallback for removing the input hooks
#define TIMER_FADE    3   // only runs during a fade
#define TIMER_RAISE   4   // one-shot: another window appeared over the cover
#define FADE_MS       250

#define FLYOUT_CLASS  L"TrueScreensaverFlyout"
#define FLYOUT_W      320 // logical pixels
#define FLYOUT_H      80

#define MAX_SCREENS   16

enum { CMD_BLACKOUT = 1, CMD_STARTUP, CMD_EXIT };

static HINSTANCE g_inst;
static HWND      g_main;
static HICON     g_icon;
static UINT      g_taskbarCreated;
static HHOOK     g_mouseHook, g_kbHook;           // only while covered
static HWINEVENTHOOK g_evForeground, g_evZOrder;  // only while covered
static HWND      g_black[MAX_SCREENS];
static int       g_blackCount;
static BOOL      g_active;        // screens are covered
static COLORREF  g_color;         // cover colour; black except when picked from the panel
static int       g_alpha;         // cover opacity, 0..255
static int       g_fadeDir;       // +1 fading in, -1 fading out, 0 not fading
static int       g_fadeFrom;
static DWORD     g_fadeStart;
static UINT      g_swallowedButtons; // mouse buttons whose press we swallowed (bit per button)
static DWORD     g_pendingKeyUp;  // key that ended the blackout; its key-up is eaten too
static HWND      g_flyout;        // panel with the big button; exists only while open
static HFONT     g_flyoutFont;
static int       g_flyoutHot = -1; // button under the pointer: 0 Black out, 1..4 colour, -1 none
static DWORD     g_flyoutClosedAt;

// ------------------------------------------------------------------ settings

static BOOL StartupEnabled(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ,
                        NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void ToggleStartup(void)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL))
        return;
    if (StartupEnabled()) {
        RegDeleteValueW(key, APP_NAME);
    } else {
        WCHAR cmd[MAX_PATH + 32];
        cmd[0] = L'"';
        DWORD n = GetModuleFileNameW(NULL, cmd + 1, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            lstrcatW(cmd, L"\" " STARTUP_ARG);
            RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE *)cmd,
                           (DWORD)(lstrlenW(cmd) + 1) * sizeof(WCHAR));
        }
    }
    RegCloseKey(key);
}

// ------------------------------------------------------------------ blackout

static void RemoveHooks(void);

static BOOL CALLBACK AddScreen(HMONITOR mon, HDC dc, LPRECT r, LPARAM lp)
{
    (void)mon; (void)dc; (void)lp;
    if (g_blackCount >= MAX_SCREENS) return FALSE;
    // Layered so the opacity can be animated.
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, BLACK_CLASS,
                             L"True Screensaver", WS_POPUP, r->left, r->top,
                             r->right - r->left, r->bottom - r->top,
                             NULL, NULL, g_inst, NULL);
    if (w) {
        // Tell the shell this is not a full-screen app. Otherwise Windows turns on
        // Do Not Disturb while the cover is up and holds notification banners back.
        SetPropW(w, L"NonRudeHWND", (HANDLE)1);
        SetLayeredWindowAttributes(w, 0, (BYTE)g_alpha, LWA_ALPHA);
        ShowWindow(w, SW_SHOW);
        g_black[g_blackCount++] = w;
    }
    return TRUE;
}

static void CreateBlackWindows(void)
{
    EnumDisplayMonitors(NULL, NULL, AddScreen, 0);
    if (g_blackCount) SetForegroundWindow(g_black[0]);
}

static void DestroyBlackWindows(void)
{
    for (int i = 0; i < g_blackCount; i++) {
        RemovePropW(g_black[i], L"NonRudeHWND");
        DestroyWindow(g_black[i]);
    }
    g_blackCount = 0;
}

// Fades are driven by a timer, not AnimateWindow: that call blocks the thread, and
// the mouse hook runs on this thread, so blocking would freeze the pointer.
static void StartFade(int dir)
{
    if (g_fadeDir == dir) return;
    g_fadeDir = dir;
    g_fadeFrom = g_alpha;
    g_fadeStart = GetTickCount();
    SetTimer(g_main, TIMER_FADE, 10, NULL);
}

static void FadeStep(void)
{
    int a = g_fadeFrom + g_fadeDir * (int)((GetTickCount() - g_fadeStart) * 255 / FADE_MS);
    BOOL done = a <= 0 || a >= 255;
    g_alpha = a < 0 ? 0 : a > 255 ? 255 : a;
    if (done) {
        int dir = g_fadeDir;
        g_fadeDir = 0;
        KillTimer(g_main, TIMER_FADE);
        if (dir < 0) { DestroyBlackWindows(); RemoveHooks(); return; }
    }
    for (int i = 0; i < g_blackCount; i++)
        SetLayeredWindowAttributes(g_black[i], 0, (BYTE)g_alpha, LWA_ALPHA);
}

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp);
static void CloseFlyout(void);

// The mouse and keyboard are hooked only while the screen is covered, to catch the
// click or key that ends it and keep it from reaching other apps.
static void InstallHooks(void)
{
    KillTimer(g_main, TIMER_UNHOOK);
    g_pendingKeyUp = 0;
    g_swallowedButtons = 0;
    if (!g_kbHook) g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, g_inst, 0);
    if (!g_mouseHook) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, g_inst, 0);
}

// Stays hooked until the cover is gone and the key or button that ended it has been
// released, so that release is swallowed too (a lone Win-key release would otherwise
// open the Start menu).
static void RemoveHooks(void)
{
    if (g_active || g_blackCount || g_pendingKeyUp || g_swallowedButtons) return;
    KillTimer(g_main, TIMER_UNHOOK);
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    g_kbHook = g_mouseHook = NULL;
}

static void KeepOnTop(void);

// Called when any other program shows a window, changes the z-order or takes focus
// while the cover is up. The first event raises the cover at once; further events in
// the same clock tick are collapsed into one raise 10 ms later, which caps the rate
// if another always-on-top program fights back.
static void CALLBACK WindowEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                     LONG idChild, DWORD thread, DWORD time)
{
    (void)hook; (void)idChild; (void)thread; (void)time;
    if (!g_active) return;
    if (event == EVENT_OBJECT_REORDER) {
        if (hwnd != GetDesktopWindow()) return;         // top-level z-order changes only
    } else if (event == EVENT_OBJECT_HIDE || idObject != OBJID_WINDOW ||
               GetAncestor(hwnd, GA_ROOT) != hwnd) {    // top-level windows only
        return;
    }
    static DWORD lastRaise;
    DWORD now = GetTickCount();
    if (now != lastRaise) {
        lastRaise = now;
        KeepOnTop();
    } else {
        SetTimer(g_main, TIMER_RAISE, 10, NULL);
    }
}

static void WatchOtherWindows(BOOL on)
{
    if (on && !g_evZOrder) {
        const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        g_evForeground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                         NULL, WindowEventProc, 0, 0, flags);
        g_evZOrder = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_REORDER,
                                     NULL, WindowEventProc, 0, 0, flags);
    } else if (!on) {
        if (g_evForeground) UnhookWinEvent(g_evForeground);
        if (g_evZOrder) UnhookWinEvent(g_evZOrder);
        g_evForeground = g_evZOrder = NULL;
        KillTimer(g_main, TIMER_RAISE);
    }
}

static void BlackOut(COLORREF color)
{
    if (g_blackCount) return;
    CloseFlyout();
    g_active = TRUE;
    g_color = color;
    g_alpha = 0;
    InstallHooks();
    CreateBlackWindows();
    StartFade(+1);
    WatchOtherWindows(TRUE);
    SetTimer(g_main, TIMER_TOPMOST, 1000, NULL);  // fallback for anything the events miss
}

static void Restore(void)
{
    g_active = FALSE;
    WatchOtherWindows(FALSE);
    KillTimer(g_main, TIMER_TOPMOST);
    // The windows are destroyed and the hooks removed when the fade ends. The timer is
    // the fallback if the key or button that ended the blackout is never seen released.
    if (g_blackCount) StartFade(-1);
    SetTimer(g_main, TIMER_UNHOOK, 2000, NULL);
}

// Puts the cover back above every other always-on-top window.
static void KeepOnTop(void)
{
    for (int i = 0; i < g_blackCount; i++)
        SetWindowPos(g_black[i], HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// ------------------------------------------------------------------ hooks

static UINT ButtonBit(WPARAM msg)
{
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: return 2;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: return 4;
    default:                                return 8;  // X buttons
    }
}

// While installed this runs for every mouse event on the system, so the common case
// (a move) exits first.
//
// A button-up is swallowed only if its button-down was. If the down reached Windows
// (e.g. the second click of a tray double-click, which then blacks out), the up must
// reach it too, or Windows believes the button is still held and starts a drag.
static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && wp != WM_MOUSEMOVE) {
        switch (wp) {
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
            if (g_active) {                     // any click ends the blackout
                g_active = FALSE;
                g_swallowedButtons |= ButtonBit(wp);
                PostMessageW(g_main, WM_RESTORE, 0, 0);
                return 1;
            }
            g_swallowedButtons &= ~ButtonBit(wp);  // this press went through; so must its release
            break;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            if (g_swallowedButtons & ButtonBit(wp)) {
                g_swallowedButtons &= ~ButtonBit(wp);
                PostMessageW(g_main, WM_UNHOOK, 0, 0);
                return 1;
            }
            break;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        BOOL down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (g_active && down) {
            // The first key press ends the blackout and reaches no other app. Key
            // releases pass through: their presses happened before the blackout, so
            // Windows must see them released (same reasoning as the mouse hook).
            g_active = FALSE;
            g_pendingKeyUp = k->vkCode;
            PostMessageW(g_main, WM_RESTORE, 0, 0);
            return 1;
        }
        if (!down && g_pendingKeyUp && k->vkCode == g_pendingKeyUp) {
            g_pendingKeyUp = 0;
            PostMessageW(g_main, WM_UNHOOK, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

// ------------------------------------------------------------------ tray

static HICON MakeIcon(void)
{
    // 32x32 monitor glyph: grey bezel and stand (readable on light and dark taskbars), black screen.
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 32;
    bi.bmiHeader.biHeight = -32;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    DWORD *px;
    HBITMAP color = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!color) return LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);

    const DWORD grey = 0xFF969696, black = 0xFF000000;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            DWORD c = 0;
            if (x >= 1 && x <= 30 && y >= 3 && y <= 23)  c = grey;   // bezel
            if (x >= 4 && x <= 27 && y >= 6 && y <= 20)  c = black;  // screen
            if (x >= 14 && x <= 17 && y >= 24 && y <= 26) c = grey;  // neck
            if (x >= 9 && x <= 22 && y >= 27 && y <= 29)  c = grey;  // base
            px[y * 32 + x] = c;
        }
    }

    static const BYTE maskBits[32 * 4] = {0};
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, maskBits);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon ? icon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
}

static void TrayAdd(BOOL welcome)
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof nid;
    nid.hWnd = g_main;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    lstrcpynW(nid.szTip, L"True Screensaver: double-click to black out, click for more", ARRAYSIZE(nid.szTip));
    if (welcome) {
        nid.uFlags |= NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        lstrcpynW(nid.szInfoTitle, L"True Screensaver is running", ARRAYSIZE(nid.szInfoTitle));
        lstrcpynW(nid.szInfo, L"Double-click this icon to black out your screens. "
                              L"Press any key or click to come back.", ARRAYSIZE(nid.szInfo));
    }
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void TrayRemove(void)
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof nid;
    nid.hWnd = g_main;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowMenu(int x, int y)
{
    // Built on demand and destroyed right after, so nothing is kept around while idle.
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CMD_BLACKOUT, L"Black out now");
    SetMenuDefaultItem(menu, CMD_BLACKOUT, FALSE);
    AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), CMD_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"Exit");

    SetForegroundWindow(g_main);
    UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                    x, y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
    case CMD_BLACKOUT: BlackOut(RGB(0, 0, 0)); break;
    case CMD_STARTUP:  ToggleStartup(); break;
    case CMD_EXIT:     DestroyWindow(g_main); break;
    }
}

// ------------------------------------------------------------------ flyout
// Left-clicking the tray icon opens a small panel: a big "Black out" button with four
// small colour buttons to its left.

static void CloseFlyout(void)
{
    if (!g_flyout) return;
    HWND w = g_flyout;
    g_flyout = NULL;
    g_flyoutClosedAt = GetTickCount();
    DestroyWindow(w);
}

static void ShowFlyout(void)
{
    if (g_flyout) { CloseFlyout(); return; }
    if (g_blackCount) return;  // e.g. the second click of a double-click that blacked out
    // Clicking the tray icon while the panel is open closes it (focus loss) and then
    // reports a click; don't reopen on that same click.
    if (GetTickCount() - g_flyoutClosedAt < 250) return;

    RECT icon;
    NOTIFYICONIDENTIFIER id = {0};
    id.cbSize = sizeof id;
    id.hWnd = g_main;
    id.uID = 1;
    if (FAILED(Shell_NotifyIconGetRect(&id, &icon))) {
        POINT p;
        GetCursorPos(&p);
        SetRect(&icon, p.x, p.y, p.x, p.y);
    }

    // Created empty at the icon so it picks up that monitor's DPI, then sized.
    g_flyout = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, FLYOUT_CLASS, L"True Screensaver",
                               WS_POPUP, icon.left, icon.top, 0, 0, NULL, NULL, g_inst, NULL);
    if (!g_flyout) return;

    int dpi = (int)GetDpiForWindow(g_flyout);
    if (!dpi) dpi = 96;
    int w = MulDiv(FLYOUT_W, dpi, 96), h = MulDiv(FLYOUT_H, dpi, 96), gap = MulDiv(12, dpi, 96);
    g_flyoutFont = CreateFontW(-MulDiv(20, dpi, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                               0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    MONITORINFO mi = {0};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromRect(&icon, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT *scr = &mi.rcMonitor;

    int x = (icon.left + icon.right - w) / 2;
    int y = icon.top - h - gap;                     // above the icon
    if (y < scr->top + gap) y = icon.bottom + gap;  // taskbar at the top: below it
    if (x > scr->right - w - gap) x = scr->right - w - gap;
    if (x < scr->left + gap) x = scr->left + gap;
    if (y > scr->bottom - h - gap) y = scr->bottom - h - gap;

    int round = 2; // DWMWCP_ROUND; rounded corners on Windows 11, ignored on 10
    DwmSetWindowAttribute(g_flyout, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);

    SetWindowPos(g_flyout, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(g_flyout);  // so clicking anywhere else closes it
}

static const COLORREF kSwatch[4] = {
    RGB(0, 255, 0), RGB(255, 0, 0), RGB(0, 0, 255), RGB(255, 255, 255)
};

// A 2x2 grid of small colour swatches on the left, the Black out button filling the
// rest. Everything is derived from the panel height so it scales with DPI.
typedef struct { int cell, pad, split; } FlyoutLayout;

static FlyoutLayout GetFlyoutLayout(HWND w, RECT *rc)
{
    FlyoutLayout l;
    GetClientRect(w, rc);
    l.cell = rc->bottom * 2 / 5;             // 32 at 80 high
    l.pad = (rc->bottom - 2 * l.cell) / 2;   // 8
    l.split = 2 * l.pad + 2 * l.cell;        // 80: where the Black out button starts
    return l;
}

// 0 = Black out, 1..4 = swatch, -1 = nothing.
static int FlyoutHit(HWND w, int x, int y)
{
    RECT rc;
    FlyoutLayout l = GetFlyoutLayout(w, &rc);
    if (x >= l.split) return 0;
    if (x < l.pad || y < l.pad) return -1;
    int col = (x - l.pad) / l.cell, row = (y - l.pad) / l.cell;
    return col < 2 && row < 2 ? 1 + row * 2 + col : -1;
}

static void FillSolid(HDC dc, const RECT *rc, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, rc, brush);
    DeleteObject(brush);
}

static void PaintFlyout(HWND w)
{
    static const WCHAR label[] = L"Black out";
    PAINTSTRUCT ps;
    HDC screen = BeginPaint(w, &ps);
    RECT rc;
    FlyoutLayout l = GetFlyoutLayout(w, &rc);

    // Draw off-screen so hover changes don't flicker.
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    FillSolid(dc, &rc, RGB(32, 32, 32));
    if (g_flyoutHot == 0) {
        RECT button = { l.split, 0, rc.right, rc.bottom };
        FillSolid(dc, &button, RGB(60, 60, 60));
    }

    // Swatches; the one under the pointer is drawn larger.
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    for (int i = 0; i < 4; i++) {
        int inset = g_flyoutHot == i + 1 ? l.cell / 16 : l.cell / 6;
        int x = l.pad + (i % 2) * l.cell, y = l.pad + (i / 2) * l.cell;
        HBRUSH brush = CreateSolidBrush(kSwatch[i]);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        RoundRect(dc, x + inset, y + inset, x + l.cell - inset + 1, y + l.cell - inset + 1,
                  l.cell / 4, l.cell / 4);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
    }
    SelectObject(dc, oldPen);

    // Icon and label, centered together in the button area.
    HGDIOBJ oldFont = SelectObject(dc, g_flyoutFont);
    SIZE text;
    GetTextExtentPoint32W(dc, label, ARRAYSIZE(label) - 1, &text);
    int icon = l.cell, gap = icon / 3;
    int x = l.split + (rc.right - l.split - (icon + gap + text.cx)) / 2;
    DrawIconEx(dc, x, (rc.bottom - icon) / 2, g_icon, icon, icon, 0, NULL, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, x + icon + gap, (rc.bottom - text.cy) / 2, label, ARRAYSIZE(label) - 1);
    SelectObject(dc, oldFont);

    BitBlt(screen, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(w, &ps);
}

static LRESULT CALLBACK FlyoutProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && w == g_flyout) CloseFlyout();
        return 0;
    case WM_MOUSEMOVE: {
        int hit = FlyoutHit(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hit != g_flyoutHot) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, w, 0 };
            TrackMouseEvent(&t);
            g_flyoutHot = hit;
            InvalidateRect(w, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_flyoutHot = -1;
        InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int hit = FlyoutHit(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hit == 0) BlackOut(RGB(0, 0, 0));       // BlackOut closes the panel
        else if (hit > 0) BlackOut(kSwatch[hit - 1]);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) CloseFlyout();
        else if (wp == VK_RETURN || wp == VK_SPACE) BlackOut(RGB(0, 0, 0));
        return 0;
    case WM_ERASEBKGND:
        return 1;    // WM_PAINT fills everything
    case WM_PAINT:
        PaintFlyout(w);
        return 0;
    case WM_DESTROY:
        if (g_flyoutFont) { DeleteObject(g_flyoutFont); g_flyoutFont = NULL; }
        g_flyoutHot = -1;
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ------------------------------------------------------------------ windows

static LRESULT CALLBACK BlackProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_SETCURSOR:
        SetCursor(NULL);  // hide the mouse pointer
        return TRUE;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(w, &rc);
        HBRUSH brush = CreateSolidBrush(g_color);
        FillRect((HDC)wp, &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    // Backup paths in case Windows ever drops the hooks.
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        PostMessageW(g_main, WM_RESTORE, 0, 0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static LRESULT CALLBACK MainProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:   ShowMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); break;
        case NIN_SELECT:
        case NIN_KEYSELECT:    ShowFlyout(); break;
        // The first click of a double-click has already opened the panel; BlackOut closes it.
        case WM_LBUTTONDBLCLK: BlackOut(RGB(0, 0, 0)); break;
        }
        return 0;
    case WM_RESTORE:   Restore(); return 0;
    case WM_UNHOOK:    RemoveHooks(); return 0;
    case WM_TIMER:
        if (wp == TIMER_FADE) FadeStep();
        else if (wp == TIMER_RAISE) { KillTimer(w, TIMER_RAISE); KeepOnTop(); }
        else if (wp == TIMER_TOPMOST) KeepOnTop();
        else if (wp == TIMER_UNHOOK) {
            KillTimer(w, TIMER_UNHOOK);
            if (!g_active) { g_pendingKeyUp = 0; g_swallowedButtons = 0; RemoveHooks(); }
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (g_blackCount) { DestroyBlackWindows(); CreateBlackWindows(); }  // monitor added/removed
        return 0;
    case WM_DESTROY:
        CloseFlyout();
        g_active = FALSE;
        Restore();
        DestroyBlackWindows();  // exiting: don't wait for the fade
        g_pendingKeyUp = 0;
        g_swallowedButtons = 0;
        RemoveHooks();
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    if (m == g_taskbarCreated && m) {  // Explorer restarted: put the tray icon back
        TrayAdd(FALSE);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ------------------------------------------------------------------ startup

static BOOL HasStartupArg(void)
{
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL found = FALSE;
    for (int i = 1; argv && i < argc; i++)
        if (!lstrcmpiW(argv[i], STARTUP_ARG)) found = TRUE;
    LocalFree(argv);
    return found;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show)
{
    (void)prev; (void)cmdLine; (void)show;
    g_inst = inst;

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\TrueScreensaver.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc = {0};
    wc.hInstance = inst;
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassW(&wc);

    wc.lpfnWndProc = BlackProc;
    wc.lpszClassName = BLACK_CLASS;
    RegisterClassW(&wc);

    wc.lpfnWndProc = FlyoutProc;
    wc.lpszClassName = FLYOUT_CLASS;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    RegisterClassW(&wc);

    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    g_main = CreateWindowExW(0, MAIN_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_main) return 1;

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_icon = MakeIcon();
    TrayAdd(!HasStartupArg());

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyIcon(g_icon);
    if (mutex) CloseHandle(mutex);
    return 0;
}
