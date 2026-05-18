# ShaderGlass Linux M4 — manual test checklist

Run on a real X11 or Wayland session (the nested agent X server is enough for
the X11 entries; Wayland entries need the user's actual desktop).

## Phase A — ImGui scaffolding + source picker

- [ ] `shaderglass --capture x11-screen --source monitor:root` opens a window
      and the "Source" panel lists at least one source.
- [ ] Clicking a different `monitor:*` row in the Source panel switches the
      visible capture within ~1 second.
- [ ] Clicking "Refresh" re-enumerates sources (open a new window, click
      Refresh, the new window appears).
- [ ] `shaderglass --capture wayland-screen` shows the "Open portal picker..."
      button; clicking it re-triggers the portal dialog.
- [ ] Closing the window (or pressing Esc) exits with code 0.
- [ ] All Phase A automated tests (`ctest -R AppState`) pass.

## Phase B — Preset library + browser

- [ ] `shaderglass --capture x11-screen --source monitor:root` shows a
      "Presets" panel listing the starter library grouped by category.
- [ ] Clicking a preset (e.g. `crt-easymode`) visibly changes the rendered
      output within ~200ms (first compile) and instantly on revisit.
- [ ] Clicking "✕ Passthrough" returns to the unmodified capture.
- [ ] The search filter narrows the list as you type ("scan" → only
      scanline entries remain visible).
- [ ] `--preset starter/crt-easymode.slangp` on launch starts with that
      preset already active.

## Phase C / D
_(extended by later tasks)_
