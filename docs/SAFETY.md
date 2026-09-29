# Safety analysis

This document is the argument for why `PreoMG14Driver.exe` is safe to run.
Every claim here is something you can check yourself; the verification
commands are given rather than asserted.

---

## 1. Threat model

A mouse driver installer is an unusually privileged thing to download. A
kernel driver runs in ring 0 with the same power as the operating system, it
loads before most security software, and — if the user is asked to disable
Secure Boot to allow it — it may be the only unsigned code running on the
machine. Malware in this position is extremely hard to detect and to remove.

So the questions that matter for this program are:

1. Does it run code in the kernel?
2. Does it weaken any system protection?
3. Can it talk to the network?
4. What does it leave behind, and can that be undone?

---

## 2. Does it run code in the kernel?

**No.**

There is no `.sys` file in this repository, none is produced by the build, and
the program has no code path that writes one. The only files it references are
ones Windows already ships.

The privileged operations it performs are all documented Win32 calls issued
from a normal user-mode process:

| Call | What it does |
| --- | --- |
| `SetupDiGetClassDevsW` / `SetupDiEnumDeviceInterfaces` | enumerate HID devices |
| `SetupDiGetDevicePropertyW` | read device properties |
| `CM_Get_DevNode_Status` | read the problem code |
| `CM_Disable_DevNode` / `CM_Enable_DevNode` | restart the devnode |
| `UpdateDriverForPlugAndPlayDevicesW` | re-bind to a driver package already on disk |
| `CM_RemoveDevice` | remove the devnode |
| `HidD_*` / `HidP_GetCaps` | read the HID report descriptor |
| `SetWindowsHookExW(WH_MOUSE_LL)` | a documented user-mode hook |
| `SendInput` | synthesise input |

None of these execute code in kernel space. `WH_MOUSE_LL` in particular is
worth calling out because "mouse hook" sounds alarming: it is a documented
user-mode callback, it lives in this process, it is removed the moment the
process exits, and it cannot be used to escalate privilege.

Verify:

```bash
python3 tools/inspect_pe.py dist/PreoMG14Driver.exe
```

The last two lines must read:

```
PASS  no networking libraries imported
PASS  no kernel-mode components referenced
```

---

## 3. Does it weaken any system protection?

**No.**

The program never touches Secure Boot, driver signature enforcement, test
signing, HVCI/Memory Integrity, or UEFI settings, and it never runs `bcdedit`.

- It does not install a driver, so signature enforcement is never engaged.
- The driver package it binds to is one that is already on the machine and
  already signed by Microsoft. It cannot downgrade you to something unsigned.
- If no suitable package is found, it reports that and does nothing, rather
  than looking for a way around.

This is the main reason the design avoids a kernel driver. The open-source
filter drivers it learned from all required `bcdedit /set testsigning on` or a
Secure Boot change, which is a global, persistent reduction in the machine's
security for the sake of a mouse button.

---

## 4. Can it talk to the network?

**No, and it is structurally incapable of doing so.**

It imports no networking library. Not `ws2_32`, not `winhttp`, not `wininet`,
not `dnsapi`, not `urlmon`. Without one of those it cannot open a socket, and
there is no other supported way to reach the network from a Win32 process.

It also has no update check, no telemetry, no crash reporting, and no
analytics. The complete set of what it links against is printed by
`tools/inspect_pe.py`; it is 17 DLLs, all of them local system libraries.

---

## 5. Elevation

The application manifest declares `asInvoker`:

```xml
<requestedExecutionLevel level="asInvoker" uiAccess="false"/>
```

So it starts as a normal user. Browsing the tabs, reading every device
property, and changing pointer and button settings all work without
elevation.

Only three buttons require administrator rights, and each one:

1. shows a `MessageBox` naming the specific action,
2. re-launches the same executable through `ShellExecuteW` with the `runas`
   verb, producing a standard UAC prompt,
3. passes the action id as `--action=N` so the elevated half does exactly what
   was asked and nothing else,
4. runs one operation and exits.

There is no path that elevates silently or does more than was requested.

---

## 6. What it leaves behind

| Location | Contents | How to remove |
| --- | --- | --- |
| `%LOCALAPPDATA%\PreoMG14Driver\settings.ini` | all program settings, plain INI | delete the folder |
| `HKCU\...\Control Panel\Mouse` | pointer settings, only if you applied them | the Pointer tab's mouse settings, or Windows' own mouse settings UI |
| `HKCU\...\Control Panel\Mouse\{4D36E96F-...}\Device Parameters\Pointer` | per-device pointer settings, only if you applied them | same |
| `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` | one value, only if you enabled start-with-Windows | uncheck the box, or delete the value |

There are no files in `System32`, no service registration, no scheduled task,
no firewall rule, and no WMI subscription.

Start-with-Windows is **off by default**, which matters: an autostart entry is
the one thing in that list that runs without you opening the program again.

---

## 7. Supply chain

- Full source is in this repository under the MIT licence.
- The build is offline and deterministic. It uses one external tool, the Zig
  C compiler, which is why the build script is four lines of `zig cc`.
- No prebuilt binaries are committed. Even the icon is generated from a
  script, `tools/make_icon.py`, so there is no opaque blob in the tree.
- No obfuscation, no packing, no anti-debugging, no self-modifying code.
  The binary is plain compiled C.
- `dist/PreoMG14Driver.exe.sha256` is written by the build so you can confirm
  the file you are about to run is the one this source produces.

To be sure the binary matches the source, rebuild and compare:

```bash
./build.sh --clean
sha256sum dist/PreoMG14Driver.exe
```

---

## 8. Honest gaps

Stating these because a safety document that only lists strengths is not
useful.

- **It is not code-signed.** You will get a SmartScreen warning. That warning
  is about the absence of a signature, not evidence of anything, but it does
  mean you cannot use the signature as a trust signal. Use the import table
  and the hash instead.
- **It has not been run on physical MG14 hardware.** It was written and
  statically checked on Linux; the PE structure, import table and embedded
  resources were verified programmatically, but no one has plugged the mouse
  into a machine and clicked through it. Treat the first run as a test.
- **The button hook can see all mouse input in your session**, because that is
  what a `WH_MOUSE_LL` hook does. It is off by default, it only acts on the
  buttons you configured, and it only runs while the window is open — but a
  user who is security-sensitive should know that enabling remapping gives a
  user-mode hook a view of their mouse activity for as long as it is enabled.
- **Remapping is not per-device.** A second mouse would be affected too.
