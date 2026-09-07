# my-driver

Android AArch64 loadable kernel module. Compile-time gated subsystems: process memory R/W, per-thread hardware breakpoints and watchpoints, PTE inline hooks, touch injection, sensor spoofing, page-fault harvest, and four concealment layers. Userspace fd is obtained via a magic `reboot()` handshake — no `/dev` node.

Defaults: `HIDE_SELF_MODULE=1`, `HIDE_VMAP=1`, `HIDE_TASK=1`, `HIDE_KGSL_STRENGTH=0`.

## Build matrix

CI builds seven KMIs plus the arm64-v8a userspace client:

- `android12-5.10`
- `android13-5.10`
- `android13-5.15`
- `android14-5.15`
- `android14-6.1`
- `android15-6.6`
- `android16-6.12`

Runtime coverage today is `android15-6.6` (NP05J / Android 15 / kernel 6.6.56-android15-8). Other legs are compile-validated.

## Build

Kernel code builds inside the matching `ghcr.io/ylarod/ddk:<kmi>-<release>` image (default release `20251104`, required by 6.12 KCFI):

```bash
cd driver
make                                             # defaults
make HIDE_SELF_MODULE=1 HIDE_VMAP=1 HIDE_TASK=1  # everything on except KGSL
make HIDE_KGSL_STRENGTH=3                        # KGSL retro + proactive
```

## Userspace client

```bash
cmake -S client -B out/client -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
cmake --build out/client
```

`Driver::open()` performs the handshake and negotiates HWBP caps. On `EOPNOTSUPP`/`ENOTTY` the fd stays open with `hwbpAvailable() == false` (memory/touch/sensor still work). On ABI-generation or size mismatch, `open()` fails with `EPROTO`. Sub-APIs: `driver.memory`, `driver.touch`, `driver.gyro`, `driver.hwbp`, `driver.pteHook`, `driver.hidePid`, `driver.hideName`.

## Concealment

Four independent hide surfaces, each compile-gated.

### `HIDE_SELF_MODULE`

`lifecycle.c::conceal_module()` + `module_hide.c`. Renames `mod->name` to `KCFG_DECOY_NAME` (default `iptable_filter`) before `list_del(&mod->list)`, then `kobject_del(&mod->mkobj.kobj)` and `list_del(&mod->mkobj.kobj.entry)`. Zeroes `taints`, `srcversion`, `notes_attrs`, `modinfo_attrs`, `build_id`. Arms a kprobe on `m_show`/`s_show` so a seq_file iterator that reinstates the module still skips it silently. Applied once at `init_driver`.

### `HIDE_VMAP`

`lifecycle.c::conceal_vmap()`. Uses `&drv` (core `.bss`) as the probe address so `__init` freeing stays consistent. `list_del()`s from `vmap_area_list`, and where the single-root form exists (< 6.9) also `rb_erase()`s from `vmap_area_root`. Falls back to no-op if symbols are not in kallsyms.

### `HIDE_TASK`

`dirent_hide.c`. One kprobe on `filldir64` services two hide sets — every `iterate_dir` path funnels through this callback (proc, ext4, f2fs, tmpfs, overlayfs, sysfs):

- Numeric `/proc/<pid>` entries (up to `DIRENT_HIDE_MAX_PIDS = 8`).
- Exact-basename file/dir entries (16 slots × 63 chars).

Return contract is version-gated: 6.1+ `filldir_t` returns `bool` (skip = `true = 1`), 5.10..6.0 returns `int` (skip = `0`). Advances PC to `LR` on the target's dispatch frame to bypass the original `filldir64`.

Client APIs `HidePid` / `HideName`; ioctls `0x50..0x56`.

### `HIDE_KGSL_STRENGTH`

`stealth.c`. Values:

- `0` — off. `DRV_CMD_HIDE_KGSL` returns `-EOPNOTSUPP`.
- `1` — retroactive. `hide_kgsl_by_pid()` walks both `kgsl_process_private` rbtrees on `kgsl_driver` and `rb_erase`s the entry whose PID string matches. `holder_ptr_looks_valid()` guards against BSP offset drift.
- `2` — proactive. Three kprobes: `kgsl_process_init_sysfs`, `kgsl_process_init_debugfs`, `sysfs_create_group`. All pre-check `dirent_hide_pid_contains(current->tgid)` and, for hidden PIDs, spoof `-ENOMEM` before the original runs. `sysfs_create_group` walks the kobject parent chain (depth 7) and only fires when a `"kgsl"` name is in the ancestry.
- `3` — both.

