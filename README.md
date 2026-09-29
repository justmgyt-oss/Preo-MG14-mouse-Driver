# Preo MG14 Driver Manager

A settings manager for the **Preo MG14** gaming mouse — the wired 3200 DPI
six-button mouse sold under the Preo / Teknosa brand.

It detects the mouse, shows exactly which driver Windows has bound to it, can
repair that binding if it breaks, and exposes the pointer and button settings
Windows actually offers for a HID mouse.

**This is not a kernel driver, and it does not install one.** Read
[Why not a kernel driver](#why-not-a-kernel-driver) below — that is the single
most important thing to understand about this project.

---

## Quick start

1. Copy `PreoMG14Driver.exe` anywhere you like. There is no installer, and it
   does not write itself anywhere.
2. Double-click it. You do **not** need to run it as administrator to look
   around or change pointer settings.
3. Open the **Device** tab. If the mouse is plugged in it will say
   `MG14 detected - 30FA:0400`.
4. If the mouse is misbehaving, press **Restart driver** and accept the UAC
   prompt. That is the only action that needs administrator rights.

To remove it completely: delete the `.exe`, then delete
`%LOCALAPPDATA%\PreoMG14Driver`. See [Uninstall](#uninstall).

### If something is wrong, start here

```cmd
PreoMG14Driver.exe --diagnose
```

That prints a plain text report: your real Windows build (read via
`RtlGetVersion`, so the manifest cannot lie about it), whether the MG14 was
matched, the full driver details if it was, the complete list of every HID
collection Windows is reporting with your MG14 marked, and the current
Windows pointer settings.

It opens no window, needs no administrator rights, and changes nothing. It is
the fastest way to find out why a mouse is not being detected — if the
`All HID collections` section shows a different VID/PID than `30FA`/`0400`,
that is exactly what needs fixing in the match table, and the report says so.

If it cannot attach to a console it writes `%TEMP%\PreoMG14Diagnostic.txt`
instead.

---

## What it does

### Device tab

Enumerates HID devices and matches the MG14 on its hardware IDs:

```
HID\VID_30FA&PID_0400&REV_0100
HID\VID_30FA&PID_0400
HID\VID_30FA&UP:0001_U:0002
HID_DEVICE_SYSTEM_MOUSE
HID_DEVICE_UP:0001_U:0002
HID_DEVICE
```

Then it shows, read-only:

- friendly name, description, manufacturer, product and serial strings
- vendor / product / revision, usage page and usage, reported button count
- the servicing driver: service name, `.sys` path, version, provider, date
- the device problem code and start state
- every hardware ID the device reports

Driver version and provider are read from the driver's own file version
resource rather than from a device property, so what you see is the version of
the binary that is actually loaded.

Three actions, all of which need Administrator rights and each of which shows
a UAC prompt naming what it is about to do:

| Button | What it actually does |
| --- | --- |
| **Restart driver** | Disables and re-enables the devnode via `CM_Disable_DevNode` / `CM_Enable_DevNode`, which makes the PnP manager re-run driver matching from the driver store. Fixes the common "disabled", "not provisioned" and "driver failed to start" states without touching a file on disk. |
| **Reinstall driver** | Calls `UpdateDriverForPlugAndPlayDevices` with `INSTALLFLAG_FORCE` against a driver package already present on the machine. |
| **Remove and rescan** | Removes the devnode so Windows re-detects the mouse from scratch. Unplug and replug afterwards. |

**The driver package it binds to** is, in order of preference:

1. `preo_mg14.inf` sitting next to the `.exe` (so you can supply your own
   package), otherwise
2. the Windows inbox `%SystemRoot%\INF\hid.inf`.

It never downloads a package and never accepts an unsigned one. Windows would
reject an unsigned package anyway, and quietly turning off signature
enforcement is exactly the kind of thing this program refuses to do.

### Buttons tab

Assign a different Windows action to the two side buttons and to the DPI
switch, and optionally invert vertical scrolling.

The MG14 is a plain HID mouse with no firmware interface a program can talk
to, so this works by watching clicks with a normal `WH_MOUSE_LL` hook and
re-sending the ones you reassigned. That has real consequences, all of them
stated in the app itself:

- **Remapping is active only while this window is open.** Close it and the
  mouse goes back to normal.
- **The hook is per-session, not per-device.** A second mouse on the same
  computer would be remapped the same way.
- **The DPI switch is usually invisible to Windows.** On many mice of this
  type the DPI button is handled entirely on-board and is never sent to the
  host, so it often cannot be remapped. The box is still offered in case
  yours does report it.
- Events the hook itself injects are tagged `LLKHF_INJECTED` and passed
  straight through, so remapping cannot feed back into a loop.

### Pointer tab

Pointer speed, enhance pointer precision, button swap, double-click speed and
enhance click precision. These write the standard Windows values:

- all pointing devices: `HKCU\Control Panel\Mouse`
- this mouse only: `HKCU\Software\Microsoft\Windows\CurrentVersion\Control Panel\Mouse\{4D36E96F-E325-11CE-BFC1-08002BE10318}\Device Parameters\Pointer`

then push them into the live input stack with `SystemParametersInfo` and
broadcast `WM_SETTINGCHANGE`.

**On DPI:** a standard HID mouse has no way to report or change its sensor
resolution — the MG14's DPI stage lives in its own firmware and is cycled by
the hardware button. No program, signed or unsigned, can change it. This tool
does not pretend otherwise.

---

## Why not a kernel driver

Because you cannot ship one that works on a normal Windows 10/11 machine.

- A replacement mouse driver has to be a kernel-mode `.sys`. Windows
  signatures have been enforced for 64-bit kernel drivers since Vista.
- Since Windows 10 version 1607, a *new* kernel driver must carry a Microsoft
  signature obtained through the Hardware Dev Center, which requires an EV
  code-signing certificate. That is a paid, yearly cost.
- Windows has no "trust this one unsigned driver" option. The only bypasses
  are disabling Secure Boot in UEFI, or `bcdedit /set testsigning on` — both
  global, both weakening the machine, and both a bad trade for a mouse.

The old open-source filter drivers that this project learned from
(`Interception`, `MouseTrap`, `IntelliMouseDriver`, the WDK `moufiltr` sample)
were all written for Windows 7, when an in-house test certificate was enough.
None of them install cleanly on a current Windows 11 machine with Secure Boot
on, and every one of them requires you to weaken the system first.

So this project does the job in **user mode**: the same SetupAPI, CfgMgr32
and HID calls those drivers' user-mode halves use, with none of the kernel
code, none of the signing requirement, and nothing to weaken.

If you ever see someone offer you a signed-looking "MG14 driver" `.exe` that
asks you to turn off Secure Boot or enable test mode, that is the thing to
walk away from.

---

## Safety

Full analysis in [docs/SAFETY.md](docs/SAFETY.md). The short version:

- **No network access at all.** The binary imports no networking library —
  no `winhttp`, `wininet`, `ws2_32`, `dnsapi`. It is not capable of phoning
  home. You can check this yourself:

  ```bash
  python3 tools/inspect_pe.py dist/PreoMG14Driver.exe
  ```

- **No kernel driver, no service, no scheduled task, no driver file is ever
  written.** Nothing is installed into `System32\drivers`.
- **Secure Boot, driver signature enforcement and test mode are never
  touched**, and the program will not ask you to touch them.
- **Runs as a normal user.** The manifest requests `asInvoker`. It elevates
  only for the three Device-tab buttons, via `ShellExecute("runas")`, so you
  always get a UAC prompt that names a specific action.
- **All state is one text file**, `%LOCALAPPDATA%\PreoMG14Driver\settings.ini`.
  It is plain INI, you can read it, and deleting it resets the program.
- **Start-with-Windows is off by default** and is a single `HKCU` value.
- **No obfuscation, no packing, no self-modifying code.** What is in the
  repository is what is in the binary; rebuild it and compare.

### About the SmartScreen warning

This build is **not code-signed**, because code signing costs money and I do
not have a certificate. SmartScreen warns about every unsigned program it has
not seen before, so a warning here is expected and is not by itself evidence
of anything.

If you would rather not take that on trust, check it yourself before running
it:

```bash
python3 tools/inspect_pe.py dist/PreoMG14Driver.exe   # import table
sha256sum dist/PreoMG14Driver.exe                     # compare with the .sha256 file
```

And after opening it, the Device tab shows you every device property it
reads, before it does anything at all.

---

## Building from source

Requires Python 3 and the Zig C compiler, which bundles its own MinGW-w64
toolchain and can target Windows from Linux.

```bash
pip install ziglang     # or install Zig from ziglang.org and put it on PATH
./build.sh              # -> dist/PreoMG14Driver.exe
./build.sh --clean      # from scratch
```

The build is offline, deterministic, and produces no intermediate binaries
that are not in this repository.

### Layout

```
src/
  wincompat.h     declarations the mingw-w64 headers get wrong or omit
  app.h           shared types and interfaces
  main.c          window, tabs, the four pages
  diagnostics.c   the --diagnose console report
  device.c        SetupAPI enumeration and device description
  hidinfo.c       HID collection capabilities
  driverops.c     the three privileged driver operations
  buttons.c       low-level mouse hook and the remap engine
  settings.c      INI persistence
  util.c          small helpers
  PreoMG14Driver.rc / app.manifest   icon, version info, manifest
tools/
  make_icon.py    generates src/preo.ico, so no opaque binary is committed
  inspect_pe.py   reads the import table back out of the built .exe
```

### A note on `wincompat.h`

That file exists because the mingw-w64 headers this project builds against
are demonstrably wrong in a few places, and the mistakes are the kind that
fail silently. Each entry is commented with the real value. Concretely, this
build had to work around:

| Header says | Truth | Consequence if used as-is |
| --- | --- | --- |
| `CM_PROB_DISABLED = 0x16` (in `cfg.h`) | `0x05` | wrong diagnosis text |
| `TCN_SELCHANGE = -551` | `0x12E2` | tab switching would never fire |
| `DEVPKEY_Device_Driver = 11` | `27` | reads the wrong property |
| `HIDP_CAPS.NumberButtons` | does not exist | button count always zero |
| `SetupDiCallClassInstaller(a,b,c)` | 4 arguments | corrupts the stack |
| `__declspec` on `HidD_*` redeclaration | n/a | malformed symbol names |

Driver restart uses `CM_Disable_DevNode` / `CM_Enable_DevNode` rather than
`SetupDiCallClassInstaller(DIF_DISABLE)` precisely because that header entry
cannot be called safely from here.

---

## Limitations, stated plainly

- **Not tested on physical MG14 hardware.** It was written and statically
  verified on Linux against the documented Windows APIs, and the import table
  and embedded resources were checked programmatically, but it has never been
  run on a machine with the mouse attached. Treat the first run as a test,
  and run `--diagnose` first — it will tell you immediately whether the mouse
  is being matched at all.
- **Button remapping stops when the window closes.** Making it survive logoff
  would need either an autostart entry (opt-in, in the Pointer tab) or a
  service, which this project deliberately does not install.
- **DPI resolution cannot be changed by software** on this hardware.
- **The DPI switch button is usually not visible to Windows**, as above.
- **One mouse at a time.** If you plug in two MG14s, the program uses the
  first one it finds.
- **x86-64 only.** A 32-bit build would need a separate compile.
- **Windows 10 version 1607 and later.** Older releases have not been tested.

---

## Uninstall

1. Close the program.
2. Delete `PreoMG14Driver.exe`.
3. Delete `%LOCALAPPDATA%\PreoMG14Driver`.
4. If you turned on start-with-Windows, delete the `PreoMG14Driver` value
   under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

That is the whole footprint. Nothing else was changed on the system.

---

## Licence

MIT. See [LICENSE](LICENSE).
