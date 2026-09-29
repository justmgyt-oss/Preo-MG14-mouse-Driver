/*
 * driverops.c - Everything that can change the *driver state* of the device.
 *
 * All of these need Administrator rights, so each one is expected to be called
 * from the elevated half of the process (see relaunch_elevated in main.c).
 * The user always sees a normal UAC consent prompt first.
 *
 * Safety properties, by design:
 *   - We never install a kernel-mode .sys of our own. We only ask Windows to
 *     re-bind the device to a *Microsoft-signed inbox* driver package that is
 *     already on the machine, so driver signature enforcement stays on and
 *     Secure Boot is never touched.
 *   - Every operation is reversible from the same UI.
 */
#include "app.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <stdio.h>

/* newdev.dll is not present on every SKU, and Zig's generated cfgmgr32
 * import library does not cover CM_RemoveDevice / CM_Reenumerate, so all
 * three are resolved with GetProcAddress at run time. */
typedef BOOL (WINAPI *PfnUpdateDriverForPlugAndPlayDevices)(
    HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);

static PfnCM_RemoveDevice  pfn_CM_RemoveDevice;
static PfnCM_Reenumerate   pfn_CM_Reenumerate;

static void resolve_cfgmgr32(void)
{
    HMODULE h;
    if (pfn_CM_RemoveDevice && pfn_CM_Reenumerate) return;
    h = GetModuleHandleW(L"cfgmgr32.dll");
    if (!h) h = LoadLibraryW(L"cfgmgr32.dll");
    if (!h) return;
    pfn_CM_RemoveDevice = (PfnCM_RemoveDevice)GetProcAddress(h, "CM_RemoveDevice");
    pfn_CM_Reenumerate  = (PfnCM_Reenumerate) GetProcAddress(h, "CM_Reenumerate");
}

BOOL is_elevated(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elev;
    DWORD  len = sizeof(elev);
    BOOL   ok = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len))
        ok = elev.TokenIsElevated != 0;
    CloseHandle(token);
    return ok;
}

BOOL relaunch_elevated(const wchar_t *actionArgs)
{
    wchar_t exe[MAX_PATH];
    wchar_t params[96];
    SHELLEXECUTEINFOW sei;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return FALSE;
    _snwprintf(params, 96, L"--elevated %ls", actionArgs ? actionArgs : L"");
    params[95] = L'\0';

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params;
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) return FALSE;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return TRUE;
}

const wchar_t *drvop_result_message(DrvOpResult r)
{
    switch (r) {
    case DRVOP_OK:
        return L"Done. The device is using the standard Windows HID mouse driver.";
    case DRVOP_ERR_ACCESS:
        return L"This action needs Administrator rights. Re-run and accept the UAC prompt.";
    case DRVOP_ERR_NOTFOUND:
        return L"The Preo MG14 was not found. Plug it in and press Rescan.";
    case DRVOP_ERR_API:
        return L"Windows reported an error while applying the change. The device was left as it was.";
    case DRVOP_ERR_NO_INF:
        return L"No driver package was found. Place the .inf file next to this program, or pick one manually.";
    case DRVOP_ERR_REBOOT:
        return L"Done, but Windows needs a restart before it takes effect.";
    default:
        return L"Unknown result.";
    }
}

/*
 * Find a usable, already-signed driver package.
 *
 * Preference order:
 *   1. An .inf sitting next to this executable (lets a user supply their own
 *      signed package without editing anything).
 *   2. The Windows inbox HID-class package %SystemRoot%\INF\hid.inf.
 *
 * We never download anything and never accept an unsigned package: Windows
 * would reject it anyway, and silently downgrading signature enforcement is
 * exactly the kind of thing this program refuses to do.
 */
const wchar_t *inbox_hid_inf_path(void)
{
    static wchar_t path[MAX_PATH];
    wchar_t exeDir[MAX_PATH], candidate[MAX_PATH];
    wchar_t winDir[MAX_PATH];

    if (path[0]) return path[0] ? path : NULL;

    /* 1. next to the exe */
    if (GetModuleFileNameW(NULL, exeDir, MAX_PATH)) {
        wchar_t *slash;
        exeDir[MAX_PATH - 1] = L'\0';
        slash = wcsrchr(exeDir, L'\\');
        if (slash) {
            *slash = L'\0';
            _snwprintf(candidate, MAX_PATH, L"%s\\preo_mg14.inf", exeDir);
            candidate[MAX_PATH - 1] = L'\0';
            if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
                wstr_copy(path, MAX_PATH, candidate, MAX_PATH - 1);
                return path;
            }
        }
    }

    /* 2. inbox HID class package */
    if (GetWindowsDirectoryW(winDir, MAX_PATH)) {
        static const wchar_t *candidates[] = { L"hid.inf", L"hidclass.inf", L"usb.inf" };
        size_t i;
        for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            _snwprintf(candidate, MAX_PATH, L"%s\\INF\\%s", winDir, candidates[i]);
            candidate[MAX_PATH - 1] = L'\0';
            if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
                wstr_copy(path, MAX_PATH, candidate, MAX_PATH - 1);
                return path;
            }
        }
    }
    return NULL;
}

/*
 * Restart the devnode: disable then enable. This makes the PnP manager run
 * driver matching again from the driver store, which repairs the common
 * "device is disabled / not provisioned / driver failed to start" states
 * without touching a single file on disk.
 *
 * We use CM_Disable_DevNode / CM_Enable_DevNode rather than
 * SetupDiCallClassInstaller(DIF_DISABLE) on purpose: mingw-w64 declares
 * SetupDiCallClassInstaller with a 3-argument signature and ships a DI_FUNCTION
 * table whose values disagree with the Windows SDK, so the class-installer
 * path is not safe to call from here.
 */
