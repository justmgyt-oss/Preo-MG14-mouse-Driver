/*
 * app.h - Shared types and interfaces for the Preo MG14 Driver Manager.
 *
 * Design rule for this program: everything happens in user mode through
 * documented Win32 APIs. We never install a kernel driver, never touch Secure
 * Boot, and never talk to the network. See docs/SAFETY.md.
 */
#ifndef PREO_APP_H
#define PREO_APP_H

#include <windows.h>
#include <setupapi.h>
#include "wincompat.h"

#define APP_NAME        L"Preo MG14 Driver Manager"
#define APP_SHORT       L"PreoMG14Driver"
#define APP_VERSION     L"1.0.0"
#define APP_VERSION_NUM 0x00010000

/* ------------------------------------------------------------------ *
 * Target hardware. These are the exact hardware IDs reported by the
 * Preo MG14 (VID_30FA, PID_0400, REV_0100, Generic Desktop / Mouse).
 * ------------------------------------------------------------------ */
#define PREO_VID              0x30FA
#define PREO_PID              0x0400
#define PREO_REV              0x0100
#define PREO_USAGE_PAGE       0x01   /* Generic Desktop */
#define PREO_USAGE            0x02   /* Mouse */

/* The full Hardware IDs string as it appears in Device Manager > Details. */
extern const wchar_t *const g_preoHardwareIds[];
extern const size_t          g_preoHardwareIdCount;

/* ------------------------------------------------------------------ *
 * Device model
 * ------------------------------------------------------------------ */
typedef struct {
    BOOL    found;              /* device present and matched            */
    wchar_t devInst[64];        /* HID\VID_30FA&PID_0400&REV_0100\5&... */
    wchar_t interfacePath[512]; /* device interface path for CreateFile   */

    wchar_t deviceDesc[256];    /* "HID-compliant mouse"                  */
    wchar_t friendlyName[256];
    wchar_t manufacturer[128];
    wchar_t productName[128];
    wchar_t serial[128];
    wchar_t serviceName[64];    /* e.g. "HidUsb"                          */
    wchar_t driverSys[512];     /* full path of the servicing .sys       */
    wchar_t driverVersion[64];
    wchar_t driverProvider[128];
    wchar_t driverDate[64];
    wchar_t infName[128];

    USHORT  vendorId, productId, versionNumber;
    USAGE   usagePage, usage;
    USHORT  numberButtons;
    USHORT  numberValueCaps;
    USHORT  numberInputReportCaps;

    ULONG   problemCode;        /* CM_PROB_*                              */
    DWORD   devNodeStatus;      /* CM_Get_DevNode_Status                  */
    BOOL    present;
    BOOL    started;
    BOOL    needsReboot;
} DeviceInfo;

/* ------------------------------------------------------------------ *
 * Settings - persisted to a plain INI file under %LOCALAPPDATA%
 * ------------------------------------------------------------------ */
typedef enum {
    BTN_NONE = 0,
    BTN_MIDDLE, BTN_X1, BTN_X2, BTN_BACK, BTN_FORWARD,
    BTN_SCROLL_UP, BTN_SCROLL_DOWN, BTN_COPY, BTN_PASTE,
    BTN_DOUBLE_CLICK, BTN_MEDIA_PLAY_PAUSE, BTN_MEDIA_NEXT, BTN_MEDIA_PREV,
    BTN_BROWSER_BACK, BTN_BROWSER_FORWARD, BTN_MUTE, BTN_VOL_UP, BTN_VOL_DOWN,
    BTN_DISABLED,
    BTN_COUNT
} ButtonAction;

typedef struct {
    /* pointer */
    int   pointerSpeed;         /* 1..20, maps to MouseSpeed              */
    BOOL  enhancePrecision;     /* MouseThreshold1/2 == 0                 */
    BOOL  swapXY;
    int   doubleClickSpeed;     /* 250..900 ms                            */
    BOOL  enhanceClickPrecision;

    /* button remap engine */
    BOOL  remapEnabled;
    ButtonAction x1Action;      /* physical "back" side button            */
    ButtonAction x2Action;      /* physical "forward" side button         */
    ButtonAction dpiAction;     /* what the DPI switch button does        */
    BOOL  wheelInvert;

    /* housekeeping */
    BOOL  startWithWindows;
} Settings;

