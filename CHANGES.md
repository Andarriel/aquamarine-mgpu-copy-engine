# Every change made in this project, and why

All on 2026-09-05, one long session. Times are local (EEST). Nothing here is committed to git;
`aquamarine/` has the patch applied as uncommitted changes, `aquamarine-mgpu-copy-engine.patch`
is its `git diff`.

## 1. Timeline (what happened, in order)

1. 14:57 Cloned aquamarine v0.15.0 and Hyprland v0.56.2 (reference). Read the mGPU commit path.
2. Measured the live session (mGPU mode was active because the Intel-only env line was commented
   out): Hyprland 42–66% CPU, 43% user / 2% kernel, ~165 flips/s → not KMS overhead.
3. Wrote `tools/sampler.c` (perf_event_open, no `perf` installed) + `tools/symbolize.py`:
   56% `libnvidia-eglcore`, 27% `memcpy`. Found two 15.6 MB anonymous mappings = the
   `intermediateBuf` readback in `CDRMRenderer::blit`.
4. `tools/dmabuf_test.c`: NVIDIA GBM can't allocate linear (EINVAL) → aquamarine falls through to a
   tiled buffer Intel can't import → readback. Reverse direction (NVIDIA EGL importing Intel's
   buffer) works only as a read-only external texture.
5. `tools/vk_test.c` (Vulkan headers fetched from Khronos git): NVIDIA Vulkan imports Intel's
   linear dma-buf as TRANSFER_DST, imports its own GBM buffers if allocated with one of its
   Vulkan modifiers, sync_file semaphores both ways, copy 1.5 ms GPU / 0.05 ms CPU. Verified
   pixel-exact through Intel's CPU mapping.
6. Implemented the copy-engine path in aquamarine (see §2), built with a standalone CMake.
7. `tools/mgpu_e2e.cpp`: end-to-end test of the real library code with GL rendering + EGL fence.
8. **v1** built and staged (md5 `7ee35916…`). User installed it with `sudo cp` over the running
   library → live session died (SIGFPE, coredump PID 1218). After reboot, Hyprland hung at startup
   twice through SDDM/uwsm (40 s timeout, no crash report, kernel log clean, runtime log lost).
9. Reboot wiped the scratchpad (cmake, headers, build dir). Re-fetched into `deps/`, rebuilt in
   `build-aq/`.
10. **v2** (md5 `741b5248…`): hardened — no `vkDeviceWaitIdle`, bounded waits everywhere, first
    copy fence must be seen signalling within 500 ms, no fence into modeset commits,
    `AQ_MGPU_VK_DEBUG`, `AQ_MGPU_VK_SYNC`. Scripts `~/mgpu-1-install-v2.sh`, `~/mgpu-2-test.sh`,
    `~/mgpu-3-restore.sh` written so the user can install/test/restore from a TTY without copying
    text. Test from tty4: hung again; last trace line "creating copier for drm fd 19", main thread
    in `poll()`, other threads in futex.
11. Dead ends investigated: an unresponsive fake `WAYLAND_DISPLAY` socket (`tools/fake_wayland.py`)
    did not reproduce; `tools/hypr_like.c` (DRM master on both cards + EGL device-platform contexts
    + GBM devices + fake Wayland) did not reproduce either.
12. **v3** (md5 `f1127e60…`, then `a1ffbf7b…` after adding the layer disable): scrubs
    `DISPLAY`/`WAYLAND_DISPLAY` and sets `VK_LOADER_LAYERS_DISABLE=~implicit~` during init, logs
    each init step, watchdog in the test script sends SIGABRT first for a crash-report backtrace.
    16:18 test from tty4: **Hyprland started**, copier live, all frames + cursor through the copy
    engine, zero failures.
13. Measurements (`~/mgpu-4-measure.sh`, `~/mgpu-5-automeasure.sh`, an mpv 165 fps test-pattern
    client launched into the session via `WAYLAND_DISPLAY=wayland-1`): heavy load 40–44%
    (was 42–66% at *idle*); original idle-with-music scenario 10–19%, 10.7% user / 3.1% kernel.