DrvOpResult driver_restart(DeviceInfo *dev, BOOL *needsReboot)
{
    HDEVINFO        hSet;
    SP_DEVINFO_DATA di;
    CONFIGRET       cr;
    DEVINST         dn = 0;

    if (needsReboot) *needsReboot = FALSE;
    if (!is_elevated()) return DRVOP_ERR_ACCESS;
    if (!dev || !dev->found) return DRVOP_ERR_NOTFOUND;

    hSet = device_open_by_instance(dev->devInst, &di);
    if (hSet == INVALID_HANDLE_VALUE) return DRVOP_ERR_NOTFOUND;

    cr = CM_Locate_DevNodeW(&dn, dev->devInst, CM_LOCATE_DEVNODE_NORMAL);
    if (cr != CR_SUCCESS) {
        SetupDiDestroyDeviceInfoList(hSet);
        return DRVOP_ERR_NOTFOUND;
    }

    /* CM_DISABLE_POLITE lets the stack unwind cleanly. */
    if (CM_Disable_DevNode(dn, CM_DISABLE_POLITE) == CR_SUCCESS) {
        CM_Enable_DevNode(dn, 0);
    } else {
        /* Already disabled, or cannot be disabled: try starting it anyway. */
        CM_Reenumerate_DevNode(dn, 0);
    }

    SetupDiDestroyDeviceInfoList(hSet);
    if (needsReboot) *needsReboot = FALSE;
    return DRVOP_OK;
}

/*
 * Force the device to be matched against a specific driver package. The
 * package must already be present and signed; we just point SetupAPI at it.
 */
DrvOpResult driver_install(DeviceInfo *dev, BOOL *needsReboot)
{
    HMODULE newdev;
    PfnUpdateDriverForPlugAndPlayDevices pfn;
    const wchar_t *inf;
    wchar_t hardwareId[128];
    BOOL   reboot = FALSE;

    if (needsReboot) *needsReboot = FALSE;
    if (!is_elevated()) return DRVOP_ERR_ACCESS;
    if (!dev || !dev->found) return DRVOP_ERR_NOTFOUND;

    inf = inbox_hid_inf_path();
    if (!inf) return DRVOP_ERR_NO_INF;

    newdev = LoadLibraryW(L"newdev.dll");
    if (!newdev) return DRVOP_ERR_API;
    pfn = (PfnUpdateDriverForPlugAndPlayDevices)GetProcAddress(newdev, "UpdateDriverForPlugAndPlayDevicesW");
    if (!pfn) { FreeLibrary(newdev); return DRVOP_ERR_API; }

    _snwprintf(hardwareId, 128, L"%ls", L"HID\\VID_30FA&PID_0400&REV_0100");
    hardwareId[127] = L'\0';

    /* INSTALLFLAG_FORCE = reinstall even if Windows thinks it is current. */
    if (!pfn(NULL, hardwareId, inf, 0x00000001 /*INSTALLFLAG_FORCE*/, &reboot)) {
        FreeLibrary(newdev);
        return DRVOP_ERR_API;
    }
    FreeLibrary(newdev);

    if (needsReboot) *needsReboot = reboot;
    return reboot ? DRVOP_ERR_REBOOT : DRVOP_OK;
}

/*
 * Remove the devnode so Windows re-detects the mouse from scratch. The signed
 * inbox driver is re-matched automatically the next time it is plugged in.
 */
DrvOpResult driver_remove(DeviceInfo *dev, BOOL *needsReboot)
{
    HDEVINFO        hSet;
    SP_DEVINFO_DATA di;
    DEVINST         parent = 0;
    DEVINST         dn = 0;
    CONFIGRET       cr;

    if (needsReboot) *needsReboot = FALSE;
    if (!is_elevated()) return DRVOP_ERR_ACCESS;
    if (!dev || !dev->found) return DRVOP_ERR_NOTFOUND;

    hSet = device_open_by_instance(dev->devInst, &di);
    if (hSet == INVALID_HANDLE_VALUE) return DRVOP_ERR_NOTFOUND;

    cr = CM_Locate_DevNodeW(&dn, dev->devInst, CM_LOCATE_DEVNODE_NORMAL);
    if (cr != CR_SUCCESS) {
        SetupDiDestroyDeviceInfoList(hSet);
        return DRVOP_ERR_NOTFOUND;
    }
    if (CM_Get_Parent(&parent, dn, 0) != CR_SUCCESS) parent = 0;

    SetupDiDestroyDeviceInfoList(hSet);

    resolve_cfgmgr32();
    if (!pfn_CM_RemoveDevice) return DRVOP_ERR_API;
    cr = pfn_CM_RemoveDevice(&dn, 0);
    if (cr != CR_SUCCESS) return DRVOP_ERR_API;

    if (parent) CM_Reenumerate_DevNode(parent, 0);
    else if (pfn_CM_Reenumerate) pfn_CM_Reenumerate(0);
    if (needsReboot) *needsReboot = FALSE;
    return DRVOP_OK;
}

/* Ask the PnP manager to look for hardware again (new/removed devices). */
DrvOpResult driver_rescan(BOOL *needsReboot)
{
    if (needsReboot) *needsReboot = FALSE;
    resolve_cfgmgr32();
    if (pfn_CM_Reenumerate) pfn_CM_Reenumerate(0);
    else                   CM_Reenumerate_DevNode(0, 0);
    return DRVOP_OK;
}
