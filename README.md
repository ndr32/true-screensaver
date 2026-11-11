# True Screensaver

Windows tray app. Double-click its tray icon and every monitor goes black.
Any key or mouse click brings the screen back; moving the mouse does not.

## Build

```powershell
winget install zig.zig   # once
.\build.ps1
.\bin\TrueScreensaver.exe
```

Plain C, Win32 only. Windows 10 1703 or later.

## Controls

- Double-click the tray icon: black out.
- Left-click the tray icon: opens a small panel with a large "Black out" button and, to its
  left, four small colour buttons (green, red, blue, white) that fill the screens with that
  colour instead. Enter also blacks out; click elsewhere or press Esc to close the panel.
- Right-click the tray icon > Black out now: same.
- Any key or click while black: restore. That key or click is not passed to other apps.
- Tray menu > Start with Windows: adds or removes an `HKCU\...\Run` entry.

## How it works

- While idle the app does nothing: no hooks, timers or polling. It waits for tray messages.
- Blackout is one topmost black window per monitor with the pointer hidden.
- Low-level mouse and keyboard hooks are installed only while covered, to catch the click or
  key that ends it. They are removed once the cover is gone and that key or button is released.
- A button or key release is swallowed only if its press was. A press that Windows already saw
  (the second click of a tray double-click) must have its release seen too, or Windows thinks
  the button is still held.
- While covered, the app listens for other programs showing a window, changing the stacking
  order or taking focus, and puts the cover back on top at once. Measured: a popup from
  another always-on-top program is above the cover for 0-24 ms. A 1-second timer is the
  fallback.
- The cover fades in and out over 250 ms (`FADE_MS`). The fade is timer-driven so the hooks
  keep responding during it.
- The only stored setting is the optional `Run` entry.

## Limits

- Windows keeps some of its own surfaces in layers above every normal program: notification
  banners, the Start menu, the volume overlay, the lock screen and admin (UAC) prompts. The
  cover cannot go above those.
- Notification banners show over the cover on purpose. The cover windows carry the
  `NonRudeHWND` property so Windows does not treat them as a full-screen app; without it,
  Windows turns on Do Not Disturb while the cover is up.
