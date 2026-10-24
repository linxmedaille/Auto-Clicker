# Autoclicker

A small, precise autoclicker for Windows 10 and 11. One portable `autoclicker.exe`
(about 535 KB): no installer, no extra DLLs, nothing to set up. Download it, run it, press **F6**.

- **Accurate timing.** Clicks are scheduled against a high-resolution clock, so the rate
  doesn't drift, from one click every few hours down to 1 ms.
- **Can't click itself.** While the cursor is over the Autoclicker window it waits, so
  pressing Start never clicks its own Stop button.
- **Out of the way.** Close the window and it keeps running in the system tray; the hotkey
  still works from any app.
- **Your hotkey.** F6 by default, or any key or combination you choose.
- **Sharp on any display.** Renders at your screen's real resolution at every Windows
  scaling level, and follows your light or dark theme.

---

## Getting started

1. Run `autoclicker.exe`.
2. Set how fast to click and which button to use.
3. Point the mouse at what you want clicked and press **F6** (or click **Start**, then move
   the cursor off the window).
4. Press **F6** or **Esc** to stop.

### First launch: the SmartScreen warning

The exe isn't code-signed, so Windows SmartScreen may say it "protected your PC" the first
time. Click **More info → Run anyway**. Some antivirus tools also flag autoclickers in general;
that's about what the program does (sending mouse input), not a sign of anything harmful.

---

## Options

| Option | What it does |
| --- | --- |
| **Interval** | Time between clicks, in **ms** or **s** (default 100 ms, i.e. about 10 clicks per second). Switching units keeps the same duration, so 100 ms becomes 0.1 s. Below 10 ms you'll see a warning: many apps can't keep up that fast. |
| **Button** | Left, Right or Middle mouse button. |
| **Click** | Single or Double click. |
| **Repeat** | **Until stopped**, or a fixed **Count** of clicks, after which it stops by itself. |
| **Position** | **Cursor** clicks wherever the mouse is. **Point** clicks one fixed spot: press **Pick**, then left-click anywhere on screen to choose it. That click is only recorded, never delivered to the window underneath. Right-click or Esc cancels. |
| **Hotkey** | The start/stop key. Click the key cap, then press a new key or combination (Esc cancels). |

The large number shows how many clicks have been sent, and the status in the top-right
corner says whether it's **Idle**, **Running** or **Waiting** (the cursor is over the
Autoclicker itself).

### Keyboard shortcuts

| Key | Action |
| --- | --- |
| **F6** (or your hotkey) | Start or stop, from any app. Also cancels picking a point. |
| **Esc** | Stop immediately. Only taken over while clicking or picking, so Esc keeps working normally in other apps the rest of the time. |
| **Tab** / **Shift+Tab** | Move between controls. |
| **Arrow keys** | Change the selected option; in a number field, step the value up or down (hold **Shift** for ×10). |
| **Enter** | Start, from the interval or count field. |

### Choosing a hotkey

Function keys (F1–F24), Pause, Scroll Lock, Insert, Home, End, Page Up and Page Down can be
used on their own. Letters, numbers, Space, Enter, arrows and other typing keys need
**Ctrl**, **Alt** or **Shift** as well, so normal typing elsewhere can't start the clicker.
Esc can't be used, because it's reserved for stopping. If another program already owns a
shortcut, you'll be told and your current hotkey stays in place.

---

## Running in the tray

Closing the window (the X button or Alt+F4) **hides it to the system tray** instead of
quitting. Clicking and the hotkeys keep working while it's hidden. The first time, a
notification reminds you.

- **Left-click** the tray icon to bring the window back.
- **Right-click** it for **Show Autoclicker**, **Start / Stop clicking** and **Quit**.
- Hover over it to see whether it's idle, running or waiting.
- Opening `autoclicker.exe` again also brings the existing window back; only one copy runs at a time.

To exit completely, use **Quit** from the tray menu.

---

## Good to know

- **Administrator windows.** Windows doesn't let normal programs send input to programs running
  as administrator. To click in one of those, run Autoclicker as administrator too.
