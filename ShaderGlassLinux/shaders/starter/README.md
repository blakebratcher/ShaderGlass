# Starter shader set

The ShaderGlass Linux M4 milestone ships a curated single-pass subset of
the RetroArch slang-shaders library so the preset browser has something
to browse out of the box. Multi-pass presets are M5 scope.

Each file is copied verbatim from upstream `libretro/slang-shaders`
(GPL-3.0) with `shader0 = ...` rewritten to be relative to this flat
directory.

To add more presets, drop a `.slangp` + its `.slang` here, ensure the
file declares exactly one `shader0`, and rebuild. The CMake install
rule copies the whole directory to `${CMAKE_INSTALL_DATADIR}/shaderglass/shaders/`.

## Preset inventory

| File | Category | Source upstream path |
|------|----------|----------------------|
| passthrough.slangp | Utility | stock.slangp |
| scanline.slangp | Scanlines | scanlines/scanline.slangp |
| crt-easymode.slangp | CRT | crt/crt-easymode.slangp |
| crt-aperture.slangp | CRT | crt/crt-aperture.slangp |
| crt-geom.slangp | CRT | crt/crt-geom.slangp |
| crt-lottes.slangp | CRT | crt/crt-lottes.slangp |
| lcd-grid.slangp | Handheld/LCD | handheld/lcd-grid.slangp |
| lcd1x.slangp | Handheld/LCD | handheld/lcd1x.slangp |
| xbrz-freescale.slangp | Upscaling | edge-smoothing/xbrz/xbrz-freescale.slangp |
| jinc2.slangp | Interpolation | interpolation/jinc2.slangp |
| hermite.slangp | Interpolation | interpolation/hermite.slangp |
| aann.slangp | Pixel-art scaling | pixel-art-scaling/aann.slangp |
| sharp-bilinear.slangp | Pixel-art scaling | pixel-art-scaling/sharp-bilinear.slangp |
| grade.slangp | Color grading | misc/grade-no-LUT.slangp |
| adaptive-sharpen.slangp | Sharpen | sharpen/adaptive-sharpen.slangp |
| cheap-sharpen.slangp | Sharpen | sharpen/cheap-sharpen.slangp |
| smart-blur.slangp | Blur | blurs/smart-blur.slangp |
| sharpsmoother.slangp | Blur | blurs/sharpsmoother.slangp |
| color-mangler.slangp | Color | misc/color-mangler.slangp |
| night-mode.slangp | Color | misc/night-mode.slangp |

## Substitutions from the original task candidate list

- **grayscale** — no upstream grayscale single-pass preset found; substituted
  `color-mangler.slang` (hue/saturation/brightness controls, misc category).
- **invert** — no upstream invert single-pass preset found; substituted
  `night-mode.slang` (warm amber tint, misc category).
- **crt-hyllian** — upstream is 5-pass (with LUT textures); substituted
  `crt-lottes.slang` (single-pass CRT, same category).
- **xbr-lv2** — upstream is 6-pass; substituted `xbrz-freescale.slang`
  (single-pass xBRZ freescale, same edge-smoothing category).
- **super-xbr** — upstream is 6-pass; substituted `hermite.slang`
  (single-pass interpolation).
- **blur5fast / box-blur** — upstream `gauss_4tap` is 2-pass, no direct
  single-pass box blur found; substituted `smart-blur.slang` + `sharpsmoother.slang`
  from the blurs category.
- **monochrome** — no simple upstream monochrome single-pass found; substituted
  `lcd1x.slang` (handheld LCD effect, distinct aesthetic).
