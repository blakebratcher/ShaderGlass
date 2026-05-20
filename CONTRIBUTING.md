# Contributing to ShaderScope

Thanks for considering it. This is a small project so the bar for
contributions is "make it work and don't break what already works."

## Build setup

See [`docs/build-linux.md`](docs/build-linux.md) for distro-specific
dependency lists. Short version:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

77 gtest binaries should all pass (one DMA-BUF test skips on systems
without a GBM-capable iGPU; that's environmental, not a failure).

## Project layout

- `ShaderScope/src/` — the Linux app proper (Vulkan + SDL3 + ImGui).
  Capture / render / ui / util subfolders. See
  [CLAUDE.md](CLAUDE.md) for the per-layer breakdown.
- `ShaderGC/` — shared slang→SPIR-V compiler from upstream
  [mausimus/ShaderGlass](https://github.com/mausimus/ShaderGlass).
  Do NOT touch the upstream GPL headers (they preserve attribution).
- `packaging/` — `.desktop`, icons, man page, AppStream metainfo,
  AppImage build script, Flatpak manifest.
- `docs/superpowers/{specs,plans}/` — per-milestone design specs and
  TDD task plans.

## Code style

- C++20. camelCase functions/members, PascalCase types, `m_*` prefix
  for non-public class members, `kFoo` for static constants.
- One short comment per non-obvious WHY; no docstrings on what
  identifiers already say.
- New tests under `ShaderScope/tests/test_<thing>.cpp`, register in
  `ShaderScope/tests/CMakeLists.txt` with `gtest_discover_tests(...)`.
- Use `LOG_INFO` / `LOG_WARN` / `LOG_ERROR` / `LOG_DEBUG` (filtered by
  `SHADERSCOPE_LOG`). User-visible messages go through
  `Logging::{info,ok,warn,error}Toast(state, msg)` which logs AND posts
  to the on-screen toast queue.

## Commits

- Conventional-ish: `feat(<area>):`, `fix(<area>):`, `chore:`, `docs:`,
  `test:`, `build:`, `ci:`, `refactor(<area>):`.
- One logical change per commit. Bug-fix and refactor go in separate
  commits even if they touch the same file.
- If a commit is fixing a previously merged regression, mention the
  prior SHA in the message body.

## Pull requests

- Branch off `linux/main`. Don't open PRs against `master` — that's
  the upstream Windows trunk.
- `ctest --test-dir build` must be green. If your change can't be
  exercised by a unit test, add a manual-smoke note to
  `docs/manual-tests-*.md`.
- For UI changes, attach a screenshot or describe the visual
  difference in plain text.
- If you're touching the multi-pass render path, confirm both
  `crt-easymode.slangp` (single-pass) and `passthrough-2pass.slangp`
  (2-pass) still render correctly.

## What to expect

This is a one-person Linux fork. Reviews can be slow. Be patient or,
if you want it merged faster, keep the PR small, well-described, and
focused on one thing.

## Code of conduct

See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Be kind and curious.