14. Root cause of the hang proven: `tools/fake_x11.py` (accepting, never answering X socket on
    `:99`) + `tools/hypr_like.c` hangs in `vkCreateInstance`; `tools/connect_trace.c`
    (LD_PRELOAD `connect()` shim with backtraces) shows
    `libGLX_nvidia.so.0 → XOpenDisplay` called from `vk_icdNegotiateLoaderICDInterfaceVersion`.
    Disabling layers individually did not help; only hiding `DISPLAY` does. (The one earlier run
    where `~implicit~` seemed to help was a fluke.) The final build only changed a comment; md5
    stayed `a1ffbf7b…`, identical to what is installed.

## 2. Code changes in `aquamarine/` (v0.15.0 base), hunk by hunk

### `CMakeLists.txt`
* `target_link_libraries(... ${CMAKE_DL_LIBS})` — the copier `dlopen`s libvulkan.
* `check_include_file_cxx("vulkan/vulkan.h" AQ_HAS_VULKAN_HEADERS)` → defines
  `AQUAMARINE_HAS_VULKAN`; without headers the copier compiles as a stub that returns null.
  No link dependency on libvulkan (found at runtime; absence just disables the path).
  Local builds pass `-DCMAKE_CXX_FLAGS=-I$PWD/deps/vkh/include -DCMAKE_REQUIRED_INCLUDES=...`
  because `vulkan-headers` isn't installed system-wide.

### `src/backend/drm/VulkanCopy.hpp` / `VulkanCopy.cpp` (new, private headers under src/)
`CVulkanCopier` — one per primary-GPU `CDRMRenderer`, created lazily:
* `attempt(backend, drmFD)`: honours `AQ_MGPU_NO_VULKAN`; `debug` from `AQ_MGPU_VK_DEBUG`
  (mirrors every log line to stderr with `[aq-vk]`, because Hyprland's file logger is buffered
  and lost with the session).
* `init()`: RAII `CScrubDisplayEnv` unsets `DISPLAY`, `WAYLAND_DISPLAY` and sets
  `VK_LOADER_LAYERS_DISABLE=~implicit~` for the duration (restored after). **The DISPLAY unset is
  the fix for the startup deadlock**, the layer disable is hygiene. Then: dlopen
  `libvulkan.so.1`, `vkCreateInstance` (API 1.1), load instance functions, `pickPhysicalDevice`,
  `createDevice`, `createSlots`. Every step logs.
* `pickPhysicalDevice`: matches `VkPhysicalDeviceDrmPropertiesEXT` primary/render major:minor
  against `fstat(drmFD)`; requires `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`,
  `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_semaphore_fd`,
  `VK_EXT_queue_family_foreign`, and sync_file semaphore import+export.
* `createDevice`: prefers a transfer-only queue family (NVIDIA family 1 = copy engines, doesn't
  contend with the 3D queue), else any transfer/graphics/compute family. Command pool with
  RESET_COMMAND_BUFFER|TRANSIENT.
* Slots: 4 × {command buffer, fence, exportable signal semaphore, wait semaphore, last exported
  fd}. A slot is reused after `vkWaitForFences` on it (1 s bound); its previous sync_file fd is
  closed at reuse (the consumer must dup if it needs it longer).
* `vkFormat()`: fourcc → VkFormat for XRGB/ARGB8888, XBGR/ABGR8888, 2101010 variants,
  16-bit UNORM/SFLOAT, RGB565. Src and dst must map to the same VkFormat.
* `importableModifiers(fourcc, asSource)`: `vkGetPhysicalDeviceFormatProperties2` modifier list
  filtered by transfer feature bit and `vkGetPhysicalDeviceImageFormatProperties2` with
  DMA_BUF external handle + the modifier (must be IMPORTABLE). Cached per (fourcc, direction).
* `canCopy(from, to)`: both dmabuf, single plane, same size, same VkFormat, both modifiers
  importable in their role, copier healthy.
* `importBuffer(buf, asSource)`: creates a `VkImage` with
  `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` + explicit plane layout (stride/offset from the
  dmabuf attrs), imports the dup'ed fd with a dedicated allocation, binds, stores it as a
  `CVulkanImageAttachment` on the `IBuffer` (so the import happens once per buffer; the
  attachment destructor destroys the image via the copier if it still exists).