Kbuild enforces `STRENGTH >= 2 → HIDE_TASK=1` (proactive layer reads the shared hidden-PID list). Per-kernel offsets for `kgsl_driver` and `kgsl_process_private` are version-gated at compile time.

## Memory API — `driver.memory`

`Read`/`Write` chunked at `DRV_MEM_CMD_MAX_SIZE = 16 MiB` per ioctl. Linear-map (`_LINEAR`) and vmap (`_VMAP`) variants. Fast pagewalk from module init: `aarch64_insn_patch_text_nosync` when available (FIX_TEXT_POKE0 + IS TLBI) with legacy PTE-flip fallback on stripped kernels. Additional helpers:

- `getModuleBase(name)`, `getTls()`, `readVmaCookie(addr)`, `dumpVmas()`.
- `multiRead(addrs)` — batched `pin_user_pages` + single mm-lock, 8–8× speedup at 128–512 pointers.
- `findTaskByComm(comm)`, `findPidByPackage(package)` — exact full `argv[0]` match via `get_cmdline`.

Ioctls: `0x0B..0x17`, all sharing the 40-byte `drv_ioctl_req` (`pid, addr, buf, size, extra`).

## HWBP — `driver.hwbp`

Per-thread AArch64 hardware breakpoints and watchpoints. Types: `X` (execute, 4B only), `R`/`W`/`RW` (watchpoint, 1/2/4/8B). Register overrides mutate X0..X30, PC, and V0..V31 low/high before the target instruction retires. Trackers keyed on `(pid, addr, owner_file)` — two fds on the same address get independent state. Closing the fd sweeps only its own trackers. Install detects `mm` change (after `exec()`) and drops the stale tracker before the reuse branch. Re-install on an orphaned tracker resurrects it in place through `modify_user_hw_breakpoint(disabled=0)`, preserving every sticky setter's state (`notify_pid_ref`, `sample_every`, `condition`, `bypass_pid`).

Watchpoints are one-shot: the overflow handler queues a deferred `modify_user_hw_breakpoint(disabled=1)` since the arm64 hw-bp core does not auto-single-step a non-default overflow handler. Client re-arms by calling `install()` on the same `(pid, addr)`.

### Flags (install-time bitmask)

