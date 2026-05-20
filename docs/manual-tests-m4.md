# ShaderScope Linux M4 — manual test checklist

Run on a real X11 or Wayland session (the nested agent X server is enough for
the X11 entries; Wayland entries need the user's actual desktop).

## Phase A — ImGui scaffolding + source picker

- [ ] `shaderscope --capture x11-screen --source monitor:root` opens a window
      and the "Source" panel lists at least one source.
- [ ] Clicking a different `monitor:*` row in the Source panel switches the
      visible capture within ~1 second.
- [ ] Clicking "Refresh" re-enumerates sources (open a new window, click
      Refresh, the new window appears).
- [ ] `shaderscope --capture wayland-screen` shows the "Open portal picker..."
      button; clicking it re-triggers the portal dialog.
- [ ] Closing the window (or pressing Esc) exits with code 0.
- [ ] All Phase A automated tests (`ctest -R AppState`) pass.

## Phase B — Preset library + browser

- [ ] `shaderscope --capture x11-screen --source monitor:root` shows a
      "Presets" panel listing the starter library grouped by category.
- [ ] Clicking a preset (e.g. `crt-easymode`) visibly changes the rendered
      output within ~200ms (first compile) and instantly on revisit.
- [ ] Clicking "✕ Passthrough" returns to the unmodified capture.
- [ ] The search filter narrows the list as you type ("scan" → only
      scanline entries remain visible).
- [ ] `--preset starter/crt-easymode.slangp` on launch starts with that
      preset already active.

## Phase C — Parameter editor

- [ ] Launch with a CRT preset (`--preset .../crt-easymode.slangp`); the
      Parameters panel shows sliders + checkboxes for the preset's params.
- [ ] Dragging a slider visibly changes the rendered output within the
      same frame (no perceptible lag).
- [ ] Clicking "Reset to defaults" snaps every slider back to its default
      and the output updates accordingly.
- [ ] Switching to a different preset (e.g. `lcd-grid`) replaces the
      param list with the new preset's params; values are at defaults.
- [ ] Switching back to `crt-easymode` again — params reset to defaults
      (any tweaks you made earlier are NOT preserved; spec-confirmed).
- [ ] All Phase C automated tests (`ctest -R AppState`) pass.

## Phase D — Config persistence + session restore

- [ ] `shaderscope --reset-config` exits 0 with "removed ..." message;
      ~/.config/shaderscope/config.json is gone afterwards.
- [ ] Launch with `--capture x11-screen --source monitor:root --preset
      .../crt-easymode.slangp`, tweak a few sliders, exit.
- [ ] Re-launch `shaderscope --capture x11-screen --source monitor:root`
      (no --preset). crt-easymode is auto-restored with the tweaks intact.
- [ ] Re-launch `shaderscope` (no flags). Window opens; same source +
      preset + params are restored.
- [ ] Drag a panel to a new dock position, exit, re-launch. The new
      dock layout is restored (imgui.ini is the proof).
- [ ] Hand-corrupt ~/.config/shaderscope/config.json (e.g. `echo "{"
      > $_`); re-launch. App starts with defaults, logs a warning.

## M4 final integration

- [ ] All Phase A/B/C/D automated tests pass (`ctest`).
- [ ] All Phase A/B/C/D manual checks pass on the user's actual desktop
      (X11 *and* Wayland, where applicable).
- [ ] `shaderscope --version` still reports a sensible commit hash + date.
- [ ] `shaderscope --help` includes the `--reset-config` line.
- [ ] No new validation-layer errors in `cmake --build` or `ctest`
      output beyond the M3 baseline.

## Known limitations (M4 → M5 follow-ups)

- **Bare `shaderscope` requires a saved session.** On a fresh config
  (after `--reset-config` or first install), `shaderscope` with no flags
  infers the capture kind from env but still needs `--source` for X11.
  Once a session is saved, subsequent bare launches auto-resume the
  source. Fixing the first-run UX (deferred capture + GUI-only source
  pick) is M5 scope — it requires relaxing the no-source guard and
  making the render loop tolerate a null capture for the lifetime of
  the picker.
- **Multi-pass shaders unsupported.** Preset constructor throws on
  `ShaderDefs.size() > 1`. The starter set is hand-curated to be
  single-pass. M5 will add multi-pass + inter-pass render-target
  management.
- **No toast UI for errors.** Source-switch failures, preset compile
  failures, and unsupported-fourcc errors log to stderr instead of
  surfacing in the ImGui window. M5 will add a transient toast component.
