# M7 — Frame-history textures + click-through X11 overlay (design)

Date: 2026-06-09
Status: approved (autonomous session; user directive "finish the project" = implement
the two documented Future-milestone items)

M7 closes out the two items the milestone table lists as "Future":

1. **Frame-history textures** — make `OriginalHistory#`, `PassOutput#`, and
   `PassFeedback#` (plus their `*Size#` semantics and `.slangp` alias names) real
   instead of degrading to the current original input.
2. **True click-through X11 overlay** — the window stops receiving mouse input via
   the XShape input region, so clicks land on the windows beneath.

---

## Feature A — frame-history textures

### Semantics being implemented (RetroArch slang spec)

| Sampler name | Meaning | Storage |
|---|---|---|
| `OriginalHistory0` | current original input | already works (= Original) |
| `OriginalHistory1..N` | original input from 1..N frames ago | NEW: history ring buffer |
| `PassOutput#` | output of pass # **this** frame (# < current pass) | existing intermediates — binding resolution only |
| `PassFeedback#` | output of pass # from the **previous** frame | NEW: double-buffered intermediates |
| `<alias>` | `PassOutput` of the pass whose `.slangp` `aliasN` matches | name resolution via PresetDef |
| `<alias>Feedback` | `PassFeedback` of that pass | name resolution via PresetDef |

