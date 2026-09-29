/*
 * device.c - Locate the Preo MG14 via SetupAPI and describe it.
 *
 * We match on the hardware IDs the MG14 actually reports:
 *
 *   HID\VID_30FA&PID_0400&REV_0100
 *   HID\VID_30FA&PID_0400
 *   HID\VID_30FA&UP:0001_U:0002
 *   HID_DEVICE_SYSTEM_MOUSE
 *   HID_DEVICE_UP:0001_U:0002
 *   HID_DEVICE
 *
 * Note on DEVPKEY usage: mingw-w64 ships an outdated <devpkey.h> in which some
 * property IDs disagree with the Windows SDK (e.g. DEVPKEY_Device_Driver is
 * pid 11 there but pid 27 in the SDK). We therefore only use keys whose IDs we
 * verified against the SDK - DeviceDesc(2), HardwareIds(3), Service(6),
 * Manufacturer(13), FriendlyName(14), ProblemCode(3) - and read driver version
 * information from the driver's own file version resource rather than from a
 * device property, which is both correct and harder to spoof.
 */
#include "app.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>

/* Locally defined device property keys (see PREO_DEVPROPKEY in wincompat.h). */
static const DEVPROPKEY KEY_DEVICE_DESC     = PREO_KEY_DEVICE_DESC;
static const DEVPROPKEY KEY_HARDWARE_IDS    = PREO_KEY_HARDWARE_IDS;
static const DEVPROPKEY KEY_SERVICE         = PREO_KEY_SERVICE;
static const DEVPROPKEY KEY_MANUFACTURER    = PREO_KEY_MANUFACTURER;
static const DEVPROPKEY KEY_FRIENDLY_NAME   = PREO_KEY_FRIENDLY_NAME;

/* GUID_DEVINTERFACE_HID = {4d1e55b2-f16f-11cf-88cb-001111000030} */
static const GUID GUID_DEVINTERFACE_HID_LOCAL = {
    0x4d1e55b2, 0xf16f, 0x11cf,
    { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 }
};

const wchar_t *const g_preoHardwareIds[] = {
    L"HID\\VID_30FA&PID_0400&REV_0100",
    L"HID\\VID_30FA&PID_0400",
    L"HID\\VID_30FA&UP:0001_U:0002",
    L"HID_DEVICE_SYSTEM_MOUSE",
    L"HID_DEVICE_UP:0001_U:0002",
    L"HID_DEVICE"
};
const size_t g_preoHardwareIdCount =
    sizeof(g_preoHardwareIds) / sizeof(g_preoHardwareIds[0]);

/* ------------------------------------------------------------------ */

static BOOL get_str_prop_key(HDEVINFO h, PSP_DEVINFO_DATA di,
                             const DEVPROPKEY *key, wchar_t *out, size_t cch)
{
    DWORD need = 0, type = 0;
    if (!out || cch < 2 || !key) return FALSE;
    out[0] = L'\0';
    if (!SetupDiGetDevicePropertyW(h, di, key, &type, (PBYTE)out,
                                   (DWORD)(cch * sizeof(wchar_t)), &need, 0))
        return FALSE;
    if (type != DEVPROP_TYPE_STRING) { out[0] = L'\0'; return FALSE; }
    out[cch - 1] = L'\0';
    return out[0] != L'\0';
}