/* ------------------------------------------------------------------ *
 * Interfaces
 * ------------------------------------------------------------------ */

/* util.c ---------------------------------------------------------------- */
void      *xmalloc(size_t n);
wchar_t  *wstrdup_n(const wchar_t *s, size_t n);
/* Truncating, always-NUL-terminating copy. Named to avoid the CRT's
 * wcsncpy_s, which has a different signature. */
size_t     wstr_copy(wchar_t *dst, size_t dstChars, const wchar_t *src, size_t n);
void       utf8_to_wide(const char *in, wchar_t *out, size_t outChars);
wchar_t   *wide_dup(const wchar_t *s);
BOOL       get_settings_dir(wchar_t *out, size_t cch);
BOOL       ensure_settings_dir(void);

/* settings.c ------------------------------------------------------------ */
void   settings_defaults(Settings *s);
BOOL   settings_load(Settings *s);
BOOL   settings_save(const Settings *s);
BOOL   settings_reset(void);            /* delete the ini file            */
const wchar_t *button_action_name(ButtonAction a);
ButtonAction button_action_from_name(const wchar_t *name);
const wchar_t *mouse_button_label(int buttonIndex); /* 0=MB_LB .. 5=MB_XB2*/

/* device.c -------------------------------------------------------------- */
void   device_scan(DeviceInfo *out);
const wchar_t *device_problem_text(const DeviceInfo *dev);
BOOL   file_version_info(const wchar_t *path, wchar_t *ver, size_t cchVer,
                         wchar_t *company, size_t cchCompany,
                         wchar_t *desc, size_t cchDesc);

/* Re-open a device info set for a known device instance id. The caller owns
 * the returned set and must call SetupDiDestroyDeviceInfoList. */
HDEVINFO device_open_by_instance(const wchar_t *devInst, PSP_DEVINFO_DATA di);

/* Append a human-readable dump of every present HID collection to `buf`,
 * marking the ones that match the MG14. Used by --diagnose. */
void device_list_all_hid(wchar_t *buf, size_t cch, int *outLen);

/* hidinfo.c ------------------------------------------------------------- */
BOOL   hid_query(DeviceInfo *dev);     /* fill HID fields, needs open dev  */

/* driverops.c ------------------------------------------------------------ */
typedef enum {
    DRVOP_OK = 0,
    DRVOP_ERR_ACCESS,          /* not elevated                              */
    DRVOP_ERR_NOTFOUND,        /* device not present                        */
    DRVOP_ERR_API,             /* Windows API failure                       */
    DRVOP_ERR_NO_INF,          /* no suitable signed driver package found   */
    DRVOP_ERR_REBOOT           /* succeeded, reboot required                */
} DrvOpResult;

DrvOpResult driver_install(DeviceInfo *dev, BOOL *needsReboot);
DrvOpResult driver_restart(DeviceInfo *dev, BOOL *needsReboot);
DrvOpResult driver_remove(DeviceInfo *dev, BOOL *needsReboot);
DrvOpResult driver_rescan(BOOL *needsReboot);
const wchar_t *drvop_result_message(DrvOpResult r);
BOOL   is_elevated(void);
BOOL   relaunch_elevated(const wchar_t *actionArgs);
const wchar_t *inbox_hid_inf_path(void);

/* buttons.c -------------------------------------------------------------- */
#define WM_APP_CAPTURED (WM_APP + 3)
BOOL   remap_start(const Settings *s);
void   remap_stop(void);
BOOL   remap_is_running(void);
int    remap_last_button(void);                /* -1 if none yet          */
void   remap_reset_last(void);
void   remap_set_notify(HWND w);

/* main.c ----------------------------------------------------------------- */
BOOL   apply_pointer_settings(const Settings *s, BOOL perDevice, HWND hWnd);
BOOL   apply_autostart(BOOL enable);

/* diagnostics.c ---------------------------------------------------------- */
/* Console report of everything the program can see about the machine. This is
 * the way to get a real-hardware sanity check without a GUI. */
BOOL diagnostics_requested(LPWSTR cmdLine);
int  run_diagnostics(LPWSTR cmdLine);

#endif /* PREO_APP_H */
