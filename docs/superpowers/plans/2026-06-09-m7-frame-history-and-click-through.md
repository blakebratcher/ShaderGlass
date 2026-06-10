# M7 Frame-History Textures + Click-Through X11 Overlay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement RetroArch frame-history/feedback semantic textures (`OriginalHistory#`, `PassOutput#`, `PassFeedback#`, aliases, `*Size#` semantics) and a true click-through X11 overlay (XShape empty input region + global F5 escape hatch).

**Architecture:** History frames are render-sampled into an `OffscreenTarget` ring via a builtin-passthrough blit pipeline; feedback double-buffers each feedback-sampled intermediate (parity swap per frame); `PassOutput#` is pure binding resolution onto existing intermediates. Click-through is a small SDL3-props + XShape wrapper polled per frame via `XQueryKeymap` for the F5 escape.

**Tech Stack:** C++20, Vulkan 1.3 dynamic rendering, SDL3 properties API, Xlib/XShape (libXext — already linked), gtest.

Spec: `docs/superpowers/specs/2026-06-09-m7-frame-history-and-click-through-design.md`

---

### Task 1: SlangSemantics name classification (pure functions)

**Files:**
- Create: `ShaderScope/src/render/SlangSemantics.h`, `ShaderScope/src/render/SlangSemantics.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (add to `shaderscope_core`)
- Test: `ShaderScope/tests/test_slang_semantics.cpp` (+ register in `ShaderScope/tests/CMakeLists.txt`)

- [ ] **Step 1: failing test** — table test: `Source`→Source, `Original`/`OriginalHistory0`→Original, `OriginalHistory3`→{OriginalHistory,3}, `PassOutput0`→{PassOutput,0}, `PassFeedback2`→{PassFeedback,2}, alias `RefPass`→{PassOutput,idx}, `RefPassFeedback`→{PassFeedback,idx}, `BLUR_RADIUS`/`OriginalHistoryX`→Unknown; size forms `OriginalHistorySize2`, `PassOutputSize0`, `PassFeedbackSize1`, `RefPassSize`, `RefPassFeedbackSize`; `isIndexedSizeSemanticName` true only for the numeric `*Size#` families.
- [ ] **Step 2: run** `ctest --test-dir build -R SlangSemantics` → fails to build (header missing).
- [ ] **Step 3: implement**

API:
```cpp
enum class SemanticTexKind { Source, Original, OriginalHistory, PassOutput, PassFeedback, Unknown };
struct SemanticTexRef { SemanticTexKind kind = SemanticTexKind::Unknown; uint32_t index = 0; };
SemanticTexRef classifySamplerName(const std::string& name,
                                   const std::map<std::string, uint32_t>& aliasToPass);
SemanticTexRef classifySizeSemanticName(const std::string& name,
                                        const std::map<std::string, uint32_t>& aliasToPass);
bool isIndexedSizeSemanticName(const std::string& name);
```
Implementation: `splitIndexed(name, prefix, idx)` helper (exact prefix + all-digit suffix, reject > 1000). Sampler classify order: exact `Source`/`Original`; `OriginalHistory#` (0 ⇒ Original); `PassOutput#`; `PassFeedback#`; alias lookup; `<alias>Feedback`. Size classify: numeric `OriginalHistorySize#`/`PassOutputSize#`/`PassFeedbackSize#`; else strip trailing `Size` and re-classify the base (covers alias forms).

- [ ] **Step 4: run** → PASS.
- [ ] **Step 5: commit** `feat(render): slang semantic-texture name classification`

### Task 2: ShaderPipeline extra semantic-texture bindings

**Files:**
- Modify: `ShaderScope/src/render/ShaderPipeline.h` (config + signature), `ShaderScope/src/render/ShaderPipeline.cpp` (`createPipeline` layout/pool, `bindAndDrawWithImageView`)

- [ ] **Step 1:** `ShaderPipelineSlangConfig` gains `std::vector<uint32_t> extraBindings;` (descriptor slots whose views the Preset resolves per draw).
- [ ] **Step 2:** `createPipeline`: `for (uint32_t eb : m_config.extraBindings) addSamplerBinding(eb);` after the original-bindings loop; include `extraBindings.size()` in pool `samplerCount` and the `bindings.reserve`.
- [ ] **Step 3:** `bindAndDrawWithImageView` gains trailing param `const std::vector<std::pair<uint32_t, VkImageView>>* extraViews = nullptr`; after the original-bindings writes: each `(binding, view)` pair gets an image write (skip `binding == m_sourceBinding`); reserve sizes account for it.
- [ ] **Step 4:** build clean (`cmake --build build -j`), full ctest green (no behavior change). Commit `feat(render): ShaderPipeline per-draw extra semantic-texture bindings`

### Task 3: Preset — classification, history ring, feedback double-buffer, size semantics

**Files:**
- Modify: `ShaderScope/src/render/Preset.h`, `ShaderScope/src/render/Preset.cpp`

