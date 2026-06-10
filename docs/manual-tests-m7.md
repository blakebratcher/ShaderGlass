# M7 manual smoke checklist — frame-history + click-through overlay

Build: `cmake --build build -j` · Run: `./build/ShaderScope/shaderscope`

## Frame-history textures

- [ ] Load `motionblur.slangp` from the preset browser with an X11 screen
      capture active. Move a window around inside the captured area —
      moving edges leave a short trail; static content stays sharp.
- [ ] Params panel shows `Blur Strength`; dragging it to 0 removes the
      trail, 1.0 maximises it. Built-in semantics (MVP, sizes) stay hidden.
- [ ] Resize the ShaderScope window while motionblur is active — no
      validation errors / crash; the effect resumes after a one-frame
      black reset of the history (expected).
- [ ] `]` / `[` cycle through every starter preset with capture running —
      no crash entering/leaving motionblur.
- [ ] Headless parity: render any `.slangp` that samples `PassFeedback#`
      (e.g. `ShaderScope/tests/data/feedback.slangp`) — first frame is
      black by design; no warnings about unsupported history/feedback in
      the log for supported forms.

## Click-through overlay (X11)

- [ ] Press `F5` with the window focused → toast "Click-through on — press
      F5 to restore". Clicks inside the ShaderScope window now reach the
      window *behind* it (e.g. select text in a terminal underneath).
- [ ] ImGui panels no longer react to the mouse while active (expected —
      all pointer input passes through).
- [ ] Click another window (focus leaves ShaderScope), then press `F5`
      again → toast "Click-through off"; the window is clickable again.
      This exercises the global-keymap escape hatch.
- [ ] `F2` + `F3` + `F4` + `F5` together: borderless, always-on-top,
      chrome-free, click-through — full overlay mode over a game/video.
- [ ] On a Wayland session, `F5` shows "Click-through requires X11" and
      changes nothing.
- [ ] Esc still closes the window when it has keyboard focus (keyboard
      focus is unaffected by the input shape).

## Known-limitation spot checks

- [ ] A preset sampling `PassFeedback` of its *final* pass logs a warning
      at load and renders with black at that slot (not garbage).
- [ ] Overlaying the same monitor that is being captured still produces
      the hall-of-mirrors feedback (pre-existing, unrelated to F5).
