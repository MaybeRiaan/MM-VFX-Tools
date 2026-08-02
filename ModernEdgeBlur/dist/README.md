# Modern Edge Blur — distributable build

`ModernEdgeBlur.aex` is the compiled plugin, ready to drop into After Effects 2026 (Windows x64) for testing.

## Install

1. Quit After Effects.
2. Copy `ModernEdgeBlur.aex` to:
   `C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\`
   (You may need admin rights or an `icacls` grant on the Plug-ins folder.)
3. Launch AE. The effect appears under **Effect → MM → Modern Edge Blur**.

## What you're testing

Phase 3 (Color Dilation) and Phase 4 (Edge Extend mode), with the dilation+compose fixes from the current commit. See [../NEXT_STEPS.md](../NEXT_STEPS.md) and [../Architecture.md](../Architecture.md) for the parameter model and intended behaviour.

## Rollback to last working edge-blur-only build

The commit tagged `pre-dilation` is the last known-good build before any dilation work began (edge blur and erosion only). To check out that version:

```
git checkout pre-dilation
```

Then rebuild from source via the instructions in the root [README.md](../README.md).

## Binary version

The `.aex` here corresponds to whatever commit it was last committed alongside — `git log dist/ModernEdgeBlur.aex` will show its history. Always rebuild from source if you need to be certain.