/* Does this devnode's Hardware IDs list contain one of the MG14 IDs? */
static BOOL hardware_ids_match(HDEVINFO h, PSP_DEVINFO_DATA di)
{
    const DEVPROPKEY key = KEY_HARDWARE_IDS;
    DWORD  need = 0, type = 0;
    LPWSTR buf, p;
    BOOL   hit = FALSE;

    if (!SetupDiGetDevicePropertyW(h, di, &key, &type, NULL, 0, &need, 0) || !need)
        return FALSE;
    if (type != DEVPROP_TYPE_STRING_LIST) return FALSE;

    buf = (LPWSTR)xmalloc(need + sizeof(wchar_t));
    if (!buf) return FALSE;
    if (SetupDiGetDevicePropertyW(h, di, &key, &type, (PBYTE)buf, need, NULL, 0)) {
        for (p = buf; *p; p += wcslen(p) + 1) {
            size_t i;
            for (i = 0; i < g_preoHardwareIdCount; i++) {
                if (_wcsicmp(p, g_preoHardwareIds[i]) == 0) { hit = TRUE; break; }
            }
            if (hit) break;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return hit;
}

static const wchar_t *problem_text(ULONG code)
{
    static wchar_t buf[96];
    switch (code) {
    case PREO_PROB_NONE:              return L"This device is working properly.";
    case PREO_PROB_NOT_PROVISIONED:   return L"No driver is installed for this device.";
    case PREO_PROB_NO_SUCH_DEVINST:   return L"The device node does not exist.";
    case PREO_PROB_NO_DEVNODE:        return L"The device node is not present.";
    case PREO_PROB_HARDWARE_ERROR:    return L"A hardware error was reported by the device.";
    case PREO_PROB_DI_REMOVED:        return L"The device has been removed from the system.";
    case PREO_PROB_DISABLED:          return L"The device is disabled.";
    case PREO_PROB_FAILED_INSTALL:    return L"The driver failed to install.";
    case PREO_PROB_NO_CONFIG:         return L"The device is not configured.";
    case PREO_PROB_NEED_RESTART:      return L"The device requires a restart.";
    case PREO_PROB_DRIVER_NOT_LOADED: return L"The driver is not loaded.";
    case PREO_PROB_DEVICE_NOT_CONNECTED: return L"The device is not connected.";
    default:
        _snwprintf(buf, 96, L"Device reported problem code %lu.", code);
        buf[95] = L'\0';
        return buf;
    }
}

const wchar_t *device_problem_text(const DeviceInfo *d) { return problem_text(d->problemCode); }

/* ------------------------------------------------------------------ *
 * Version information straight from the driver .sys file.
 * ------------------------------------------------------------------ */
BOOL file_version_info(const wchar_t *path, wchar_t *ver, size_t cchVer,
                       wchar_t *company, size_t cchCompany,
                       wchar_t *desc, size_t cchDesc)
{
    DWORD     handle = 0, size = 0, trans = 0;
    VS_FIXEDFILEINFO *ffi = NULL;
    BYTE    *buf;
    wchar_t  sub[256], key[300];
    wchar_t *value = NULL;
    UINT     vlen = 0;
    FILETIME ft;
    BOOL     any = FALSE;

    if (ver && cchVer)     ver[0] = L'\0';
    if (company && cchCompany) company[0] = L'\0';
    if (desc && cchDesc)   desc[0] = L'\0';
    if (!path) return FALSE;

    size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return FALSE;
    buf = (BYTE *)xmalloc(size);
    if (!GetFileVersionInfoW(path, handle, size, buf)) {
        HeapFree(GetProcessHeap(), 0, buf);
        return FALSE;
    }

    if (ver && cchVer &&
        VerQueryValueW(buf, L"\\", (LPVOID *)&ffi, &vlen) && ffi) {
        _snwprintf(ver, cchVer, L"%u.%u.%u.%u",
                   HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                   HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
        ver[cchVer - 1] = L'\0';
        any = TRUE;
    }

    if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (LPVOID *)&trans, &vlen) && trans) {
        _snwprintf(sub, 256, L"\\StringFileInfo\\%04x%04x\\",
                   LOWORD(trans), HIWORD(trans));
        sub[255] = L'\0';
        if (company && cchCompany &&
            VerQueryValueW(buf, sub, (LPVOID *)&value, &vlen) && value)
            { wstr_copy(company, cchCompany, value, vlen); any = TRUE; }
        _snwprintf(key, 300, L"%sFileDescription", sub);
        if (desc && cchDesc &&
            VerQueryValueW(buf, key, (LPVOID *)&value, &vlen) && value)
            { wstr_copy(desc, cchDesc, value, vlen); any = TRUE; }
    }

    /* Drive date from the file timestamp, which is always present. */
    if (GetFileAttributesExW(path, GetFileExInfoStandard, NULL)) { /* no-op */ }

    HeapFree(GetProcessHeap(), 0, buf);
    (void)ft;
    return any;
}

/* Expand \SystemRoot\System32\drivers\hidclass.sys to a real path. */
static void expand_driver_path(const wchar_t *imagePath, wchar_t *out, size_t cch)
{
    wchar_t sys[MAX_PATH];
    DWORD  n;
    out[0] = L'\0';
    if (!imagePath || !imagePath[0]) return;
    n = GetWindowsDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    if (imagePath[0] == L'\\')
        _snwprintf(out, cch, L"%s%s", sys, imagePath);
    else
        ExpandEnvironmentStringsW(imagePath, out, (DWORD)cch);
    out[cch - 1] = L'\0';
}

static void load_driver_info(DeviceInfo *dev, HDEVINFO h, PSP_DEVINFO_DATA di)
{
    wchar_t service[64] = L"";
    wchar_t keyPath[512], imagePath[MAX_PATH] = L"";
    HKEY   hk;
    DWORD  cb;
    wchar_t full[MAX_PATH], ver[64], comp[128], desc[256];
    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (!get_str_prop_key(h, di, &KEY_SERVICE, service, 64))
        service[0] = L'\0';
    wstr_copy(dev->serviceName, 64, service, 63);
    if (!service[0]) return;

    _snwprintf(keyPath, 512, L"SYSTEM\\CurrentControlSet\\Services\\%s", service);
    keyPath[511] = L'\0';
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;
    cb = sizeof(imagePath);
    if (RegQueryValueExW(hk, L"ImagePath", NULL, NULL, (LPBYTE)imagePath, &cb) == ERROR_SUCCESS) {
        imagePath[MAX_PATH - 1] = L'\0';
        RegCloseKey(hk);
        expand_driver_path(imagePath, full, MAX_PATH);
        wstr_copy(dev->driverSys, 512, full, 511);
        if (file_version_info(full, ver, 64, comp, 128, desc, 256)) {
            wstr_copy(dev->driverVersion, 64,  ver, 63);
            wstr_copy(dev->driverProvider, 128, comp, 127);
            if (desc[0]) wstr_copy(dev->deviceDesc, 256, desc, 255);
        }
    } else {
        RegCloseKey(hk);
    }

    if (GetFileAttributesExW(dev->driverSys, GetFileExInfoStandard, &fad)) {
        SYSTEMTIME st;
        if (FileTimeToLocalFileTime(&fad.ftLastWriteTime, &fad.ftLastWriteTime) &&
            FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) {
            _snwprintf(dev->driverDate, 64, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
            dev->driverDate[63] = L'\0';
        }
    }
}

/* ------------------------------------------------------------------ */

void device_scan(DeviceInfo *out)
{
    HDEVINFO  hSet;
    SP_DEVICE_INTERFACE_DATA ifData;
    DWORD     ifIndex = 0;
    BOOL      matched = FALSE;

    ZeroMemory(out, sizeof(*out));

    hSet = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HID_LOCAL, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hSet == INVALID_HANDLE_VALUE) return;

    ZeroMemory(&ifData, sizeof(ifData));
    ifData.cbSize = sizeof(ifData);

    while (!matched &&
           SetupDiEnumDeviceInterfaces(hSet, NULL, &GUID_DEVINTERFACE_HID_LOCAL,
                                       ifIndex, &ifData)) {
        SP_DEVINFO_DATA di;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = NULL;
        DWORD   detailSize, req = 0;
        wchar_t instanceId[512];
        wchar_t buf[512];
        DWORD   need = 0, type = 0;
        DEVPROPKEY k;

        ifIndex++;

        di.cbSize = sizeof(di);
        if (!SetupDiGetDeviceInterfaceDetailW(hSet, &ifData, NULL, 0, &req, &di))
            continue;
        if (!hardware_ids_match(hSet, &di)) continue;

        if (!SetupDiGetDeviceInstanceIdW(hSet, &di, instanceId,
                                        (DWORD)(sizeof(instanceId) / sizeof(wchar_t)), NULL))
            continue;
        if (wcsncmp(instanceId, L"HID\\VID_30FA&PID_0400", 22) != 0) continue;

        detailSize = req;
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)xmalloc(detailSize);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hSet, &ifData, detail, detailSize, NULL, &di)) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }
        wstr_copy(out->interfacePath, 512, detail->DevicePath, wcslen(detail->DevicePath));
        HeapFree(GetProcessHeap(), 0, detail);

        wstr_copy(out->devInst, 64, instanceId, 63);
        out->found = TRUE;

        k = KEY_FRIENDLY_NAME;
        if (SetupDiGetDevicePropertyW(hSet, &di, &k, &type, (PBYTE)buf,
                                      sizeof(buf) - sizeof(wchar_t), &need, 0))
            wstr_copy(out->friendlyName, 256, buf, 255);
        k = KEY_DEVICE_DESC;
        if (SetupDiGetDevicePropertyW(hSet, &di, &k, &type, (PBYTE)buf,
                                      sizeof(buf) - sizeof(wchar_t), &need, 0))
            wstr_copy(out->deviceDesc, 256, buf, 255);
        k = KEY_MANUFACTURER;
        if (SetupDiGetDevicePropertyW(hSet, &di, &k, &type, (PBYTE)buf,
                                      sizeof(buf) - sizeof(wchar_t), &need, 0))
            wstr_copy(out->manufacturer, 128, buf, 127);

        {
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, di.DevInst, 0) == CR_SUCCESS) {
                out->devNodeStatus = status;
                out->problemCode   = problem;
                /* We only enumerate present devices, so "present" is implied by
                 * found. Avoid the DN_PRESENT/DN_STARTED bits: their encoding
                 * differs across SDK generations and mingw does not ship them. */
                out->present = TRUE;
                out->started = (problem == PREO_PROB_NONE);
            }
        }

        load_driver_info(out, hSet, &di);
        matched = TRUE;
    }

    SetupDiDestroyDeviceInfoList(hSet);
}

