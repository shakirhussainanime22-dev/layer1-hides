# layer1-hide

## 1. What this module does

This module hides files and folders that reveal root from normal apps. When a normal app tries to open, check, or list a hidden path, the kernel tells it the file does not exist. The app sees a clean phone, even though root is still there underneath.

Root processes still see everything. That is on purpose. APatch itself, `su`, and root `adb` need the real files to work. Only processes with UID not equal to 0 get the filtered view.

## 2. Prerequisites

You need these before you start:

- APatch installed and working on your phone, with root already working.
- A phone that is already rooted with APatch.
- Termux from F-Droid, if you want to run shell checks on the phone. You do not need Termux to build, the build runs on GitHub.

You do not need a PC. The build runs in GitHub Actions.

## 3. How to fork/upload this repo to GitHub from a phone browser

1. On your phone, open github.com in your browser and sign in.
2. Create a new repository named `layer1-hide`. Leave it empty, do not add a README from GitHub.
3. On your phone, create the files from this project one by one. Tap Add file, then Create new file, paste the contents, and commit. Keep the exact paths: `src/layer1_hide.c`, `Makefile`, `Kbuild`, `.github/workflows/build.yml`, `README.md`, `CHANGELOG.md`, `LICENSE`.
4. Make sure the branch is `main`. If GitHub created `master`, rename it to `main` in Settings, Branches.
5. If you already have this repo on a friend's account, tap Fork on that repo page instead, then use your fork.

Tip: create `.github/workflows/build.yml` last, so the first build runs after all files are present. You can also push with the GitHub app if you prefer.

## 4. How to find and download the built .kpm from the Actions tab

1. Open your repo on github.com.
2. Tap the Actions tab at the top.
3. Tap the newest run named Build layer1-hide KPM. Wait until it shows a green check.
4. Scroll to the bottom, to Artifacts.
5. Tap `layer1-hide-kpm` to download the zip.
6. Unzip it on your phone. Inside you will find `layer1_hide.kpm`. That is the module file. You can rename it to `layer1-hide.kpm` if you like, they are the same file.

If the run is red, tap it, tap the failed step, and read the log. Most failures are a typo in a pasted file or a KernelPatch `main` change. See Troubleshooting below.

## 5. How to load it in APatch Manager

1. Copy `layer1_hide.kpm` to your phone, for example to `/sdcard/Download/`.
2. Open APatch Manager.
3. Go to the KPM tab.
4. Tap the `+` button.
5. Pick the `.kpm` file.
6. Choose Load for a temporary test, it will go away on reboot. Choose Embed for persistent, it stays after reboot.
7. If APatch asks for module args, leave it empty for a normal test. See section 7 for the `hide_list` args.

Do not load it twice. If it is already listed, unload the old one first.

## 6. How to verify it is loaded

Open Termux and run:

```sh
su -c dmesg | grep layer1-hide
```

You should see lines like:

```
[layer1-hide] init, version 1.0.0
[layer1-hide] loaded, hiding from uid != 0, extra=0, execve=0
```

If you see nothing, the module did not load, or `dmesg` is restricted on your ROM. Try `su -c "dmesg | grep layer1"` and check the APatch KPM list.

Quick functional check, as a normal app user, not root:

```sh
# As non-root, this should say No such file or directory if hidden:
ls /data/adb/
cat /data/adb/apd
ls /system/bin/su
```

As root, the same paths must still work:

```sh
su -c "ls /data/adb/ | head"
su -c "ls -l /system/bin/su"
```

If root cannot see them, unload the module at once, something is wrong.

## 7. How to add a new path to hide at runtime via the hide_list parameter

This module has a parameter called `hide_list`. It is a comma-separated list of extra substrings to hide. It is passed as KPM init args when you load the module.

Exact example: hide `/data/local/tmp/mycheat/` and anything containing `mytool`:

```
hide_list=/data/local/tmp/mycheat/,mytool
```

How to use it:

