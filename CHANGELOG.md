# Changelog

## 1.0.0 - 2026-11-12

First release of `layer1-hide`.

### What is hidden

Substring match on path, only when caller UID is not 0. Root still sees the real filesystem.

- `/data/adb/`
- `/data/adb/modules`
- `/data/adb/ap/`
- `/data/adb/apd`
- `/system/bin/su`
- `/system/xbin/su`
- `/sbin/su`
- `magisk`
- `Magisk`
- `xposed`
- `Xposed`
- `riru`
- `zygisk`
- `busybox`
- `/sbin/.magisk/`
- `/debug_ramdisk/`
- `/data/local/tmp/GG/`
- `/data/local/tmp/gameguardian/`
- `/sdcard/GameGuardian/`
- `/data/media/0/GameGuardian/`
- `/data/data/com.sand.air/`
- `/data/data/catch_.me_.if_.you_.can_/`
- Plus up to 16 extra substrings via `hide_list=` init args.

### Syscalls hooked

- `__NR_openat` - before-hook, `args->ret = -ENOENT`, `args->skip_origin = 1` on match
- `__NR_openat2` - same, guarded with `#ifdef __NR_openat2`
- `__NR_faccessat` - same
- `__NR_faccessat2` - same, guarded with `#ifdef __NR_faccessat2`
- `__NR_newfstatat` - same
- `__NR_statx` - same
- `__NR_readlinkat` - same
- `__NR_getdents64` - after-hook, filters dirent entries whose `d_name` contains a hidden token, repacks buffer, adjusts return count. Fail-open if buffer > 4096 bytes or validation fails.
- `__NR_execve` - compiled in, off by default. Only installed when init args contain `execve_hook=1`. Returns `-ENOENT` when filename basename is `su` or `argv[0]` basename is `su` and caller UID != 0.

### Other

- UID filter: hide only when `current_uid() != 0`. Root sees real filesystem.
- Safe init: on any hook install failure, unhook everything already installed and return negative errno.
- Safe exit: unhooks all hooks.
- Runtime control: `KPM_CTL0` accepts `hide_list=a,b,c` and `status`.
- Build: `Makefile` with `KP_DIR` and `TARGET_COMPILE`, GitHub Actions on `ubuntu-latest` producing `layer1_hide.kpm`.
- `getdents64` limit documented: buffers larger than 4096 bytes are returned unfiltered to avoid corruption.
