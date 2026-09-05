# Can it be better? Ranked options (2026-09-05)

> Items 1 (packaging) and 2 (damage-limited copies) below have since been **done** — see
> `packaging/PKGBUILD` and CHANGES.md §7. They are kept here with their original reasoning,
> annotated with what was actually built. Items 3-8 are still open.

Where the numbers stand: same idle-with-music workload, Hyprland went from 42–66% to 10–19% CPU.
Of what remains, roughly half is `libnvidia-eglcore` (Hyprland's own GL rendering of the damaged
region every frame, blur included), ~15% libc (allocations, mutexes), ~20% Hyprland itself, ~3%
aquamarine. The copy engine costs ~0.1–0.2 ms CPU per frame, ~1.5 ms of GPU copy-engine time,
and adds that ~1.5 ms of latency between "frame rendered" and "frame on screen".

## 1. Make it survive `pacman -Syu` (low effort, do first) — DONE

Any `aquamarine` package update silently reinstalls the readback path. Options:
* **Best:** a local PKGBUILD (`aquamarine-mgpu`, `provides=aquamarine conflicts=aquamarine`,
  applies `aquamarine-mgpu-copy-engine.patch`, `makedepends+=vulkan-headers cmake`) installed
  with `pacman -U`. Upgrades then only happen when you rebuild it. Also fixes the "Hyprland
  built against 0.14.0 / system 0.15.0" mismatch line in crash reports (cosmetic).
* Quick alternative: a pacman hook (`/etc/pacman.d/hooks/`) that re-runs
  `~/mgpu-1-install-v2.sh` after any aquamarine transaction. Fragile if the ABI changes.
* Cheapest: `export LD_LIBRARY_PATH=$HOME/Experiments/mgpu/build` in `env-hyprland`. Works, but
  every app started from Hyprland inherits it.

## 2. Damage-limited copies (medium effort, real latency win) — DONE

Today every frame copies the full 2560×1600 (16 MB over PCIe, ~1.5 ms). Hyprland already hands
aquamarine the frame's damage (`AQ_OUTPUT_STATE_DAMAGE`, in buffer pixel coordinates, used for
`FB_DAMAGE_CLIPS`). Plan:
* Keep, per destination buffer (`CVulkanImageAttachment`), an accumulated `CRegion` "still
  stale". On every commit add the new damage to *all* destinations except the one being written;
  copy the chosen destination's accumulated region (as multiple `VkImageCopy` regions, merged
  rectangles), then clear it. Full copy when no damage was committed or the region is empty.
* Expected: the visualiser case copies a few hundred KB instead of 16 MB; the copy engine time
  and the added latency drop to ~0.1 ms. CPU stays the same.
* Risk: coordinate space mistakes → stale rectangles on screen. Validate with `mgpu_e2e`
  extended to partial damage, then `AQ_MGPU_VK_DEBUG` runs from a TTY.
* **Built as described**, with a per-destination `stale` region. `tools/mgpu_damage.cpp` is the
  validation. Two bugs found on the way (CHANGES.md §7). Remaining idea: instead of collapsing
  >64 rectangles to one bounding box, merge them adaptively (bounding box only when it is not
  much larger than the real area).

## 3. Trim the remaining Hyprland CPU (config, not code)

The 10–19% left is Hyprland re-rendering the visualiser region with blur 165 times per second.
Not aquamarine's business, but easy wins if wanted: lower the Caelestia visualiser frame rate or
turn it off when the media widget isn't visible; reduce `decoration:blur` passes/size; check
what damages the screen with `hyprctl` + `debug:damage_blink`. A truly static screen already
costs ~0% with the patch (VFR).

## 4. Robustness work before calling it "done" (medium)

Untested paths that should be exercised once each from a TTY with `AQ_MGPU_VK_DEBUG=1`:
* Normal SDDM/uwsm login (identical code path, but not re-run after v3).
* DPMS off/on and lid close/open: output disable → `releaseMgpuResources()` destroys the Intel
  swapchain → attachments destroyed (`drainPending` ≤1 s) → re-enable re-imports lazily.
* VT switch away and back (DRM master drop): the render node keeps working; check no
  `copy failed` lines and no `disabling the copy engine`.
* Suspend/resume with the NVIDIA GPU: a lost Vulkan device would show up as `vkQueueSubmit`
  errors → after 3 failures the code falls back to GL for the rest of the session. A
  "re-create the copier after N seconds" retry would be nicer.
* External monitor on the NVIDIA outputs (HDMI/DP-4 are on the dGPU): those outputs don't blit
  at all (primary GPU scans out directly) — unaffected, but worth one look.
* 10-bit / HDR output formats (`XRGB2101010`, `ABGR16161616F`): mapped in `vkFormat()` but never
  run. `canCopy` refuses anything unmapped, so worst case is the old path.

## 5. Copier startup off the hot path (small)

`vkCreateInstance` + device creation (~50–150 ms) currently happens during the first swapchain
allocation, i.e. inside Hyprland's first frame. Creating the copier when the primary renderer is
created (in `initMgpu()` when the backend has secondaries) would move that to startup where a
100 ms stall doesn't matter. A worker thread is possible but not worth the complexity.

## 6. Cursor through the copy engine: keep or simplify (small, either way)

Cursor images (256×256) also go through Vulkan now, with a ≤100 ms CPU poll because cursor
commits carry no fence. It works and costs microseconds; the alternative (leave cursors on the GL
readback path, 256 KB per cursor change) is simpler and equally fine. Decide when upstreaming.

## 7. Upstreaming (medium; the code is close)

Target: hyprwm/aquamarine. This is the NVIDIA↔Intel case mentioned in PR #161 (Vulkan
host-mapping blit for same-vendor dual GPU); the two can coexist (#161 copies through host memory,
this one copies device-to-device). Before opening a PR:
* `clang-format` with the repo's style; replace the `fprintf(stderr)` debug lines with
  `backend->log` under `AQ_TRACE`; keep `AQ_MGPU_NO_VULKAN`.
* Turn the header check into a CMake option (`-DWITH_VULKAN_COPY=ON/OFF`) plus `vulkan-headers`
  in the Nix/CI deps.
* Write up the `XOpenDisplay`-in-`vkCreateInstance` deadlock in the PR (and report it to NVIDIA;
  any compositor initialising Vulkan with `DISPLAY` set will hit it).
* Include the numbers from README.md and `tools/mgpu_e2e.cpp` as the test story.

## 8. Not worth pursuing (checked)

* Rendering directly into Intel's buffer from NVIDIA GL: linear is external-only on NVIDIA EGL.
  (NVIDIA *Vulkan* reports COLOR_ATTACHMENT support on the imported linear image, but Hyprland
  renders with GL, so that would need a whole Vulkan renderer in Hyprland.)
* Making Intel import NVIDIA memory: impossible without PCIe P2P into VRAM and NVIDIA's tiling.
* Skipping KMS commits / simulated vblanks: commits already only happen on damage.
* Reducing the readback path's cost (PBO, BGRA readback): irrelevant now that it isn't used.