* `copy(from, to, waitFD)`: import both; pick slot; if `waitFD >= 0` dup it and import into the
  slot's wait semaphore (TEMPORARY sync_file import; on failure poll ≤100 ms instead); record:
  acquire barriers from `VK_QUEUE_FAMILY_FOREIGN_EXT` (src old layout GENERAL; dst UNDEFINED on
  first use, GENERAL after), `vkCmdCopyImage` full extent, release barriers back to FOREIGN;
  submit with the wait semaphore + signal semaphore + slot fence; export the signal semaphore as a
  sync_file (`vkGetSemaphoreFdKHR`). The very first copy in the process polls its fence (≤500 ms)
  and disables the path if it doesn't signal. Three consecutive failures → `broken`, permanent
  fallback to the GL blit. Debug: first 20 copies and every 600th are logged.
* `destroyImage`: `drainPending(1 s)` instead of `vkDeviceWaitIdle`; on timeout leaks the image
  and marks the copier broken (never block the compositor).
* Destructor: bounded drain, destroy slots/pool/device/instance, dlclose.

### `src/backend/drm/Renderer.hpp`
* `#include "VulkanCopy.hpp"`.
* `SBlitResult::explicitRequired` — tells callers the returned fence is mandatory (the copy
  engine does no implicit sync).
* `CDRMRenderer::ensureVulkanCopier()`, `vkCopier`, `vkCopierAttempted` (public, private header).

### `src/backend/drm/Renderer.cpp`
* `ensureVulkanCopier()`: one attempt per renderer.
* `blit()`: after the null/size checks and *before* taking the EGL context guard: if a
  `primaryRenderer` is given, ensure its copier and, if `canCopy`, `copy()` and return
  `{success, syncFD, explicitRequired = true}`; on failure log and fall through to the unchanged
  GL path (EGL import → readback fallback). `AQ_MGPU_VK_DEBUG` prints the decision.

### `src/allocator/GBM.cpp` (`CGBMBuffer` constructor)
* `#include <cstdio>`.
* Block A (primary side, `MULTIGPU` buffers = the output swapchain Hyprland renders into): ask the
  primary renderer's copier for importable *source* modifiers and intersect with the renderer's
  EGL-renderable, non-external modifiers → `vkSourceModifiers`. If non-empty, use them as the
  explicit modifier list (`gbm_bo_create_with_modifiers2`, RENDERING only). This replaces the
  "force linear" attempt that fails on NVIDIA and previously fell through to a tiled modifier
  Vulkan can't import (`0x300000000e08014`); NVIDIA now picks `0x300000000606014`.
* Block B (secondary side, non-cursor scanout buffers when the backend has a `primary` — i.e.
  the mgpu blit target): if the primary's copier can import LINEAR as destination and LINEAR is
  in the plane's format list, force `explicitModifiers = {LINEAR}`. Scanout flag stays set.
* Debug prints of every allocation decision under `AQ_MGPU_VK_DEBUG`.
* Everything else (linear fallback chain, clearBuffer) unchanged.

### `src/backend/drm/DRM.cpp`
* `#include <poll.h>`.
* `commitState()` (synchronous commit path): new local `blitFenceRequired`. After the blit, if
  `explicitRequired` and a fence exists: when the output supports explicit sync, `AQ_MGPU_NO_EXPLICIT`
  is unset, the commit is not a modeset (`NEEDS_RECONFIG`) and `AQ_MGPU_VK_SYNC` is unset → set
  `explicitInFence` and remember to OR `AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE` into `data.committed`
  (done after `data.committed = COMMITTED`). Otherwise poll the fence (≤200 ms, logs on timeout)
  and commit without a fence. The old branch (use the GL blit fence only if the client passed
  one) is kept for the GL path.
* `setCursor()`: captures the blit result; if the copy engine did it, poll its fence (≤100 ms)
  before importing the cursor FB, because cursor commits carry no fence.
* `prepareAsyncCommitData()` (async commit path) needed no change: it already requires a blit
  fence + explicit sync support, else falls back to the sync path. Hyprland 0.56.2 doesn't use
  async commits anyway.

Public headers (`include/`) are untouched; no ABI change. Verified: every exported symbol of the
distro build exists in the patched build; `NEEDED` differs only by an explicit `libGLESv2.so.2`.

## 3. Version history of the built library

