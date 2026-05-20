---
name: Bug report
about: Something doesn't work the way it should
title: ''
labels: bug
assignees: ''
---

**Describe the bug**
What happened, in one or two sentences.

**Steps to reproduce**
1. Run `shaderscope --capture …`
2. Click / press / drop …
3. Observe …

**Expected vs actual**
What you expected. What actually happened.

**Environment**
- Distro + version (e.g. Arch, CachyOS, Ubuntu 24.04):
- Compositor (X11 + WM name, or Wayland + compositor name):
- GPU + driver (output of `glxinfo | grep "OpenGL renderer"`):
- ShaderScope version (`shaderscope --version`):

**Logs**
If reproducible, set `SHADERSCOPE_LOG=debug` and `SHADERSCOPE_LOG_FILE=/tmp/ss.log`,
reproduce, then attach `/tmp/ss.log`. Otherwise paste the last ~50 lines of
stderr output here in a code block.

**Screenshots**
If visual, drag a screenshot into this issue.
