/*
 * diagnostics.c - "PreoMG14Driver.exe --diagnose"
 *
 * A console report of everything this program can see about the machine: the
 * OS build, the real (unshimmed) version, every present HID collection, and a
 * full description of the MG14 if one is attached.
 *
 * This exists because the program cannot be exercised in a build environment.
 * Running this on the machine that actually has the mouse turns "I think it
 * works" into a report that either confirms the device is being matched, or
 * shows exactly what Windows is reporting instead so the match can be fixed.
 */
#include "app.h"
#include <stdarg.h>
#include <stdio.h>

typedef LONG (WINAPI *PfnRtlGetVersion)(LPLONG major, LPLONG minor,
                                        LPLONG build, LPLONG platform);

static FILE *g_out;

static void emit(const wchar_t *fmt, ...)
{
    wchar_t line[2048];
    va_list ap;
    if (!g_out) return;
    va_start(ap, fmt);
    _vsnwprintf(line, 2048, fmt, ap);
    va_end(ap);
    line[2047] = L'\0';
    fwprintf(g_out, L"%s\n", line);
    fflush(g_out);
}

BOOL diagnostics_requested(LPWSTR cmdLine)
{
    return cmdLine && wcsstr(cmdLine, L"--diagnose") != NULL;
}

static void section(const wchar_t *title)
{
    emit(L"");
    emit(L"== %ls ==============================================", title);
}