- If APatch Manager asks for args on load, paste the whole string above.
- If it does not ask, you can use the KPM control interface if your APatch build exposes it, with the same string: `hide_list=/data/local/tmp/mycheat/,mytool`.
- To also enable the optional `su` execve block, add a second arg: `hide_list=/data/local/tmp/mycheat/,mytool execve_hook=1`. Note: `execve_hook` is off by default. Only turn it on if you understand it blocks `su` execution for non-root callers.

Rules for `hide_list`:

- Separate items with a comma, no spaces around commas if you can avoid them.
- Each item is a substring match, not an exact path. `mytool` will hide `/data/app/mytool.apk`, `/sdcard/mytool/`, and any path containing `mytool`.
- Max 16 extra items, each max 63 characters. Longer items are truncated.
- To change the list, unload and reload with a new list. The built-in list in `src/layer1_hide.c` is always active.

## 8. Uninstall / rollback instructions

Temporary load:

1. Open APatch Manager, KPM tab.
2. Tap `layer1-hide`, tap Unload or Remove.
3. Reboot if APatch asks.

Embedded load:

1. Open APatch Manager, KPM tab.
2. Tap `layer1-hide`, tap Unembed / Remove from embedded list.
3. Reboot. The module must be gone after reboot. Check with `su -c dmesg | grep layer1-hide`, you should see no new load lines, and `ls /data/adb/` as a normal user should work again if it worked before.

If you are unsure, reboot to safe mode once, see section 9, then remove the module.

## 9. Troubleshooting

### Bootloop recovery

If the phone will not boot after embedding a bad KPM:

1. Power off fully.
2. Boot with APatch safe mode. On most phones, hold Volume Down during boot when you see the boot logo, until the system finishes booting. APatch safe mode disables all KPMs and modules for that boot.
3. Once booted, open APatch Manager, KPM tab, remove `layer1-hide`.
4. Reboot normally.

If safe mode does not work, use the APatch uninstall zip from recovery, or re-flash your patched boot image without the embedded KPM. Keep a backup of your working boot image before embedding any KPM.

### How to remove a bad KPM

- Safe mode boot, then remove from APatch Manager, as above.
- Or, from Termux with root, check `dmesg` for the error, then use APatch Manager to unload. Do not delete files in `/data/adb/` by hand unless you know what they are.

### Common errors

- `hook openat failed` in `dmesg`: your kernel / KernelPatch version changed the syscall hook API. The module rolled back and did not load. Do not force it. Check for a newer `layer1-hide` version.
- Build fails in GitHub Actions with `kpmodule.h: No such file`: the KernelPatch clone path is wrong. Check `build.yml` still clones to `../KernelPatch` and `KP_DIR` points there.
- Build fails with `hook_fargs5_t: unknown type`: your KernelPatch checkout renamed the hook arg types. Update the type names to match your checkout.
- `dmesg` shows nothing: `dmesg` may be blocked on your ROM, or the module did not load. Check the APatch KPM list first.
- Normal apps still see `/data/adb/`: check you are not running as root, check `hide_list` spelling, and check the module is actually loaded. Remember `getdents64` filtering is limited to 4096 byte buffers, see Limitations.

## 10. Honest limitations

This only hides filesystem paths from non-root processes. It does NOT hide process list, memory maps, or network traffic. Anti-cheat may still detect those.

Specifically, it does NOT hide:

- `/proc` process list, `ps` output, or task names.
- `/proc/self/maps`, `/proc/self/mountinfo`, or memory maps.
- Network sockets, DNS, or traffic.
- SELinux contexts, properties, or system properties that reveal root.
- Files opened via a direct file descriptor that was already opened before the module loaded.
- Paths accessed via a different spelling that does not contain any hidden substring, for example a bind mount or a symlink with a clean name.

If an app checks any of those, it can still detect root. This module is Layer 1, filesystem hiding only. Do not rely on it alone for games with strong anti-cheat.