New members: `m_aliasToPass` (from each pass's `PresetParams["alias"]`); `m_passSemanticTextures[pass]` = `vector<{binding, kind, index}>`; `m_maxHistory`; `m_history` ring (`m_maxHistory+1` source-sized R8G8B8A8_UNORM targets); `m_historyBlit` (builtin passthrough `ShaderPipeline`, created when `m_maxHistory > 0` using `builtin_shaders.h` SPIR-V); `m_feedback[N-1]` twin targets + `m_passHasFeedback[N-1]`; `m_feedbackParity` flipped in `advanceFrame()`.

- [ ] **Step 1: buildPipelines classification.** Replace the warn-everything branch: classify each non-Source/non-LUT sampler. `Original`/`OriginalHistory0` → `originalBindings` (unchanged). `OriginalHistory k≥1` (cap 16) → semantic texture + `m_maxHistory = max(...)`. `PassOutput k` valid iff `k < i` and `k < N-1`. `PassFeedback k` valid iff `k < N-1` (final-pass feedback unsupported → warn + original fallback); valid ⇒ `m_passHasFeedback[k] = true`. Unknown / invalid → existing warn + `originalBindings` fallback. Valid ones → `cfg.extraBindings` + `m_passSemanticTextures[i]`.
- [ ] **Step 2: ensureSourceSize.** Gate on `isMultiPass() || m_maxHistory > 0`; the unchanged-early-return must also check history/feedback allocation; reallocate intermediates (multi-pass), feedback twins (same extent/format as intermediate k), history ring. All fresh targets start `VK_IMAGE_LAYOUT_UNDEFINED` (lazily cleared in-band).
- [ ] **Step 3: current/previous target helpers + view resolution.** `currentTarget(k)` = parity ? feedback[k] : intermediates[k] (when twinned, else intermediates[k]); `previousTarget(k)` = the other. `resolveSemanticView(st, passIdx)`: OriginalHistory k → `m_history[(m_frameCount + ring − k%ring) % ring]`; PassOutput k → `currentTarget(k).view()`; PassFeedback k → `previousTarget(k).view()`; fallback `m_originalView`.
- [ ] **Step 4: recordIntermediatePasses.** First: `clearIfFresh` every UNDEFINED-layout history/feedback/intermediate target (empty `LOAD_OP_CLEAR` dynamic-rendering scope + transition to SHADER_READ_ONLY). Then history blit: transition write slot → COLOR_ATTACHMENT, render `m_historyBlit` sampling `sourceView` at slot extent, transition → SHADER_READ_ONLY. Then the intermediate-pass loop rendering into `currentTarget(i)` and passing per-pass `extras` to `bindAndDrawWithImageView`. Single-pass-with-history: history work + `m_finalInputView = sourceView`; return. `drawFinalPass` passes the final pass's extras.
- [ ] **Step 5: size semantics.** `isSemanticName`: existing list ∪ `isIndexedSizeSemanticName`. `writeParamValue`: after `FrameDirection`, a `!isUserParam(p)` branch classifying via `classifySizeSemanticName(p.name, m_aliasToPass)`: Original/OriginalHistory → src size; PassOutput/PassFeedback k → intermediate k extent (or viewport fallback).
- [ ] **Step 6:** `requiresCustomRenderPath()` = `isMultiPass() || m_maxHistory > 0`. Build clean; existing ctest green. Commit `feat(render): frame-history ring, pass-output and feedback textures in Preset`

### Task 4: Fixtures + in-process multi-frame e2e tests

**Files:**
- Create fixtures under `ShaderScope/tests/data/`: `history1.slangp` + `history1.slang`; `feedback.slangp` + `pass-red.slang`(passthrough) + `show-feedback.slang`; `passoutput.slangp` + `invert.slang` + `show-passoutput.slang`; `alias.slangp` + `show-alias.slang`
- Create: `ShaderScope/tests/test_frame_history.cpp` (+ register, link `shaderscope_core`)

Fixture shape (RetroArch slang, mirror `tests/data/stock.slang`): UBO `global { mat4 MVP; }` binding 0; semantic samplers at binding 3+ (binding 2 is the un-reflected Source fallback slot — avoid collisions). E.g. `history1.slang` fragment: `layout(set=0, binding=3) uniform sampler2D OriginalHistory1; FragColor = texture(OriginalHistory1, vTexCoord);`

- [ ] **Step 1: failing tests** (in-process: `VulkanContext` + `Texture` + `HeadlessOutput` + `Preset`, mirroring `test_texture_upload.cpp` setup; `advanceFrame()` before each `renderToBytes`):
  - `OriginalHistory1SamplesPreviousFrame`: frame 1 red input → black output; frame 2 green input → red output.
  - `PassFeedbackSamplesPreviousFrameOutput`: frame 1 → black; frame 2 → red (frame 1's pass-0 output).
  - `PassOutputSamplesEarlierPassThisFrame`: 3-pass (pass 1 inverts); final samples `PassOutput0` → red survives in frame 1.
  - `AliasResolvesToPassOutput`: final samples `RefPass` (alias of pass 0) → red in frame 1.
- [ ] **Step 2: run** → FAIL (samplers degrade to original / fixtures warn).
- [ ] **Step 3:** fix any implementation gaps until PASS.
- [ ] **Step 4:** full ctest sweep green. Commit `test(render): multi-frame history/feedback/pass-output e2e coverage`

### Task 5: main.cpp routing + motion-blur starter preset

**Files:**
- Modify: `ShaderScope/src/main.cpp:810` (`multiPass` → `state.preset->requiresCustomRenderPath()`, both DMA-BUF + CPU branches)
- Create: `ShaderScope/shaders/starter/motionblur.slangp` + `motionblur.slang` (average of OriginalHistory0..2; re-run cmake for the staging glob)

- [ ] **Step 1:** routing change; build; ctest green.
- [ ] **Step 2:** starter preset; verify `./build/ShaderScope/shaderscope --headless --preset build/ShaderScope/shaders-staging/motionblur.slangp --input ShaderScope/tests/data/4x4_red.png --output /tmp/mb.png --width 4 --height 4` exits 0 and `/tmp/mb.png` is non-black.
- [ ] **Step 3:** commit `feat: route history presets through custom render path; motionblur starter preset`

### Task 6: X11 click-through

**Files:**
- Create: `ShaderScope/src/output/X11ClickThrough.h`, `ShaderScope/src/output/X11ClickThrough.cpp` (add to core CMake)
- Test: `ShaderScope/tests/test_x11_clickthrough.cpp`

- [ ] **Step 1: failing test** — bare `XCreateSimpleWindow`; `applyClickThroughShape(dpy, win, true)` ⇒ `XShapeGetRectangles(…, ShapeInput, …)` count == 0; `false` ⇒ count > 0. `GTEST_SKIP` without `DISPLAY`/Shape.
- [ ] **Step 2: implement** free function (`XShapeCombineRectangles` empty set / `XShapeCombineMask(None)` reset + `XFlush`) and `X11ClickThrough` class: ctor reads `SDL_PROP_WINDOW_X11_DISPLAY_POINTER` + `SDL_PROP_WINDOW_X11_WINDOW_NUMBER` from `SDL_GetWindowProperties`, probes `XShapeQueryExtension`, caches `XKeysymToKeycode(dpy, XK_F5)`; `setEnabled` applies shape and seeds `m_keyWasDown = true` (swallow the enabling press); `pollDisableKey` = `XQueryKeymap` bit test + press-edge detection while enabled.
- [ ] **Step 3:** test PASS. Commit `feat(output): X11 click-through via XShape input region`

### Task 7: F5 wiring + escape hatch + UI text

**Files:**
- Modify: `ShaderScope/src/ui/AppState.h` (add `bool clickThrough = false;`), `ShaderScope/src/main.cpp` (F5 case + per-frame poll), `ShaderScope/src/ui/HelpPanel.cpp` (F5 row)

- [ ] **Step 1:** construct `X11ClickThrough clickThrough(window.handle());` after window creation. F5 case: unsupported → warn toast "Click-through requires X11"; else toggle, mirror into `state.clickThrough`, toast "Click-through on — press F5 to restore" / "Click-through off".
- [ ] **Step 2:** frame loop (after `pollEvents`): `if (state.clickThrough && clickThrough.pollDisableKey()) { clickThrough.setEnabled(false); state.clickThrough = false; infoToast("Click-through off"); }`
- [ ] **Step 3:** HelpPanel F5 entry. Build; ctest green; live smoke (`timeout 8 ./build/ShaderScope/shaderscope --capture x11-screen`, exit 124, no errors in log). Commit `feat: F5 click-through hotkey with global escape hatch`

### Task 8: Docs + final verification

**Files:**
- Modify: `CLAUDE.md` (hotkeys +F5, Known limitations rewrite, milestone table M7 shipped, gotchas: history ring / feedback parity / XQueryKeymap escape), `CHANGELOG.md`
- Create: `docs/manual-tests-m7.md`

- [ ] **Step 1:** full `ctest --test-dir build --output-on-failure` — all green (DMA-BUF GBM skip allowed).
- [ ] **Step 2:** headless sweep over every starter preset (existing loop pattern) — all exit 0, non-black.
- [ ] **Step 3:** docs updates + manual checklist; commit `docs: M7 shipped — frame-history textures + click-through overlay`

---

## Self-review

- Spec coverage: classification (T1), pipeline bindings (T2), Preset storage/resolution/sizes (T3), tests+fixtures (T4), routing+starter (T5), click-through core+test (T6), hotkey+escape (T7), docs (T8). Final-pass-feedback limitation handled in T3 step 1. ✓
- No placeholders; signatures consistent (`extraViews` param name, `requiresCustomRenderPath`, `applyClickThroughShape`). ✓
- ShaderDef.h include-order gotcha applies to `test_frame_history.cpp` (include `<filesystem>`, `<map>`, `<string>`, `<vector>` first). Binding-2 collision avoidance noted in T4. ✓