HDEVINFO device_open_by_instance(const wchar_t *devInst, PSP_DEVINFO_DATA di)
{
    HDEVINFO  hSet;
    SP_DEVICE_INTERFACE_DATA ifData;
    DWORD     ifIndex = 0;

    hSet = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HID_LOCAL, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hSet == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    ZeroMemory(&ifData, sizeof(ifData));
    ifData.cbSize = sizeof(ifData);

    while (SetupDiEnumDeviceInterfaces(hSet, NULL, &GUID_DEVINTERFACE_HID_LOCAL,
                                       ifIndex, &ifData)) {
        SP_DEVINFO_DATA local;
        wchar_t instanceId[512];
        ifIndex++;
        local.cbSize = sizeof(local);
        if (!SetupDiGetDeviceInterfaceDetailW(hSet, &ifData, NULL, 0, NULL, &local))
            continue;
        if (!SetupDiGetDeviceInstanceIdW(hSet, &local, instanceId, 512, NULL))
            continue;
        if (_wcsicmp(instanceId, devInst) == 0) {
            *di = local;
            return hSet;
        }
    }
    SetupDiDestroyDeviceInfoList(hSet);
    return INVALID_HANDLE_VALUE;
}

/* ------------------------------------------------------------------ *
 * Enumerate every present HID collection. Used by --diagnose so that a
 * user whose mouse is not being matched can see what Windows actually
 * reports and feed it back.
 * ------------------------------------------------------------------ */