- **Games and online services.** Some games and websites forbid automated input or detect it
  with anti-cheat software. Check the rules of whatever you use it with.
- **Animations.** If Windows **Animation effects** is turned off (Settings → Accessibility →
  Visual effects), Autoclicker switches to calmer, still visuals. Contrast themes and
  transparency settings are respected too.
- **Requirements.** Windows 10 or 11 with the Microsoft Edge WebView2 Runtime, which comes
  with Windows 11 and almost every up-to-date Windows 10 PC. If it's missing, the app tells
  you and offers the download page: <https://go.microsoft.com/fwlink/p/?LinkId=2124703>.

### Privacy and settings

Autoclicker itself makes no network requests, collects nothing and sends nothing anywhere.
Its fonts and graphics are built into the exe.

The only thing it saves is your hotkey, in `%APPDATA%\Autoclicker\settings.ini`.

### Uninstalling

Quit it from the tray, delete `autoclicker.exe`, and optionally delete the
`%APPDATA%\Autoclicker` folder.

---

## For developers

Written in C11 on Windows APIs, with the interface as a single embedded HTML/CSS/JS page
shown through the system WebView2 runtime via the [webview](https://github.com/webview/webview)
library (0.12.0, linked statically with its built-in WebView2 loader).

### Building

Requires CMake 3.21+. The first configure needs an internet connection: CMake downloads
webview 0.12.0 and the WebView2 SDK headers. All UI assets are already in `vendor/`.

**MSVC** (Visual Studio 2022 or Build Tools with *Desktop development with C++*). From an
**x64 Native Tools Command Prompt for VS 2022** in this folder:

```bat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Or with the Visual Studio generator:

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output is `build\autoclicker.exe` (`build\Release\autoclicker.exe` with the VS generator).
It links the static CRT (`/MT`), so no Visual C++ redistributable is needed.

**MinGW-w64** (for example MSYS2 UCRT64:
`pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja`):

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Linked with `-mwindows -static -static-libgcc -static-libstdc++`, so no MinGW DLLs are needed.
The MSVC build is the tested one; the MinGW configuration hasn't been verified yet.

### How it works

| File | Role |
| --- | --- |
| `main.c` | Window setup, the JS↔C bridge (every value from the page is validated in C), global hotkeys, the hotkey setting, point picking (a low-level mouse hook installed only while picking), tray icon and close-to-tray, single-instance handling. |
| `clicker.c` / `clicker.h` | The clicking thread: `SendInput`, absolute `QueryPerformanceCounter` deadlines so the interval never drifts, `timeBeginPeriod(1)` only while running, and the "don't click our own window" check. |
| `ui.html` | The whole interface: HTML, CSS and JavaScript, no framework. |
| `cmake/embed.cmake` | Inlines `vendor/fonts.css` and `vendor/gsap.min.js` into `ui.html` and turns the result into a C byte array (`ui_html.h`) at build time. |
| `app.rc`, `app.manifest`, `app.ico` | Icon, version info, and the PerMonitorV2 DPI-awareness manifest. |
| `tools/make-icon.ps1`, `tools/make-fonts-css.sh` | Regenerate `app.ico` and `vendor/fonts.css`. |

**Threading.** Clicking runs on its own thread with an atomic stop flag plus a stop event, so a
stop takes effect instantly and the UI thread never blocks. The worker reaches the page only
through `webview_dispatch`, with counter updates throttled to about 30 per second.

**Input safety.** The page sends flat argument lists; `main.c` parses them with a small strict
parser and rejects anything outside the allowed values (interval 1 ms to 24 h, count 1 to
10⁹, known button and mode names, valid hotkey combinations). All formatting uses `snprintf`.

### Third-party assets

Embedded in the exe and stored in `vendor/`; sources and licences are listed in
`vendor/LICENSES.md`:

- [GSAP](https://gsap.com) 3.13.0, GSAP Standard "No Charge" License
- Instrument Serif and JetBrains Mono (via Fontsource), SIL Open Font License 1.1
- Solar Icons by 480 Design (via Iconify), CC BY 4.0
- [webview](https://github.com/webview/webview) 0.12.0, MIT (fetched at build time)
