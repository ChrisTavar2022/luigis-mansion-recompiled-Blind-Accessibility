# Third-party code

| Path | Source | License |
|------|--------|---------|
| `decomp/` | [SomeJakeGuy/lm-decomp](https://github.com/SomeJakeGuy/lm-decomp) @ `c347326b12c38281c5528d36bda6334997fa8d3c` | CC0 1.0 (`decomp/LICENSE`) |
| `libs/JSystem/`, `include/global.h`, `include/helpers/`, `src/port/os/OS{Thread,Mutex,Context}.cpp` | [TwilitRealm/dusklight](https://github.com/TwilitRealm/dusklight) @ `2c4960dcdfaedf9b0c96e2410a1cca2bc1657131` (adapted) | CC0 1.0 |
| `extern/aurora/` | [encounter/aurora](https://github.com/encounter/aurora) (submodule) | MIT |
| `llvm/bin/clang.exe`, `llvm/bin/lld-link.exe` (release packages only, copied at install time; not in this repository) | [LLVM Project](https://llvm.org) | Apache-2.0 WITH LLVM-exception (`packaging/LLVM-LICENSE.TXT`, installed as `llvm/LLVM-LICENSE.TXT`) |

`docs/accessibility/template/` contains files from the Accessibility Mod Template 1.0.1,
Copyright (c) 2026 Happy Starfish, licensed under CC BY 4.0
(https://creativecommons.org/licenses/by/4.0/). The copies are unmodified, apart from
`CLAUDE.md` and `README.md` being renamed `TEMPLATE_CLAUDE.md` and `TEMPLATE_README.md`.
The rest of `docs/accessibility/` and the port's accessibility code are adapted from them.

`extern/tolk/` contains Tolk (https://github.com/ndarilek/tolk, LGPL 3.0, `LICENSE.txt`) and
NVDA's controller client (`LICENSE-NVDA.txt`).

lm-decomp does not accept AI-assisted contributions. Code in this repository that was
written with AI assistance must not be submitted upstream to lm-decomp.

This project contains no Nintendo assets or code from the retail executable.
Users must provide their own disc dump.
