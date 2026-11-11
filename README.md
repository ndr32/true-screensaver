# True Screensaver

Windows tray app. Left-click the top-left corner of the taskbar and every monitor goes black.
Any key or mouse click brings the screen back; moving the mouse does not.

## Build

```powershell
winget install zig.zig   # once
.\build.ps1
.\bin\TrueScreensaver.exe
```

Plain C, Win32 only. Windows 10 1703 or later.

## Controls

- Left-click the taskbar's top-left corner: black out.
- Double-click the tray icon: black out.
- Left-click the tray icon: opens a small panel with a large "Black out" button and, to its
  left, four small colour buttons (green, red, blue, white) that fill the screens with that
  colour instead. Enter also blacks out; click elsewhere or press Esc to close the panel.
- Right-click the tray icon > Black out now: same.
- Any key or click while black: restore. That key or click is not passed to other apps.
- Tray menu > Hot-corner size: 16, 32 (default) or 48 px.
- Tray menu > Start with Windows: adds or removes an `HKCU\...\Run` entry.

## How it works

- A low-level mouse hook sees every click. A left click triggers only if the window under it
  is a taskbar (`Shell_TrayWnd` or `Shell_SecondaryTrayWnd`) and the point is inside the
  corner square. That click is swallowed.
- Blackout is one topmost black window per monitor with the pointer hidden.
- While covered, the app listens for other programs showing a window, changing the stacking
  order or taking focus, and puts the cover back on top at once. Measured: a popup from
  another always-on-top program is above the cover for 0-22 ms. A 1-second timer is the
  fallback.
- The cover fades in and out over 250 ms (`FADE_MS`). The fade is timer-driven so the mouse
  hook keeps responding during it.
- A keyboard hook is installed only while black.
- The process opts out of Windows power throttling so the hook is not delayed.
- Settings are in `HKCU\Software\TrueScreensaver`.

## Limits

- Windows keeps some of its own surfaces in layers above every normal program: notification
  banners, the Start menu, the volume overlay, the lock screen and admin (UAC) prompts. The
  cover cannot go above those.
- Notification banners show over the cover on purpose. The cover windows carry the
  `NonRudeHWND` property so Windows does not treat them as a full-screen app; without it,
  Windows turns on Do Not Disturb while the cover is up.
- The corner overlaps the Widgets button (centered taskbar) or the Start button
  (left-aligned taskbar). Clicks inside the square never reach them.
- With an auto-hiding taskbar, only the thin visible strip is clickable until the taskbar
  slides up.
