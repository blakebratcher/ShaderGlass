# M6 render-correctness — manual smoke checklist

Scope: RetroArch slang semantic support (SPIR-V reflection, quad VBO, push
constants), swapchain recreation, composited X11 capture + DRI3 DMA-BUF.

## Automated pre-checks (run first)

```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```

All tests green (DmaBufImport.ImportsGbmAllocatedBuffer may skip — environmental).

```bash
# Full starter-preset sweep: every preset must produce non-black output.
for p in build/ShaderScope/shaders-staging/*.slangp; do
  ./build/ShaderScope/shaderscope --headless \
    --input ShaderScope/tests/data/4x4_red.png \
    --output "/tmp/sweep-$(basename "$p" .slangp).png" \
    --preset "$p" --width 128 --height 128 || echo "FAIL: $p"
done
```

## 1. Slang preset rendering (the M6 headline)

- [ ] `./build/ShaderScope/shaderscope <any.png> --preset build/ShaderScope/shaders-staging/crt-easymode.slangp`
      — window shows the image with visible CRT scanlines/mask (NOT black).
- [ ] Same with `crt-geom.slangp` — curvature + scanlines visible.
- [ ] Same with `passthrough.slangp` — image reproduced unchanged.
- [ ] Same with `passthrough-2pass.slangp` — image reproduced unchanged (multi-pass).
- [ ] Params panel: drag a CRT param slider (e.g. SCANLINE_STRENGTH) —
      effect changes live.
- [ ] Params panel shows ONLY user parameters (no MVP / SourceSize /
      OutputSize / FrameCount entries).
- [ ] "Reset to defaults" restores the .slangp baseline.
- [ ] Preset cycling `[` / `]` walks the starter set; every preset shows
      real (non-black) output.
- [ ] Bypass `B` toggles between preset and passthrough.

## 2. Crop with slang presets

- [ ] Activate a crop rectangle (drag on the source picker's crop overlay)
      while a CRT preset is active — the cropped region (not the full
      source) is what gets the CRT treatment.
- [ ] Clear crop — full source returns.

## 3. Swapchain recreation

- [ ] `./build/ShaderScope/shaderscope <any.png>` — launches without
      crashing (this used to abort with VK_ERROR_OUT_OF_DATE_KHR on frame 1).
- [ ] Drag-resize the window continuously for a few seconds — no crash,
      image keeps rendering at the new size.
- [ ] Minimize → wait 2 s → restore — no crash, rendering resumes.

## 4. Composited X11 capture (needs a GL-backend compositor, e.g. picom glx)

- [ ] `./build/ShaderScope/shaderscope --capture x11-screen --source <monitor>`
      — the window shows the actual desktop content (other windows visible),
      not just the wallpaper / dark grey.
- [ ] Log shows `DRI3 DMA-BUF fast path enabled` (when xcb-dri3 present).
- [ ] `SHADERSCOPE_DISABLE_X11_DMABUF=1 ./build/ShaderScope/shaderscope
      --capture x11-screen --source <monitor>` — still shows composited
      content (CPU fallback path).
- [ ] Apply a CRT preset on top of live capture — desktop renders through
      the shader.

## 5. Regression spot-checks

- [ ] Wayland capture still works (on a Wayland session):
      `--capture wayland-screen`.
- [ ] F11 screenshot saves a PNG showing the shader output.
- [ ] Session restore: quit and relaunch — last source + preset + params
      + window geometry come back.
