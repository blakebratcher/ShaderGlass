# ShaderScope Linux M5 UX polish — manual test checklist

Run these on your actual desktop session (Plasma Wayland + X11). The
nested-XFCE agent shell that does CI smoke can't cover portal flows,
the system XDG_PICTURES_DIR, or interactive ImGui widgets.

## Toast UI
- [ ] Switch to a deleted/invalid source — red error toast bottom-right
- [ ] Take a screenshot — green success toast with filename
- [ ] Five toasts in quick succession (e.g. 5 bad-source switches) — stack
      shows newest 5; expiry order correct
- [ ] Click a toast — it disappears immediately

## First-run
- [ ] `--reset-config && shaderscope` — window opens with splash; picking
      a source begins capture
- [ ] `shaderscope --list-sources` prints sources, exits 0
- [ ] `shaderscope --capture x11-screen --list-sources` lists X11 sources, exits 0
- [ ] `shaderscope --capture x11-screen` (no source) opens GUI (no longer
      exits)

## Region/crop
- [ ] Click "Crop region", drag a rect, press Enter — viewport shows only
      that region scaled to fill
- [ ] Drag a tiny rect (<16x16) — silently snapped to 16x16 around midpoint
- [ ] Esc mid-drag — no crop change
- [ ] Switch sources while in crop mode — crop discarded
- [ ] "Clear crop" — full source returns
- [ ] Restart app — crop restored for the active source

## Screenshot
- [ ] Click "Screenshot" — file appears at `$XDG_PICTURES_DIR/shaderscope-*.png`
- [ ] Open the PNG — post-pipeline render (preset applied, cropped if
      applicable, no ImGui chrome)
- [ ] Read-only `$XDG_PICTURES_DIR` — red error toast with the path
- [ ] Two rapid clicks — only one file written (single in-flight)

## M5 final integration
- [ ] All automated tests pass (`ctest --test-dir build`).
- [ ] All manual checks above pass on the user's actual desktop (X11 + Wayland).
- [ ] `--help` documents `--list-sources`.
- [ ] No new validation-layer errors versus the M4 baseline.
