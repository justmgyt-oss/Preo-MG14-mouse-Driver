/*
 * wincompat.h - Declarations missing from the mingw-w64 headers shipped with Zig.
 *
 * The Windows SDK ships <hidclass.h> with the full set of HidD_* user-mode
 * entry points. The mingw-w64 copy only has the kernel-side IOCTL codes, so we
 * declare the handful we actually call here and link against hid.lib.
 *
 * Signatures follow the Windows SDK; see
 * https://learn.microsoft.com/en-us/windows/win32/api/hidclass/nf-hidclass-hidd_gethidguid
 */
#ifndef PREO_WINCOMPAT_H
#define PREO_WINCOMPAT_H

#include <windows.h>
#include <guiddef.h>    /* PGUID */
#include <cfgmgr32.h>
#include <hidusage.h>   /* USAGE, required by hidpi.h */
#include <hidclass.h>
#include <hidpi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- hid.lib: user-mode HID helper API ---------------------------------- */
#ifndef _HIDCLASS_H_
typedef struct _HIDD_ATTRIBUTES {
    USHORT  Size;
    USHORT  VendorID;
    USHORT  ProductID;
    USHORT  VersionNumber;
} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;
#endif

/* hid.lib */
__declspec(dllimport) BOOLEAN  __stdcall HidD_GetHidGuid(OUT GUID *pguid);
__declspec(dllimport) BOOLEAN  __stdcall HidD_GetAttributes(HANDLE h, PHIDD_ATTRIBUTES a);
__declspec(dllimport) BOOLEAN  __stdcall HidD_GetPreparsedData(HANDLE h, PHIDP_PREPARSED_DATA *d);
__declspec(dllimport) BOOLEAN  __stdcall HidD_FreePreparsedData(PHIDP_PREPARSED_DATA d);
__declspec(dllimport) BOOLEAN  __stdcall HidD_GetManufacturerString(HANDLE h, PVOID b, ULONG n);
__declspec(dllimport) BOOLEAN  __stdcall HidD_GetProductString(HANDLE h, PVOID b, ULONG n);
__declspec(dllimport) BOOLEAN  __stdcall HidD_GetSerialNumberString(HANDLE h, PVOID b, ULONG n);
__declspec(dllimport) BOOLEAN  __stdcall HidD_GetFeature(HANDLE h, PVOID b, ULONG n);
__declspec(dllimport) BOOLEAN  __stdcall HidD_SetFeature(HANDLE h, PVOID b, ULONG n);
/* Note: HidD_GetUsagePage / HidD_GetUsage are kernel-mode only and are not
 * exported by hid.dll, so the usage page/usage come from HidP_GetCaps. */

/* HidP_GetButtonCaps and HIDP_STATUS_SUCCESS are already declared by
 * <hidpi.h>; we link against hid.lib in the build script rather than
 * redeclaring them here. */

/* --- newdev.lib: installs a signed driver package for a PnP device ------- */
typedef BOOL (WINAPI *PfnUpdateDriverForPlugAndPlayDevicesW)(
    HWND hwndParent, LPCWSTR lpszHardwareId, LPCWSTR lpszFullInfPath,
    DWORD dwInstallFlags, PBOOL pbRequireReboot);

/* --- cfgmgr32: bits the mingw header gets wrong or omits ---------------- *
 * mingw-w64 ships a stale <cfg.h> that defines CM_PROB_DISABLED as 0x16 and
 * several other CM_PROB_* values that disagree with the Windows SDK, and
 * <cfgmgr32.h> omits CM_RemoveDevice / CM_Reenumerate entirely. Rather than
 * fight the headers we use our own names for the problem codes and declare
 * the two missing entry points directly.
 *
 * Values below are the documented SDK ones (setupapi.h "Device Install
 * Problems").                                                                  */
enum {
    PREO_PROB_NONE                = 0x00,
    PREO_PROB_NOT_PROVISIONED     = 0x01,
    PREO_PROB_NO_SUCH_DEVINST     = 0x02,
    PREO_PROB_NO_DEVNODE          = 0x03,
    PREO_PROB_HARDWARE_ERROR      = 0x04,
    PREO_PROB_DISABLED            = 0x05,
    PREO_PROB_DI_REMOVED          = 0x06,
    PREO_PROB_NO_CONFIG           = 0x07,
    PREO_PROB_NOT_DI_INSTALLED    = 0x08,
    PREO_PROB_FAILED_INSTALL      = 0x09,
    PREO_PROB_DEVICE_NOT_CONNECTED= 0x0A,
    PREO_PROB_DRIVER_NOT_LOADED   = 0x0B,
    PREO_PROB_COMPAT_DRIVER       = 0x0C,
    PREO_PROB_NEED_RESTART        = 0x0D,
    PREO_PROB_DEVICE_DATAFILE     = 0x0E,
    PREO_PROB_DRIVER_DATAFILE     = 0x0F,
    PREO_PROB_NO_FILTER           = 0x10,
    PREO_PROB_REMOVED             = 0x11,
    PREO_PROB_HIDDEN              = 0x12,
    PREO_PROB_NONEXISTENT         = 0x13,
    PREO_PROB_WILL_BE_REMOVED     = 0x14,
    PREO_PROB_SHUTDOWN_BINOMIAL   = 0x15,
    PREO_PROB_NEED_TO_ENUM        = 0x40
};

/* CM_RemoveDevice / CM_Reenumerate are resolved with GetProcAddress at run
 * time (see driverops.c) rather than imported, because Zig's generated
 * cfgmgr32 import library does not cover them. */
typedef CONFIGRET (WINAPI *PfnCM_RemoveDevice)(PDEVINST pdnDevInst, ULONG ulFlags);
typedef CONFIGRET (WINAPI *PfnCM_Reenumerate)(ULONG ulFlags);

/* SPI_SETSWITCHBUTTON is not present in this mingw snapshot. */
#ifndef SPI_SETSWITCHBUTTON
#define SPI_SETSWITCHBUTTON 0x0074
#endif

/* mingw-w64's <commctrl.h> derives TCN_FIRST as (0U-550U), giving
 * TCN_SELCHANGE == -551. The Windows SDK defines
 *   TCN_FIRST       = TCM_FIRST - 20  = 4844
 *   TCN_SELCHANGE   = TCN_FIRST - 10  = 4834 = 0x12E2
 * so the mingw value is wrong and a tab switch would never be delivered.
 * Pin the correct one. */
#define PREO_TCN_SELCHANGE 0x12E2

/* --- DEVPKEY helpers ------------------------------------------------- *
 * mingw-w64's <devpkey.h> declares its DEVPKEY_* constants as `extern const`
 * but never defines them, so linking fails. We build the four keys we need
 * from the verified property IDs instead.                                     */
#define PREO_DEVPROPKEY(pid_) { { 0xb725f130, 0x47ef, 0x101a,               \
                                  { 0xa5, 0xf1, 0x02, 0x60,                 \
                                    0x8c, 0x9e, 0xeb, 0xac } }, (pid_) }

#define PREO_KEY_DEVICE_DESC     PREO_DEVPROPKEY(2)
#define PREO_KEY_HARDWARE_IDS    PREO_DEVPROPKEY(3)
#define PREO_KEY_SERVICE         PREO_DEVPROPKEY(6)
#define PREO_KEY_MANUFACTURER    PREO_DEVPROPKEY(13)
#define PREO_KEY_FRIENDLY_NAME   PREO_DEVPROPKEY(14)

#ifdef __cplusplus
}
#endif
#endif /* PREO_WINCOMPAT_H */
