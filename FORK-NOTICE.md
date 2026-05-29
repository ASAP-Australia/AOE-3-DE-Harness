# Fork Notice

This repository is a downstream fork of [ValveSoftware/gamescope](https://github.com/ValveSoftware/gamescope), customised for use as the input + display layer of the [AoE 3 DE A New World](https://github.com/ASAP-Australia/AOE-3-DE-A-New-World) AI harness.

## Attribution

The original gamescope is © Valve Corporation and contributors, released under the BSD 2-Clause License (see `LICENSE`). All upstream copyright notices are preserved unmodified.

## Purpose of this fork

Stock gamescope is a general-purpose nested compositor. This fork adds harness-specific surfaces:

- A Unix-domain control socket for in-process screenshot + input commands from a Python harness
- Composite-framebuffer pixel readback (replacing the per-process DXGI hook)
- libei-based input injection routed from the control socket

Stock gamescope behaviour is preserved when these flags are not used.

## Upstream sync policy

The `upstream` remote points to `ValveSoftware/gamescope`. Rebases against upstream `master` are expected periodically. Harness-specific changes live in `harness/` (to be added) and modified core files are kept minimal to ease rebases.

## License

Modifications in this fork are released under the same BSD 2-Clause License as the upstream project.
