# ~/Experiments/mgpu — orientation for a new Claude session

This directory is a finished-but-ongoing experiment: fixing the CPU cost of Hyprland's
NVIDIA-render + Intel-scanout (Optimus) mode by patching aquamarine. Read this file first, then:

* `README.md`      — the investigation, evidence, measurements, current machine state.
* `CHANGES.md`     — every change made (code hunks, scripts, system files), with the why.
* `IMPROVEMENTS.md`— what could be done next, ranked, with risks.
* `~/CLAUDE.md`    — machine-wide notes; its session changelog has the timeline of this work.

## State as of 2026-09-05 (end of the day)

* `build/libaquamarine.so.0.15.0` is the current build (**v4**, damage-limited copies,
  md5 `9479babadd07f14a86e932a875f2ccce`). `/usr/lib/libaquamarine.so.0.15.0` still holds **v3**
  (`a1ffbf7bf1a7a7c3426fb146be132c76`, full copies) until v4 is installed — install it with
  `sudo bash ~/mgpu-6-package.sh` (builds the `aquamarine-mgpu` pacman package) and then test from
  a TTY with `bash ~/mgpu-2-test.sh`. Distro copy: `/usr/lib/libaquamarine.so.0.15.0.orig` and
  `build/libaquamarine.so.0.15.0.orig` (md5 `ca04f8f5a1723854d8db27637c0471ac`).
* Verified working when Hyprland is started from a TTY (`~/mgpu-2-test.sh`). Normal SDDM/uwsm
  login had not been re-tested at the time of writing, but nothing in that path differs.
* Result on the original workload: Hyprland 42–66% CPU → 10–19%.
* `~/.config/uwsm/env-hyprland`: the Intel-only `AQ_DRM_DEVICES` line is commented out
  (= multi-GPU mode, NVIDIA primary). No env knobs are set; debug tracing is off.
* A pacman upgrade of `aquamarine` will overwrite the patched lib (see IMPROVEMENTS.md "packaging").

## Hard rules learned here

1. **Never `cp` over a shared library a running process has mapped.** It corrupted the live
   Hyprland (SIGFPE). Always `cp X X.new && mv X.new X` (`~/mgpu-1-install-v2.sh` does this).
2. **Any Vulkan instance created inside the compositor must hide `$DISPLAY`/`$WAYLAND_DISPLAY`
   first.** NVIDIA's ICD (`libGLX_nvidia.so.0`) calls `XOpenDisplay` inside `vkCreateInstance`
   and deadlocks on Hyprland's own lazily-served Xwayland socket. `CVulkanCopier::init` does this.
3. Sudo needs a real terminal; the user runs the `~/mgpu-*.sh` scripts by hand. Write scripts
   that log to `diag/` instead of asking the user to copy text (they work from a text TTY).
4. Test risky changes by starting Hyprland from a TTY with `~/mgpu-2-test.sh` (watchdog + logs),
   never through the SDDM login loop.
5. The tool shell here is zsh (not fish); `grep` is a ugrep wrapper — use `/usr/bin/grep -E`.
   No `perf`, `cmake`, `strace`, `vulkan-headers` packages: use `deps/cmake`, `deps/vkh`,
   `tools/sampler.c`, `tools/connect_trace.c`.

## Layout

```
aquamarine/     git clone of hyprwm/aquamarine at v0.15.0 with the patch applied (uncommitted)
hyprland/       shallow clone of Hyprland v0.56.2, reference only (never built)
aquamarine-mgpu-copy-engine.patch   `git diff` of aquamarine/ (regenerate after edits, see CHANGES.md)
build-aq/       ninja build dir of the patched aquamarine
build/          staged libaquamarine.so.0.15.0 (patched) + .orig (distro)
deps/           cmake 3.31.8 binary tarball, Khronos Vulkan-Headers clone
tools/          all test/diagnostic programs + copies of the ~/mgpu-*.sh scripts
diag/           logs and measurements from the live runs (timestamped dirs)
```

## How to rebuild and redeploy

```
export PATH=$HOME/Experiments/mgpu/deps/cmake/bin:$PATH
ninja -C ~/Experiments/mgpu/build-aq && cp ~/Experiments/mgpu/build-aq/libaquamarine.so.0.15.0 ~/Experiments/mgpu/build/
cd ~/Experiments/mgpu/aquamarine && git add -N src/backend/drm/VulkanCopy.* && git diff > ../aquamarine-mgpu-copy-engine.patch
```
Then update the `expected` md5 line in `~/mgpu-1-install-v2.sh`, and the user runs
`sudo bash ~/mgpu-1-install-v2.sh` followed by `bash ~/mgpu-2-test.sh` from a TTY.
Before any deploy, run both `tools/mgpu_e2e` and `tools/mgpu_damage 200 3` (build lines in
README "Rebuilding"); both must report N/N frames correct.

## Where to look when something is wrong

* Copier decisions: start Hyprland with `AQ_MGPU_VK_DEBUG=1` → `[aq-vk]` lines on stderr
  (`diag/<time>/hypr-stdout.log` when using `~/mgpu-2-test.sh`). Copy lines report how much of the
  frame was transferred.
* Stale rectangles on screen would mean the damage tracking is wrong: `AQ_MGPU_VK_FULL_COPIES=1`
  goes back to whole-frame copies, and `tools/mgpu_damage` is the test that covers it.
* `AQ_MGPU_NO_VULKAN=1` → old GL/readback path (slow but known-good).
* `AQ_MGPU_VK_SYNC=1` → copies still on the GPU but the CPU waits for them instead of KMS.
* Profile: `tools/sampler <pid> 8 0 > s.txt; python3 tools/symbolize.py <pid> s.txt self`.
* The readback path is active again if Hyprland has two ~15.6 MB anonymous mappings and
  `libnvidia-eglcore` + `memcpy` dominate the profile.