| version | md5 | what |
|---|---|---|
| distro | `ca04f8f5a1723854d8db27637c0471ac` | CachyOS aquamarine 0.15.0-2.1 |
| v1 | `7ee359164d77006c14be934562ff175a` | first copy-engine build; hangs at Hyprland startup |
| v2 | `741b5248211b8be531d9d96bb640d55f` | bounded waits, fence proof, no fence in modesets, debug env; still hangs (DISPLAY) |
| v3 | `f1127e602ade93b1c01ec052fac77370` | + DISPLAY/WAYLAND_DISPLAY scrub, init step logs |
| v3 final | `a1ffbf7bf1a7a7c3426fb146be132c76` | + `VK_LOADER_LAYERS_DISABLE=~implicit~`; **installed** |

## 4. Files outside `aquamarine/`

* `tools/sampler.c`, `tools/symbolize.py` — perf_event_open CPU sampler + symbolizer via
  `/proc/<pid>/maps` and `nm` (works with `perf_event_paranoid=2`, Yama blocks ptrace).
* `tools/dmabuf_test.c` — GBM alloc + kernel PRIME + EGL import matrix between the two GPUs.
* `tools/vk_test.c` — Vulkan feasibility: modifier support, dma-buf import, copy timing, sync_file
  export/import. Args: `gfx` (use graphics queue), `vkmods` (allocate the NVIDIA bo with the
  Vulkan modifier list).
* `tools/mgpu_e2e.cpp` — end-to-end test of `CVulkanCopier` linked against `build-aq`.
  Args: iterations, heavy draw count. (With 0 heavy draws every frame "mismatches" by design.)
* `tools/hypr_like.c` — replica of the compositor process state for hang hunting
  (args: `master`, `current`, `plain`; uses a 25 s watchdog).
* `tools/fake_wayland.py`, `tools/fake_x11.py` — accepting-but-silent sockets.
* `tools/connect_trace.c` — LD_PRELOAD `connect()` shim printing backtraces.
* `tools/mgpu-1..5-*.sh` — copies of the `~/mgpu-*.sh` scripts (install / test from TTY /
  restore / measure inside Hyprland / VT-triggered measure from another TTY).
* `diag/` — every run's logs: `1608xx/` (v2 hang), `161840/` (v3 success), `measure-*/`,
  `auto-*/` (CPU measurements), `vk_*.txt`, `fakewl*.log`, `fakex*.log`, `samples-v3.txt`.
* `README.md`, `CLAUDE.md`, `CHANGES.md`, `IMPROVEMENTS.md` — docs.

## 5. System state changes (outside this directory)

* `/usr/lib/libaquamarine.so.0.15.0` replaced by the patched v3 (via temp file + `mv`);
  `/usr/lib/libaquamarine.so.0.15.0.orig` created (root-owned copy of the distro file).
