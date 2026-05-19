# ShaderGlass Linux M5 feature-complete — manual test checklist

Run these on your real Plasma Wayland + X11 sessions. Agent CI cannot
drive the ImGui inputs or test transparent overlay behaviour.

## Multi-pass shaders

- [ ] Load `passthrough-2pass.slangp` from the preset browser; output
      looks identical to single-pass passthrough.
- [ ] Load any 2- or 3-pass RetroArch shader you have locally — it
      compiles and renders without artifacts at native source resolution.
- [ ] Resize the source between capture starts (e.g. switch monitors);
      intermediates rebuild without crash (look for the
      `vkDeviceWaitIdle` step in the log).
- [ ] Multi-pass + crop region: known limitation — slang shaders ignore
      the UV-transform push constant, so crop only works with passthrough.
      Confirm graceful no-op (no crash, just no crop applied).
- [ ] A preset that declares LUTs (`TextureDefs`) loads with a warn toast
      but doesn't crash. Output may be visually wrong — LUT binding is
      deferred work.

## Runtime preset import

- [ ] Drag a `.slangp` file from your file manager onto the window —
      info toast appears with the path; preset loads.
- [ ] Drag a non-`.slangp` file (e.g. a `.png`) — warn toast says
      "Drop ignored — expected .slangp".
- [ ] Preset browser → **Import…** → type a full path → Enter — same
      behaviour as drag-and-drop.
- [ ] Type a path to a non-existent file → red error toast with the
      compile error.
- [ ] Type a path to a `.slangp` that fails to compile → red error toast
      with the compiler error; previous preset stays active.

## Hotkeys

- [ ] `F11` (no focus in a text field) → screenshot saved to
      `$XDG_PICTURES_DIR/shaderglass-*.png`. Verify toast.
- [ ] `B` toggles active preset ⇄ passthrough; second press restores
      the prior preset. Verify toasts.
- [ ] `]` cycles next preset; `[` cycles previous. Wraps at the ends.
      Toast shows the new preset name.
- [ ] `F1` shows a help toast listing all hotkeys.
- [ ] `F2` hides ImGui panels — shader output fills the window. Press
      again to restore.
- [ ] `F3` toggles always-on-top — window stays above other windows.
- [ ] `F4` toggles borderless — window decorations vanish.
- [ ] Click into a text input (e.g. the preset filter); press `]` —
      character is typed into the field, preset is NOT cycled. Click
      away → `]` resumes cycling.
- [ ] `Esc` closes the window.

## Regressions

- [ ] All M5 UX-polish checks (`docs/manual-tests-m5-ux-polish.md`)
      still pass.
- [ ] Single-pass `.slangp` (e.g. `crt-easymode`) still renders
      identically to before the multi-pass refactor.
- [ ] Crop drag + clear flow unchanged.
- [ ] Screenshot still excludes ImGui chrome (look for clean PNG).
- [ ] `ctest --test-dir build` green.