| Flag | Bit | Effect |
| --- | ---: | --- |
| `DRV_HWBP_FLAG_BAIT_GUARD` | 0x1 | Deprecated. Accepted for source compat, silently dropped. Absent from `caps.flags_supported`. |
| `DRV_HWBP_FLAG_NOTIFY` | 0x2 | Deliver `signal_no` (default 43, past Bionic's 32..34/40/41) to `notify_pid` on hit; `si_int` carries `tracker_id`. Picks the first thread in the tgid whose blocked mask does not contain `sig`; falls back to the group leader (signal stays pending until unblocked). |
| `DRV_HWBP_FLAG_CAPTURE_FP` | 0x4 | Fill `q_lo/q_hi/fpsr/fpcr` in every hit record from `current->thread.uw.fpsimd_state`. |
| `DRV_HWBP_FLAG_TIMING_BYPASS` | 0x8 | Skip ring push + notify on hit; register overrides still applied. Reduces observable per-hit overhead. |

### Gates (per-tracker, set after install)

- `SET_SAMPLE(every=N)` — only every Nth hit fires. 0 disables.
- `SET_CONDITION({reg, op, value})` — hit filtered unless `regs->X[reg] op value` matches. `op ∈ {NONE, EQ, NE, LT, LE, GT, GE}`. Publisher writes the whole tuple under `override_lock`; the handler snapshots it under the same lock.
- `SET_BYPASS_PID(pid)` — one-shot. The next hit whose `current->tgid == pid` is silently consumed. `cmpxchg` clears the flag.
- `SET_NOTIFY({notify_pid, signal_no})` — mutable signal target. `notify_pid_ref` swap is done under `notify_lock`; the deferred worker cannot race a `put_pid()`.
- `TRANSLATE_BAIT(addr)` — returns the largest same-basename VMA cluster address for `addr` (does not install). Useful for probing AC mapping layouts.

### Ioctls

Primary range `0x40..0x47`: `INSTALL`, `REMOVE`, `SET_OVERRIDE`, `GET_HITS_LEGACY` (returns `-EPROTO`), `CLEAR_ALL`, `GET_CAPS`, `SET_SAMPLE`, `SET_CONDITION`.

Extended range `0x60..0x63`: `SET_BYPASS_PID`, `SET_NOTIFY`, `TRANSLATE_BAIT`, `GET_HITS` (800-byte record). GET_HITS lives in the extended range because 0x48 belongs to `PTE_HOOK_INSTALL`. `_Static_assert` in `comm.c` breaks the build if any two command ranges overlap.

### CAPS negotiation

`GET_CAPS` returns 32 bytes: `{num_brps, num_wrps, ring_slots, max_overrides, hit_bytes, install_req_bytes, flags_supported, fp_ready}`. `flags_supported`'s low 24 bits are `DRV_HWBP_FLAG_*`; high 8 bits are `DRV_HWBP_ABI_GENERATION` (currently 2). Client's `open()` fails with `EPROTO` unless the generation matches and every sizeof matches — a growing caps struct under the same ioctl number would overflow an old client's stack buffer.

Handshake is deduplicated: `reboot_handler_pre` uses a global-spinlock `(last_pid, last_reply, jiffies)` window to drop the extra `__arm64_sys_reboot` hits produced by the arm64 syscall wrapper (previously produced 2–3 fd installs per `open()`).

## PTE inline hooks — `driver.pteHook`

`user_hook.c`. Installs a 32-byte constant-return stub in a private executable mapping. `returnConst<T>()` supports integral, enum, pointer, float, double; `returnVoid()` emits only a return. Stub starts with a BTI-compatible landing, validates a full same-page private VMA, and records expected patched bytes. Reinstall, remove, and `clearAll` refuse to overwrite a changed function.

Ioctls: `INSTALL=0x48`, `REMOVE=0x49`, `CLEAR_ALL=0x4A`. Kinds: `CONST_U64=0`, reserved `TRAMPOLINE=1`, `CONST_FLOAT=2`, `CONST_DOUBLE=3`, `VOID_RET=4`.

## Touch injection — `driver.touch`

`input_synth.c`. Multi-touch B protocol via lazy `vfs_read`/`vfs_write` kprobes on `/dev/input/event*`. `down/move/up` take slot + coordinates. Same allocation pool handles both live evdev traffic and injected events.

Ioctls: `TOUCH_DOWN=0x12D`, `TOUCH_UP=0x12E`, `TOUCH_MOVE=0x12F`, legacy slot `TOUCH_SLOT_LEGACY=0x136`. Full range `0x12D..0x18F` triggers lazy initialization.

## Sensor uprobe — `driver.gyro`

`sensor.c`. HIDL v1 and AIDL v1 event layouts. `bind(probeOffset, layout)` installs a `uprobe_consumer` on the libsensorservice event-convert function; subsequent writes update the shared gyro X/Y state and an enable flag. `bindAuto()` picks the offset from the AIDL/HIDL symbol pair.

Ioctl: `SENSOR_BIND=0x140` (`pid == 100` binds, else updates values).

## Harvest — `driver.harvest`

`harvest.c`. Pre-handlers on `do_mem_abort` + `arm64_force_sig_fault` catch faults inside the target's `.text` and swap `SEGV → SIGSTOP` or record ESR bank slots. Enabled through `installHooks()` and `tearDown()`; `TARGET_PKG` selects the harvest scope.

Ioctls: `INSTALL_HOOKS=0xD1`, `TEAR_DOWN=0xD2`, `INSTALL_SIGSEGV_SUPPRESS=0xD5`.

## Deploy

CI publishes `my-driver-<kmi>.ko` per KMI plus the `userspace-arm64-v8a` bundle (`my-driver-client`, `my-driver-test`). Pick the artefact for the target device's KMI, `adb push` alongside the client binary, `insmod` as root.

## Diagnostics

`diagnostics/capture-kmsg.sh` — capture `/dev/kmsg` on the device (root) filtered for the driver's log prefix. Untracked binary dumps in the same directory are gitignored.

## Autonomous test binary — `my-driver-test`

22 self-target scenarios; process exits non-zero on any FAIL. All hwbp tests short-circuit through `require_hwbp()` when `hwbpAvailable()` is false. Coverage:

- Handshake + module concealment surfaces (`/proc/modules`, `/sys/module`, `/proc/vmallocinfo`).
- HWBP CAPS + install matrix (R/W/RW/X × valid lens + rejects) + bypass/sample/condition gates + async signal delivery + FPSIMD capture + translate_bait + timing (base vs HWBP vs `TIMING_BYPASS`).
- `filldir64` file/dir hide + PID hide.
- fd-scoped cleanup (exhaust every HW slot from an alt fd, close, verify primary reinstalls every slot) + fd owner isolation (two clients on same `(pid, addr)`, alt's `clearAll` drops only alt).
- Watchpoint one-shot fire + auto-disable (first store records, second must not until re-install).
- Legacy `GET_HITS=0x43` returns `EPROTO`; deprecated `BAIT_GUARD` flag absent from `caps.flags_supported`; `PTE_HOOK_INSTALL` reaches PTE handler (not HWBP after 0x48 relocation).
- `GET_CAPS` doesn't overflow a 32-byte buffer with canary; `install()` after fork+exec doesn't reuse the parent's stale tracker.

Current runtime score on NP05J: **27 PASS / 0 FAIL / 1 SKIP** (S22 skips when the child's synthetic VA doesn't map; code path exercised in review). Stress: 15/15 back-to-back clean, 0 memory-driver warnings.

## Benchmark snapshot

NP05J / Android 15 / kernel 6.6.56. µs per operation, mean.

### Memory read

| Size | Driver | `process_vm_readv` | `/proc/pid/mem` |
| --- | ---: | ---: | ---: |
| 4 B | 0.699 | 1.162 | 1.518 |
| 1 KiB | 0.809 | 1.298 | 1.615 |
| 64 KiB | 12.293 | 15.762 | 23.481 |
| 1 MiB | 188.905 | 225.284 | 362.884 |
| 4 MiB | 796.633 | 815.044 | 1470.967 |

### Memory write

| Size | Driver | `process_vm_writev` |
| --- | ---: | ---: |
| 4 B | 0.710 | 1.166 |
| 4 KiB | 1.047 | 1.521 |
| 64 KiB | 12.651 | 15.875 |
| 1 MiB | 191.251 | 227.859 |

### MULTI_READ

| Entries | N × READ | MULTI_READ | Speedup |
| ---: | ---: | ---: | ---: |
| 8 | 5.913 | 1.819 | 3.25× |
| 128 | 92.547 | 12.573 | 7.36× |
| 512 | 373.199 | 46.071 | 8.10× |

### Hook ioctl latency

| Operation | p50 | p95 | p99 |
| --- | ---: | ---: | ---: |
| PTE install/update | 4.063 | 4.219 | 5.156 |
| HWBP install | 9.688 | 13.438 | 2448.438 |
| HWBP SET_OVERRIDE | 0.834 | 0.938 | 0.938 |

### HWBP CAPS on device

`num_brps=6`, `num_wrps=4`, `ring_slots=32`, `max_overrides=10`, `hit_bytes=800`, `install_req_bytes=192`, `fp_ready=1`. `flags_supported low = 0xE` (NOTIFY | CAPTURE_FP | TIMING_BYPASS), `abi_generation = 2`.

## Layout

```text
driver/include/driver/uapi.h  shared kernel/userspace ABI
driver/src/lifecycle.c        module init + conceal_module + conceal_vmap
driver/src/comm.c             ioctl router + reboot()-magic handshake + dedup
driver/src/memory.c           pagewalk + process-memory R/W + multi_read
driver/src/hwbp.c             hardware breakpoints/watchpoints subsystem
driver/src/user_hook.c        constant-return user-code hooks
driver/src/dirent_hide.c      filldir64 kprobe → PID + name hider
driver/src/module_hide.c      m_show/s_show scrub kprobe
driver/src/stealth.c          KGSL retroactive + proactive concealment
driver/src/sensor.c           HIDL/AIDL sensor uprobe
driver/src/input_synth.c      touch injection
driver/src/harvest.c          fault harvest + SIGSEGV suppress
driver/src/log.h              LOGE/LOGW/LOGN/LOGI/LOGD macros
client/src/Driver.{h,cpp}     userspace API
client/tests/test_all.cpp     autonomous 22-scenario test binary
diagnostics/capture-kmsg.sh   on-device kmsg capture (root)
.github/workflows/build.yml   seven-KMI kernel + userspace CI
```

## Configurable knobs

```bash
make DRIVER_NAME=my-driver \
     TARGET_PKG='"cent.tmgp.sgame"' \
     HIDE_SELF_MODULE=1 HIDE_VMAP=1 HIDE_TASK=1 HIDE_KGSL_STRENGTH=0
```

`DRIVER_NAME` controls the module filename. `TARGET_PKG` selects the harvest package string. `REBOOT_MAGIC` controls the handshake magic pair. `KCFG_DECOY_NAME` sets the module rename target.

## Caveats

No unload path — kprobes, task_work callbacks, and hook stubs can retain module pointers. Reboot before replacing a loaded artefact. Memory path does not pin pages, so migration and COW races remain. `HIDE_KGSL_STRENGTH >= 1` uses per-BSP offsets — enable only after runtime sanity checks pass. `translate_bait` is suggest-only: the largest-cluster heuristic does not prove instruction identity.

## License

GPL-2.0-only. See [LICENSE](LICENSE), [NOTICE.md](NOTICE.md), [CONTRIBUTING.md](CONTRIBUTING.md).