* `~/mgpu-1-install-v2.sh`, `~/mgpu-2-test.sh`, `~/mgpu-3-restore.sh`, `~/mgpu-4-measure.sh`,
  `~/mgpu-5-automeasure.sh` created. `~/thing.sh` (user's one-liner) removed.
* `~/CLAUDE.md`: three changelog entries appended.
* Claude memory dir: `mgpu-copy-engine-fix.md`, `frieren-machine-tooling.md`, `MEMORY.md` index.
* No Hyprland/uwsm config files were modified. `~/.config/uwsm/env-hyprland` still has the
  Intel-only line commented out (that was the state when the session began).

## 6. Things that were tried and discarded

* Timer-based vblank simulation / skipping KMS commits (the user's original idea): not needed,
  commits only happen on damage and the cost was the pixel copy.
* Reverse GL blit (NVIDIA GL writing into Intel's buffer): NVIDIA reports linear as
  external-only → not renderable.
* `WAYLAND_DISPLAY` as the deadlock cause: not reproducible; it's `DISPLAY`.
* Implicit Vulkan layers as the deadlock cause: not reproducible individually.

## 7. Damage-limited copies and packaging (later the same day)

### What was added

* `packaging/PKGBUILD` + `~/mgpu-6-package.sh`: the patch as a real pacman package
  (`aquamarine-mgpu`, provides `aquamarine` and `libaquamarine.so=14-64`, conflicts with
  `aquamarine`), so an upgrade cannot silently restore the readback path. The build fails loudly
  if the Vulkan path compiled out. Verified by building the package and inspecting `.PKGINFO`.
* Damage-limited copying in `CVulkanCopier` (see README for the design) plus the plumbing to get
  the damage there: `CDRMRenderer::blit` gained a `const CRegion* damage` parameter, and both
  commit paths in `DRM.cpp` pass `STATE.damage` when `AQ_OUTPUT_STATE_DAMAGE` is committed. The
  cursor path passes nullptr (a 256×256 buffer is not worth tracking).
* `AQ_MGPU_VK_FULL_COPIES=1` (ignore damage) and `AQ_MGPU_VK_BARRIER=0|1|2` (barrier style) knobs.
* `tools/mgpu_damage.cpp`, the test that verifies the whole destination every frame.

### Two real bugs the damage test found

1. **Consecutive copies into the same image were unordered.** Vulkan gives no ordering between
   separate queue submissions, so a still-running earlier copy could land after a later one. It
   showed up as wrong content with a 1- or 2-buffer scanout swapchain. Fixed by remembering which
   slot last wrote each destination and waiting for that slot's fence before recording a new copy
   into it (`CVulkanImageAttachment::lastSlot` / `lastCopyID`).
2. **A destination whose pages its owner has never touched loses scattered pixels.** The first
   copies into a freshly created Intel buffer left ~0.5% of pixels untouched (they read back as
   freshly-allocated zeros), through both a CPU mapping and an Intel GL readback, and a second
   copy always fixed it. Touching the buffer from the Intel side first makes it perfect — which is
   exactly what aquamarine's GBM allocator already does for scanout buffers (`clearBuffer`), so
   the real path was never affected. Kept a belt-and-braces mitigation anyway:
   `CVulkanImageAttachment::fullCopiesLeft = 2` forces the first two copies into any new import to
   be full.

### Things that looked like bugs and were not

* An early version of the test reported mismatches because it assumed GL's scissor origin was
  flipped relative to the buffer's memory rows. It is not, for these EGLImage renderbuffers.
* CPU readback of the destination (`gbm_bo_map`) is not a reliable oracle: the NVIDIA copy engine
  writes that system-memory buffer over PCIe and the CPU sees those writes lazily, so a map right
  after the fence shows scattered stale lines. The test now reads the destination with the Intel
  GPU instead.
* `VK_ACCESS_HOST_READ_BIT` was added to the release barrier while chasing that; harmless and
  correct for a system-memory destination, so it was kept.

### Verification

* `tools/mgpu_damage`: 200/200 frames with 2 and with 3 destinations, 400/400 in the soak runs,
  and 150/150 with 1 and 4 destinations. Every frame checks all 4,096,000 pixels.
* `tools/mgpu_e2e`: 60/60 (rebuilt against the new signature).
* Exported-symbol check against the distro build: only `CDRMRenderer::blit`'s old mangled name
  disappears (internal header, and neither Hyprland nor hyprtoolkit references it).

## 8. Two install traps found when deploying for real

1. **A backup copy of a shared library must not live in a library directory.**
   `mgpu-1-install-v2.sh` had been keeping the distro build at
   `/usr/lib/libaquamarine.so.0.15.0.orig`. That file carries the same `SONAME`
   (`libaquamarine.so.14`), so when anything triggers `ldconfig` — installing a package, a
   reboot — the SONAME symlink can be repointed at the *backup*:
   `/usr/lib/libaquamarine.so.14 -> libaquamarine.so.0.15.0.orig`. The session then silently runs
   the unpatched library again, with the CPU readback path and all. Symptoms: high Hyprland CPU,
   four ~15.6 MB anonymous mappings, `libnvidia-eglcore` + libc dominating the profile, and
   `/proc/<pid>/maps` naming the `.orig` file. Fixed: the backup now lives in the project
   directory, `mgpu-3-restore.sh` deletes any stray `.orig`, and both scripts run `ldconfig` and
   print what `libaquamarine.so.14` resolves to. `~/mgpu-7-fix-install.sh` repairs a system that
   already got into this state.
2. **`makepkg -i --noconfirm` cannot replace a conflicting package.** `--noconfirm` answers the
   "aquamarine-mgpu and aquamarine are in conflict. Remove aquamarine? [y/N]" prompt with its
   default, which is *no*, so the transaction aborts and the package is built but never installed.
   `mgpu-6-package.sh` now builds with `makepkg -sf --noconfirm` and installs with a plain
   `sudo pacman -U`, so the prompt can actually be answered.
