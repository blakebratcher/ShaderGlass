# M2 — Manual Test Checklist (Plasma Wayland)

Run these on a real Plasma 6 Wayland session before signing off on M2.

## Prerequisites
- KDE Plasma 6 + Wayland session (verify with `loginctl show-session $XDG_SESSION_ID -p Type` → `Type=wayland`)
- `xdg-desktop-portal-kde` running: `systemctl --user status xdg-desktop-portal-kde`
- Build: `cmake --build build -j` from a clean checkout

## Tests

### 1. Portal handshake — debug mode
```
./build/ShaderScope/shaderscope --debug-portal
```
- Expect: KDE source-picker dialog appears.
- Pick any window or screen.
- Expect: program prints `[INFO] portal: handshake complete (...)` followed by exit 0.

### 2. Restore-token persistence
Run test 1 a second time:
```
./build/ShaderScope/shaderscope --debug-portal
```
- Expect: no picker. The `[INFO] portal: loaded restore token from disk` line is followed by an immediate handshake-complete with the same node id from the first run.
- Verify token file: `cat ~/.config/shaderscope/portal-token` is non-empty.

### 3. Capture window — full pipeline (DMA-BUF if available)
```
./build/ShaderScope/shaderscope --capture wayland-screen
```
- Expect: picker (or instant restore from test 2). Pick a single window.
- Expect: window opens. The captured app's contents should show inside the ShaderScope window, rendered through the passthrough shader.
- Move the source window — the rendered content should follow.
- Log: `[INFO] portal: DMA-BUF import enabled` (or fallback warning).

### 4. Capture monitor
Repeat test 3, picking a monitor instead of a window.
- Expect: full-monitor capture displayed in ShaderScope window.

### 5. CPU-only path
```
SHADERSCOPE_DISABLE_DMABUF=1 ./build/ShaderScope/shaderscope --capture wayland-screen
```
- Expect: identical visual output, with log line saying DMA-BUF is disabled.

### 6. Cancel the picker
Run test 3, but cancel the portal dialog.
- Expect: clean exit with `[ERROR] fatal: portal: SelectSources cancelled (code 1)` or similar; no crash.

### 7. Driver / modifier known-failure log
Note any drivers / modifier combinations where DMA-BUF import fails and CPU fallback kicks in. Add to the table below:

| Driver / GPU | Format / Modifier | Result | Notes |
|---|---|---|---|
| (fill in) | | | |

### Smoke result
Sign off after all 7 above pass:
- Tester: ____________
- Date: ____________
- Plasma version: ____________
- Driver: ____________
