# M3 — Manual Test Checklist (X11)

Run these on a real X11 session before signing off on M3. Any X11 desktop
(XFCE, KDE/X11, GNOME/X11, Cinnamon, …) is acceptable.

## Prerequisites
- X11 session (`echo $XDG_SESSION_TYPE` → `x11`, or `loginctl show-session $XDG_SESSION_ID -p Type` → `Type=x11`)
- X server has the Composite extension (verify: `xdpyinfo | grep -i composite`)
- Build: `cmake --build build -j` from a clean checkout

## Tests

### 1. Enumeration on omission
```
./build/ShaderGlassLinux/shaderglass --capture x11-screen
```
- Expect: prints `no --source given; pick one with --source <id-or-name>:` followed by a list including `monitor:root` and any connected outputs / named windows. Exit code 2.

### 2. Capture the full desktop
```
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source monitor:root
```
- Expect: ShaderGlass window opens; rendered content matches the desktop. Move things on the desktop — rendered content follows. Esc / close exits cleanly.

### 3. Capture a specific output (multi-monitor)
```
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source monitor:<OUTPUT>
```
(e.g. `--source monitor:DP-1`. Use `xrandr | grep ' connected'` for output names.)
- Expect: only that output's contents render.

### 4a. Capture a specific window by xid
```
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source window:0x<XID>
```
(Use `wmctrl -l` or `xdotool search` for xids. xids change per launch.)
- Expect: just that window's contents render. Move the source window — rendered content follows.

### 4b. Capture a specific window by name substring
```
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source firefox
```
- Expect: matched window renders. Minimize the source — rendering continues from the last composited backing (XComposite redirection).

### 5. Ambiguous source error
Open two Firefox windows, then:
```
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source firefox
```
- Expect: `'firefox' matched more than one source:` followed by the matched list. Exit code 3.

### 6. No-match source error
```
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source bogus-name-12345
```
- Expect: `no source matched 'bogus-name-12345'; available:` followed by the full list. Exit code 4.

### 7. Resize the source
While running test 4b, resize the source window. Expect: ShaderGlass keeps rendering at the new size after a single dropped frame. No crash.

### 8. Smoke result

Sign off when all 7 above pass:
- Tester: ____________
- Date: ____________
- Distro / DE: ____________
- Driver: ____________

Notes / known-failure log:

| DE / WM | Source | Result | Notes |
|---|---|---|---|
| (fill in) | | | |
