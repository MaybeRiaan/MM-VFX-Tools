# Modern Edge Blur — distributable build

`ModernEdgeBlur.aex` is the compiled plugin, ready to drop into After Effects
2026 (Windows x64) for testing.

## Install

1. Quit After Effects.
2. Copy `ModernEdgeBlur.aex` to:
   `C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\`
   (You may need admin rights or an `icacls` grant on the Plug-ins folder.)
3. Launch AE. The effect appears under **Effect → MM → Modern Edge Blur**.

A normal `..\build.ps1` deploys there for you — this folder exists for handing
a build to someone else.

## What you're testing

Edge blur: a soft Gaussian background plate composited under the original sharp
layer, with Blend Radius controlling how far the sharp layer's alpha is eroded
inward to reveal it. See [../docs/Architecture.md](../docs/Architecture.md) for
the pipeline and [../README.md](../README.md) for the parameter table.

## Binary version

The `.aex` here corresponds to whatever commit it was last committed alongside
— `git log -- ModernEdgeBlur/dist/ModernEdgeBlur.aex` shows its history. Always
rebuild from source if you need to be certain which version you are running;
`..\..\build.ps1 -Versions` at the suite root prints what the sources say.

---

*Superseded note, kept for anyone who read the previous version of this file:
it described testing "Phase 3 (Color Dilation) and Phase 4 (Edge Extend mode)"
and pointed at a `pre-dilation` tag for rollback. That work is no longer part of
Modern Edge Blur — `docs/Architecture.md` records the plugin as rebuilt from
scratch on 2026-06-04 with all prior EdgeExtend/EdgeRepair code removed. Edge
extension is now its own plugin, [../../EdgeRepair](../../EdgeRepair). The
`pre-dilation` tag still exists in history if that build is ever wanted.*