UBO/push semantics: `OriginalHistorySize#` (= source size), `PassOutputSize#`
(= pass # output extent), `PassFeedbackSize#` (= same), and the alias-named
`<alias>Size` / `<alias>FeedbackSize` forms.

### Approaches considered

- **(chosen) Render-sample copies + double-buffered intermediates.** History frames
  are captured by *rendering* the source view into a ring of `OffscreenTarget`s with
  a builtin-passthrough `ShaderPipeline` (a "history blit"). Feedback is provided by
  ping-ponging each feedback-sampled intermediate between two targets per frame.
  Pros: only needs `SAMPLED` usage on the capture image (true for both the CPU
  `Texture` and the imported DMA-BUF), handles format conversion for free, reuses
  existing dynamic-rendering + layout-transition machinery. Cons: one extra
  fullscreen draw per frame when history is in use.
- **`vkCmdBlitImage` copies.** Cheaper per frame, but requires `TRANSFER_SRC` usage
  on the source image — not guaranteed for DMA-BUF imports across drivers, and the
  CPU `Texture` would need a usage change too. Rejected for robustness.
- **Persist N full capture frames CPU-side.** Re-upload old frames each frame.
  Rejected: pointless bandwidth, doesn't work for the DMA-BUF zero-copy path.

### Architecture

**Name classification** — new `ShaderScope/src/render/SlangSemantics.{h,cpp}`:
pure functions, unit-testable without Vulkan:

```cpp
enum class SemanticTexKind { Source, Original, OriginalHistory, PassOutput, PassFeedback, Unknown };
struct SemanticTexRef { SemanticTexKind kind; uint32_t index; };
// aliasToPass: .slangp aliasN values → pass index
SemanticTexRef classifySamplerName(const std::string& name,
                                   const std::map<std::string, uint32_t>& aliasToPass);
// "OriginalHistorySize3" → {OriginalHistory,3}; "PassOutputSize0" → {PassOutput,0};
// "<alias>Size"/"<alias>FeedbackSize" via aliasToPass; Unknown otherwise.
SemanticTexRef classifySizeSemanticName(const std::string& name,
                                        const std::map<std::string, uint32_t>& aliasToPass);
```

**ShaderPipeline** — `ShaderPipelineSlangConfig` gains
`std::vector<uint32_t> extraBindings` (descriptor-layout slots for semantic
textures the *Preset* resolves per draw). `bindAndDrawWithImageView` gains an
optional `const std::vector<std::pair<uint32_t, VkImageView>>* extraViews`
parameter; each pair is written into the descriptor set alongside Source/Original.
The pipeline stays semantics-blind: bindings in, views in, no name knowledge.

**Preset** — owns all the new state:

- `m_passSemanticTextures[pass]` — `vector<{binding, kind, index}>` built in
  `buildPipelines()` from sampler-name classification. `Original` /
  `OriginalHistory0` keep the existing `originalBindings` route. Unknown names keep
  the warn + bind-original fallback.
- **History ring**: `m_history` = `maxHistoryIndex + 1` source-sized
  `OffscreenTarget`s (R8G8B8A8_UNORM). Frame *f* renders the source into slot
  `f % (N+1)` via the history-blit pipeline (builtin passthrough SPIR-V, identity
  UV — matches the existing uncropped `Original` semantics); `OriginalHistoryK`
  binds slot `(f−K) mod (N+1)`. Fresh slots are cleared to black in-band with an
  empty `LOAD_OP_CLEAR` dynamic-rendering scope (no extra usage flags needed), so
  early frames sample black like RetroArch.
- **Feedback double-buffer**: each feedback-sampled intermediate pass *k* gets a
  second target; `cur[k]`/`prev[k]` pointers swap in `advanceFrame()`. Pass *k*
  renders into `cur[k]`; `PassFeedbackK` binds `prev[k]`; `PassOutputK` and the
  next pass's Source bind `cur[k]`. Both targets are black-cleared on (re)alloc.
- `recordIntermediatePasses()` records: history clear/blit first, then the
  intermediate passes with per-pass extra views. `drawFinalPass()` resolves the
  final pass's extra views the same way.
- `ensureSourceSize()` (re)allocates history + feedback targets alongside the
  intermediates.
- `updateUbo()`/`writeParamValue()` compute the `*Size#` semantics;
  `isSemanticName()` recognises the numeric families (alias forms are resolved
  instance-side where the alias map exists).
- `requiresCustomRenderPath()` — true when multi-pass **or** history is used;
  `main.cpp` routes such presets through `recordIntermediatePasses` +
  `drawFinalPass` (headless already does unconditionally).

**Synchronization**: `main.cpp` already calls `engine.waitForInFlightFrames()`
before `advanceFrame()` whenever a preset is active, so the previous frame's GPU
reads/writes complete before this frame's swap/record. No new fences.

### Known limitations (documented, deliberate)

- **Final-pass feedback** is unsupported: the final pass renders into the
  swapchain, which we don't copy back. Sampling `PassFeedback(N−1)` warns at build
  and binds black. (Feedback of the displayed pass has no practical preset usage.)
- History copies are uncropped, matching the existing `Original` binding behavior
  under crop.
- History/feedback targets reallocate (and clear to black) on source/viewport
  resize — a one-frame visual reset, same as RetroArch.

### Tests

- Unit: `test_slang_semantics.cpp` — classification table incl. aliases and size
  forms.
- E2E headless (new fixtures under `tests/data/`):
  - `history1.slangp` — final output = `OriginalHistory1`. Frame 1 (red input) →
    black; re-upload green, frame 2 → red.
  - `feedback.slangp` — 2-pass; final outputs `PassFeedback0`. Frame 1 → black;
    frame 2 → frame 1's pass-0 output.
  - `passoutput.slangp` — 3-pass; pass 1 inverts, final outputs `PassOutput0` →
    original input survives.
  - `alias.slangp` — pass 0 aliased; final samples the alias name → pass-0 output.
- New starter preset `motionblur.slangp` (blend of OriginalHistory0..2) as a
  user-visible demo.

---

## Feature B — click-through X11 overlay

### Approaches considered

- **(chosen) XShape empty input region + global-keymap escape hatch.** XShape's
  `ShapeInput` region set to empty makes the X server route all pointer events to
  whatever is beneath the window; resetting the region restores normal input.
  XShape is part of libXext, which is **already linked** (`pkg_check_modules(X11 …
  xext …)`) — zero new dependencies. Because a click-through window can't be
  clicked to turn the mode off (and keyboard focus is lost once the user clicks
  elsewhere), the frame loop polls `XQueryKeymap` — which reads global keyboard
  state regardless of focus — for an F5 press edge while the mode is active.
- **32-bit ARGB visual (`SDL_WINDOW_TRANSPARENT`)**: rejected. The shader output
  fills the window opaquely — per-pixel transparency buys nothing here, while
  requiring an alpha-composite-capable swapchain and making shaders that write
  garbage alpha punch holes in the window. The Future-milestone note named it, but
  XShape alone delivers the actual feature (clicks pass through); this deviation is
  recorded here and in CLAUDE.md.
- **X11 global hotkey via `XGrabKey`**: rejected for the escape hatch — grabbed key
  events bypass SDL's event loop and the grab steals the key system-wide.

### Architecture

New `ShaderScope/src/output/X11ClickThrough.{h,cpp}`:

```cpp
// Free function — testable against a bare X window without SDL:
bool applyClickThroughShape(Display* dpy, Window win, bool enabled);

class X11ClickThrough {
public:
    explicit X11ClickThrough(SDL_Window* win);  // SDL3 props: X11 display + window
    bool supported() const;   // false on Wayland / missing Shape extension
    bool enabled()   const;
    bool setEnabled(bool on); // applyClickThroughShape + state
    bool pollDisableKey();    // XQueryKeymap F5 press-edge while enabled
};
```

- `SDL_GetPointerProperty(…, SDL_PROP_WINDOW_X11_DISPLAY_POINTER)` +
  `SDL_GetNumberProperty(…, SDL_PROP_WINDOW_X11_WINDOW_NUMBER)` provide the native
  handles; absence ⇒ unsupported (Wayland), toggle shows a warn toast.
- Enable: `XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, nullptr, 0,
  ShapeSet, Unsorted)`. Disable: `XShapeCombineMask(dpy, win, ShapeInput, 0, 0,
  None, ShapeSet)`.
- **F5** hotkey toggles (next free slot after F1–F4); toast announces
  "Click-through on — press F5 to restore". `AppState.clickThrough` mirrors the
  state (not persisted — session-only, like F2/F3/F4). Composes with F2 (hide
  chrome), F3 (top), F4 (borderless) for full overlay mode.
- Self-capture note: an overlay above its own captured monitor feeds back
  (hall-of-mirrors) exactly as today; unchanged by this feature, documented.

### Tests

- `test_x11_clickthrough.cpp` — creates a bare Xlib window, calls
  `applyClickThroughShape(true/false)`, asserts via `XShapeGetRectangles` that the
  input region is empty / restored. `GTEST_SKIP` when `DISPLAY` is unset.
- Edge-detection logic unit-tested via injected key-state bits.
- Manual smoke checklist `docs/manual-tests-m7.md` (live click-through, escape
  hatch, Wayland refusal toast).

---

## Docs & follow-through

- CLAUDE.md: hotkey list (+F5), Known limitations (history entry replaced by the
  final-pass-feedback note), milestone table (M7 shipped), gotchas (history ring,
  feedback ping-pong, escape-hatch polling).
- CHANGELOG.md entries; `docs/manual-tests-m7.md`; build-linux.md unchanged (no
  new deps).