static void report_os(void)
{
    wchar_t buf[256];
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PfnRtlGetVersion rtl = ntdll ? (PfnRtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    LONG major = 0, minor = 0, build = 0, plat = 2;
    SYSTEM_INFO si;

    /* GetVersionEx is shimmed by the application manifest and will happily
     * claim Windows 10 on Windows 11. RtlGetVersion tells the truth, so use it
     * when we can and flag the fallback when we cannot. */
    if (rtl && rtl(&major, &minor, &build, &plat) == 0) {
        emit(L"  Windows      : %ld.%ld build %ld", major, minor, build);
    } else {
        OSVERSIONINFOW vi;
        ZeroMemory(&vi, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
        GetVersionExW(&vi);
        emit(L"  Windows      : %lu.%lu build %lu   (RtlGetVersion unavailable,"
             L" the manifest may be shimming this value)",
             vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    }

    ZeroMemory(&si, sizeof(si));
    GetNativeSystemInfo(&si);
    {
        wchar_t arch[32];
        if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
            wstr_copy(arch, 32, L"x64", 3);
        else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
            wstr_copy(arch, 32, L"ARM64", 5);
        else
            wstr_copy(arch, 32, L"x86", 3);
        emit(L"  Architecture : %s", arch);
    }

    /* Both of these take the buffer size as an in/out LPDWORD, not by value. */
    {
        DWORD n = 256;
        if (GetComputerNameW(buf, &n)) { buf[n] = L'\0'; emit(L"  Computer     : %s", buf); }
        n = 256;
        if (GetUserNameW(buf, &n))      { buf[n] = L'\0'; emit(L"  User         : %s", buf); }
    }
}

static void report_device(void)
{
    DeviceInfo d;
    const wchar_t *inf;
    size_t i;

    device_scan(&d);
    if (!d.found) {
        emit(L"  RESULT: the Preo MG14 was NOT found.");
        emit(L"  Expected any of:");
        for (i = 0; i < g_preoHardwareIdCount; i++)
            emit(L"    %s", g_preoHardwareIds[i]);
        emit(L"  The list of every HID collection Windows reported follows in the");
        emit(L"  'All HID collections' section below. Check the VID/PID there.");
        return;
    }

    hid_query(&d);
    emit(L"  RESULT: matched.");
    emit(L"    instance      : %s", d.devInst);
    emit(L"    friendly name : %s", d.friendlyName[0] ? d.friendlyName : L"(none)");
    emit(L"    description   : %s", d.deviceDesc[0] ? d.deviceDesc : L"(none)");
    emit(L"    manufacturer  : %s", d.manufacturer[0] ? d.manufacturer : L"(none)");
    emit(L"    product       : %s", d.productName[0] ? d.productName : L"(none)");
    emit(L"    serial        : %s", d.serial[0] ? d.serial : L"(none)");
    emit(L"    VID:PID:REV   : %04X:%04X:%04X", d.vendorId, d.productId, d.versionNumber);
    emit(L"    usage page    : 0x%04X", d.usagePage);
    emit(L"    usage         : 0x%04X", d.usage);
    emit(L"    buttons       : %u", d.numberButtons);
    emit(L"    problem code  : %lu  (%s)", d.problemCode, device_problem_text(&d));
    emit(L"  Driver:");
    emit(L"    service       : %s", d.serviceName[0] ? d.serviceName : L"(none)");
    emit(L"    file          : %s", d.driverSys[0] ? d.driverSys : L"(none)");
    emit(L"    version       : %s", d.driverVersion[0] ? d.driverVersion : L"(none)");
    emit(L"    provider      : %s", d.driverProvider[0] ? d.driverProvider : L"(none)");
    emit(L"    file date     : %s", d.driverDate[0] ? d.driverDate : L"(none)");
    inf = inbox_hid_inf_path();
    emit(L"    repair package: %s", inf ? inf : L"(none found - repair is unavailable)");
}

static void report_pointer_settings(void)
{
    HKEY hk;
    wchar_t dir[MAX_PATH];
    struct { const wchar_t *name; const wchar_t *path; } vals[] = {
        { L"MouseSpeed",            L"Control Panel\\Mouse" },
        { L"MouseThreshold1",       L"Control Panel\\Mouse" },
        { L"MouseThreshold2",       L"Control Panel\\Mouse" },
        { L"DoubleClickSpeed",      L"Control Panel\\Mouse" },
        { L"SwapMouseButtons",      L"Control Panel\\Mouse" },
        { L"MouseClickLockEnable",  L"Control Panel\\Mouse" },
    };
    size_t i;

    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        DWORD v = 0, cb = sizeof(v), type = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, vals[i].path, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            if (RegQueryValueExW(hk, vals[i].name, NULL, &type, (LPBYTE)&v, &cb) == ERROR_SUCCESS)
                emit(L"  HKCU\\%s\\%s = %lu", vals[i].path, vals[i].name, v);
            else
                emit(L"  HKCU\\%s\\%s = (not set)", vals[i].path, vals[i].name);
            RegCloseKey(hk);
        }
    }
    if (get_settings_dir(dir, MAX_PATH)) emit(L"  Settings file : %s\\settings.ini", dir);
    emit(L"  (this program's own settings file, created only when you save)");
}

int run_diagnostics(LPWSTR cmdLine)
{
    wchar_t *list;
    int      len = 0;
    int      i;
    wchar_t  outPath[MAX_PATH] = L"";

    (void)cmdLine;

    /* The binary is a GUI app, so borrow the console that launched us. */
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        _snwprintf(outPath, MAX_PATH, L"%sPreoMG14Diagnostic.txt", tmp);
        outPath[MAX_PATH - 1] = L'\0';
        g_out = _wfopen(outPath, L"w");
        if (!g_out) return 1;
    } else {
        g_out = stdout;
    }

    emit(L"========================================================");
    emit(L" %s %s - diagnostic report", APP_NAME, APP_VERSION);
    emit(L"========================================================");

    section(L"System");
    report_os();
    emit(L"  Elevated     : %s", is_elevated() ? L"yes" : L"no (not required for this report)");

    section(L"Preo MG14");
    report_device();

    section(L"All HID collections");
    emit(L"  A '*' marks a device that matches the MG14 hardware IDs.");
    list = (wchar_t *)xmalloc(64 * 1024);
    if (list) {
        device_list_all_hid(list, 64 * 1024, &len);
        for (i = 0; i < len; )
        {
            int j = i;
            while (j < len && list[j] != L'\n') j++;
            {
                wchar_t save = list[j];
                list[j] = L'\0';
                fwprintf(g_out, L"%s", list + i);
                fputc('\n', g_out);
                list[j] = save;
            }
            i = j + 1;
        }
        HeapFree(GetProcessHeap(), 0, list);
    }

    section(L"Windows pointer settings");
    report_pointer_settings();

    section(L"Done");
    emit(L"  If the MG14 was matched, open the program and look at the Device tab.");
    emit(L"  If it was not, the 'All HID collections' section shows what Windows");
    emit(L"  is actually reporting.");
    emit(L"");

    if (g_out) { fclose(g_out); g_out = NULL; }
    return 0;
}