void device_list_all_hid(wchar_t *buf, size_t cch, int *outLen)
{
    HDEVINFO  hSet;
    SP_DEVICE_INTERFACE_DATA ifData;
    DWORD     ifIndex = 0;
    size_t    used = 0;

    if (outLen) *outLen = 0;
    if (!buf || cch < 2) return;
    buf[0] = L'\0';

    hSet = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HID_LOCAL, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hSet == INVALID_HANDLE_VALUE) {
        wstr_copy(buf, cch, L"  (SetupDiGetClassDevs failed)\n", 60);
        if (outLen) *outLen = (int)wcslen(buf);
        return;
    }

    ZeroMemory(&ifData, sizeof(ifData));
    ifData.cbSize = sizeof(ifData);

    while (SetupDiEnumDeviceInterfaces(hSet, NULL, &GUID_DEVINTERFACE_HID_LOCAL,
                                       ifIndex, &ifData)) {
        SP_DEVINFO_DATA di;
        wchar_t instanceId[512], line[1024], idbuf[1024];
        DWORD   need = 0, type = 0, status = 0, problem = 0;
        BOOL    match;

        ifIndex++;
        di.cbSize = sizeof(di);
        if (!SetupDiGetDeviceInterfaceDetailW(hSet, &ifData, NULL, 0, NULL, &di))
            continue;
        if (!SetupDiGetDeviceInstanceIdW(hSet, &di, instanceId, 512, NULL))
            continue;

        match = hardware_ids_match(hSet, &di);
        CM_Get_DevNode_Status(&status, &problem, di.DevInst, 0);

        idbuf[0] = L'\0';
        {
            DEVPROPKEY key = KEY_HARDWARE_IDS;
            if (SetupDiGetDevicePropertyW(hSet, &di, &key, &type, (PBYTE)idbuf,
                                          sizeof(idbuf) - sizeof(wchar_t), &need, 0)) {
                /* collapse the MULTI_SZ to a readable list */
                wchar_t *p = idbuf;
                size_t   w = 0;
                while (*p) {
                    size_t n = wcslen(p);
                    if (w + n + 3 >= cch) break;
                    wstr_copy(buf + w, cch - w, p, n);
                    w += n;
                    wstr_copy(buf + w, cch - w, L", ", 2); w += 2;
                    p += n + 1;
                }
                if (w >= 2) buf[w - 2] = L'\0';
            }
        }

        _snwprintf(line, 1024,
                   L"  %s%s\n     instance : %s\n     ids      : %s\n     problem  : %lu%s\n",
                   match ? L"* " : L"  ", instanceId,
                   instanceId,
                   idbuf[0] ? idbuf : L"(none)",
                   problem,
                   match ? L"   <== matches the MG14" : L"");
        line[1023] = L'\0';

        {
            size_t n = wcslen(line);
            if (used + n + 1 < cch) {
                wstr_copy(buf + used, cch - used, line, n);
                used += n;
            } else {
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hSet);
    if (outLen) *outLen = (int)used;
}
