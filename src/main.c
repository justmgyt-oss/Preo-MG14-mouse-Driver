/*
 * main.c - Application shell: window, tabs, and the four settings pages.
 *
 * Everything runs as a normal user process. The only operations that need
 * Administrator rights (restarting the driver, reinstalling it, removing the
 * devnode) are the three buttons on the Device page, and pressing one
 * re-launches this same executable through ShellExecute("runas") so the user
 * always gets a UAC prompt that names a specific action.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <stdarg.h>
#include <stdio.h>

#define WM_APP_TIMER_RESCAN (WM_APP + 1)
#define RESCAN_INTERVAL_MS  2500

/* Mouse class GUID - the key Windows uses for per-device pointer settings. */
static const GUID GUID_MOUSE_CLASS = {
    0x4d36e96f, 0xe325, 0x11ce,
    { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};

static const wchar_t *RUN_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *RUN_VALUE = L"PreoMG14Driver";

/* ------------------------------------------------------------------ */

static HWND  g_main, g_tab, g_status;
static HFONT g_font, g_fontBold;
static DeviceInfo g_dev;
static Settings   g_settings;
static int        g_page = 0;
static int        g_deviceRow = 0;
static int        g_remappable = 5;   /* buttons we offer rows for */

/* ------------------------------------------------------------------ */
/* Registry: the Windows pointer settings we drive on the user's behalf. */
/* ------------------------------------------------------------------ */

static BOOL write_dword_path(HKEY root, const wchar_t *path, const wchar_t *name, DWORD v)
{
    HKEY hk;
    BOOL ok = FALSE;
    if (RegCreateKeyExW(root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ok = (RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v)) == ERROR_SUCCESS);
        RegCloseKey(hk);
    }
    return ok;
}

static DWORD read_dword_path(HKEY root, const wchar_t *path, const wchar_t *name, DWORD def)
{
    HKEY  hk;
    DWORD v = def, cb = sizeof(v), type = 0;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hk, name, NULL, &type, (LPBYTE)&v, &cb) != ERROR_SUCCESS) v = def;
        RegCloseKey(hk);
    }
    return v;
}

static wchar_t *per_device_pointer_key(void)
{
    static wchar_t path[512];
    _snwprintf(path, 512,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\Mouse"
        L"\\{%08lX-%04X-%04X-%04X-%012llX}\\Device Parameters\\Pointer",
        (unsigned long)GUID_MOUSE_CLASS.Data1, GUID_MOUSE_CLASS.Data2,
        GUID_MOUSE_CLASS.Data3, GUID_MOUSE_CLASS.Data4[0] * 256 + GUID_MOUSE_CLASS.Data4[1],
        (unsigned long long)(((unsigned long long)GUID_MOUSE_CLASS.Data4[2] << 40) |
                             ((unsigned long long)GUID_MOUSE_CLASS.Data4[3] << 32) |
                             ((unsigned long long)GUID_MOUSE_CLASS.Data4[4] << 24) |
                             ((unsigned long long)GUID_MOUSE_CLASS.Data4[5] << 16) |
                             ((unsigned long long)GUID_MOUSE_CLASS.Data4[6] << 8) |
                             ((unsigned long long)GUID_MOUSE_CLASS.Data4[7])));
    path[511] = L'\0';
    return path;
}

static void read_pointer_settings(Settings *s)
{
    const wchar_t *key = per_device_pointer_key();
    DWORD speed, t1, t2;
    speed = read_dword_path(HKEY_CURRENT_USER, key, L"MouseSpeed", 10);
    t1    = read_dword_path(HKEY_CURRENT_USER, key, L"MouseThreshold1", 0);
    t2    = read_dword_path(HKEY_CURRENT_USER, key, L"MouseThreshold2", 0);
    s->pointerSpeed     = (int)speed;
    s->enhancePrecision = (t1 == 0 && t2 == 0);
    s->doubleClickSpeed = (int)read_dword_path(HKEY_CURRENT_USER, L"Control Panel\\Mouse",
                                               L"DoubleClickSpeed", 500);
    s->swapXY = read_dword_path(HKEY_CURRENT_USER, L"Control Panel\\Mouse",
                                L"SwapMouseButtons", 0) ? TRUE : FALSE;
    s->enhanceClickPrecision =
        read_dword_path(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseClickLockEnable", 0) ? TRUE : FALSE;
}

BOOL apply_pointer_settings(const Settings *s, BOOL perDevice, HWND hWnd)
{
    const wchar_t *key = perDevice ? per_device_pointer_key() : L"Control Panel\\Mouse";
    DWORD th1 = s->enhancePrecision ? 0 : 4;
    DWORD th2 = s->enhancePrecision ? 0 : 8;

    write_dword_path(HKEY_CURRENT_USER, key, L"MouseSpeed", (DWORD)s->pointerSpeed);
    write_dword_path(HKEY_CURRENT_USER, key, L"MouseThreshold1", th1);
    write_dword_path(HKEY_CURRENT_USER, key, L"MouseThreshold2", th2);
    if (!perDevice) {
        write_dword_path(HKEY_CURRENT_USER, key, L"DoubleClickSpeed", (DWORD)s->doubleClickSpeed);
        write_dword_path(HKEY_CURRENT_USER, key, L"SwapMouseButtons", s->swapXY ? 1u : 0u);
        /* "Enhance click precision" is the Ctrl-to-lock setting Windows calls
         * MouseClickLockEnable. */
        write_dword_path(HKEY_CURRENT_USER, key, L"MouseClickLockEnable",
                         s->enhanceClickPrecision ? 1u : 0u);
    }

    /* Push the values into the live input stack and tell everyone else. */
    SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(ULONG_PTR)s->pointerSpeed, 0);
    if (!perDevice)
        SystemParametersInfoW(SPI_SETDOUBLECLICKTIME, 0,
                              (PVOID)(ULONG_PTR)s->doubleClickSpeed, 0);
    SystemParametersInfoW(SPI_SETSWITCHBUTTON, 0,
                          (PVOID)(ULONG_PTR)(s->swapXY ? 1 : 0), 0);

    {
        wchar_t note[128];
        _snwprintf(note, 128, L"Pointer settings applied (%s).",
                   perDevice ? L"this mouse only" : L"all pointing devices");
        SendMessageTimeoutW(hWnd, WM_SETTINGCHANGE, 0, (LPARAM)note,
                            SMTO_ABORTIFHUNG, 1500, NULL);
    }
    return TRUE;
}

BOOL apply_autostart(BOOL enable)
{
    HKEY hk;
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 8];

    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &hk, NULL) != ERROR_SUCCESS)
        return FALSE;
    if (!enable) {
        RegDeleteValueW(hk, RUN_VALUE);
        RegCloseKey(hk);
        return TRUE;
    }
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) { RegCloseKey(hk); return FALSE; }
    _snwprintf(cmd, MAX_PATH + 8, L"\"%s\"", exe);
    cmd[MAX_PATH + 7] = L'\0';
    {
        LONG r = RegSetValueExW(hk, RUN_VALUE, 0, REG_SZ,
                                (const BYTE *)cmd,
                                (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
        return r == ERROR_SUCCESS;
    }
}

/* ------------------------------------------------------------------ */
/* Control helpers                                                     */
/* ------------------------------------------------------------------ */

static HWND add_ctl(const wchar_t *cls, const wchar_t *text, DWORD style,
                    int x, int y, int w, int height, int id)
{
    HWND ctrl = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                x, y, w, height, g_main, (HMENU)(INT_PTR)id,
                                GetModuleHandleW(NULL), NULL);
    if (ctrl) SendMessageW(ctrl, WM_SETFONT, (WPARAM)g_font, TRUE);
    return ctrl;
}

static void set_status(const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 512, fmt, ap);
    va_end(ap);
    buf[511] = L'\0';
    SetWindowTextW(g_status, buf);
}

static void set_text(int id, const wchar_t *t)
{
    HWND h = GetDlgItem(g_main, id);
    if (h) SetWindowTextW(h, t);
}

static BOOL get_check(int id) { return IsDlgButtonChecked(g_main, id) == BST_CHECKED; }
static void set_check(int id, BOOL on)
{
    CheckDlgButton(g_main, id, on ? BST_CHECKED : BST_UNCHECKED);
}

/* ------------------------------------------------------------------ */
/* Device page                                                         */
/* ------------------------------------------------------------------ */

static void lv_add(const wchar_t *k, const wchar_t *v)
{
    HWND  lv = GetDlgItem(g_main, ID_DEV_LIST);
    LVITEMW it;
    wchar_t num[64];

    if (!lv) return;
    if (!k) {                       /* section header row */
        wstr_copy(num, 64, v, wcslen(v));
        ZeroMemory(&it, sizeof(it));
        it.mask     = LVIF_TEXT;
        it.iItem    = g_deviceRow++;
        it.pszText  = num;
        it.iSubItem = 0;
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        return;
    }
    wstr_copy(num, 64, k, wcslen(k));
    ZeroMemory(&it, sizeof(it));
    it.mask     = LVIF_TEXT;
    it.iItem    = g_deviceRow++;
    it.pszText  = num;
    it.iSubItem = 0;
    SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);

    ZeroMemory(&it, sizeof(it));
    it.mask     = LVIF_TEXT;
    it.iItem    = g_deviceRow - 1;
    it.iSubItem = 1;
    it.pszText  = (LPWSTR)v;
    SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&it);
}

static void lv_addf(const wchar_t *k, const wchar_t *fmt, ...)
{
    wchar_t buf[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 768, fmt, ap);
    va_end(ap);
    buf[767] = L'\0';
    lv_add(k, buf);
}

static void refresh_device(void)
{
    HWND lv = GetDlgItem(g_main, ID_DEV_LIST);
    HWND st = GetDlgItem(g_main, ID_DEV_STATUS);
    wchar_t buf[256];
    size_t i;

    if (lv) SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
    g_deviceRow = 0;

    device_scan(&g_dev);
    if (g_dev.found) hid_query(&g_dev);

    if (!g_dev.found) {
        set_text(ID_DEV_STATUS, L"MG14 not detected");
        if (st) SetWindowTextW(st, L"MG14 not detected");
        lv_add(NULL, L"Device");
        lv_addf(L"Status", L"Not connected, or Windows has not enumerated it yet.");
        lv_add(NULL, L"Expected hardware IDs");
        for (i = 0; i < g_preoHardwareIdCount; i++)
            lv_addf(L"", L"%s", g_preoHardwareIds[i]);
        lv_add(NULL, L"Next step");
        lv_addf(L"", L"Plug the mouse into a USB port, then press Rescan.");
        set_status(L"Waiting for the Preo MG14...");
        SetWindowTextW(GetDlgItem(g_main, ID_BTN_RESTART),   L"Restart driver");
        EnableWindow(GetDlgItem(g_main, ID_BTN_RESTART),   FALSE);
        EnableWindow(GetDlgItem(g_main, ID_BTN_REINSTALL), FALSE);
        EnableWindow(GetDlgItem(g_main, ID_BTN_REMOVE),    FALSE);
        return;
    }

    _snwprintf(buf, 256, L"MG14 detected  -  %04X:%04X", g_dev.vendorId, g_dev.productId);
    set_text(ID_DEV_STATUS, buf);
    if (st) SetWindowTextW(st, buf);

    lv_add(NULL, L"Identity");
    if (g_dev.friendlyName[0])  lv_addf(L"Friendly name",   L"%s", g_dev.friendlyName);
    if (g_dev.deviceDesc[0])    lv_addf(L"Description",     L"%s", g_dev.deviceDesc);
    if (g_dev.manufacturer[0])  lv_addf(L"Manufacturer",    L"%s", g_dev.manufacturer);
    if (g_dev.productName[0])   lv_addf(L"Product string",  L"%s", g_dev.productName);
    if (g_dev.serial[0])        lv_addf(L"Serial number",   L"%s", g_dev.serial);
    lv_addf(L"Vendor ID",  L"%04X", g_dev.vendorId);
    lv_addf(L"Product ID", L"%04X", g_dev.productId);
    lv_addf(L"Revision",   L"%04X", g_dev.versionNumber);
    lv_addf(L"Device instance", L"%s", g_dev.devInst);

    lv_add(NULL, L"HID collection");
    lv_addf(L"Usage page",  L"0x%04X (%s)", g_dev.usagePage,
            g_dev.usagePage == PREO_USAGE_PAGE ? L"Generic Desktop" : L"other");
    lv_addf(L"Usage",       L"0x%04X (%s)", g_dev.usage,
            g_dev.usage == PREO_USAGE ? L"Mouse" : L"other");
    if (g_dev.numberButtons)
        lv_addf(L"Buttons",  L"%u", g_dev.numberButtons);
    else
        lv_addf(L"Buttons",  L"(not reported)");

    lv_add(NULL, L"Driver");
    if (g_dev.serviceName[0])    lv_addf(L"Service",        L"%s", g_dev.serviceName);
    if (g_dev.driverSys[0])      lv_addf(L"Driver file",    L"%s", g_dev.driverSys);
    if (g_dev.driverVersion[0])  lv_addf(L"Version",        L"%s", g_dev.driverVersion);
    if (g_dev.driverProvider[0]) lv_addf(L"Provider",       L"%s", g_dev.driverProvider);
    if (g_dev.driverDate[0])     lv_addf(L"File date",      L"%s", g_dev.driverDate);
    {
        const wchar_t *inf = inbox_hid_inf_path();
        lv_addf(L"Package used for repair", L"%s", inf ? inf : L"(none found)");
    }

    lv_add(NULL, L"Status");
    lv_addf(L"Result", L"%s", device_problem_text(&g_dev));
    lv_addf(L"Problem code", L"%lu", g_dev.problemCode);
    lv_addf(L"Start state", L"%s", g_dev.started ? L"Running" : L"Not running");

    lv_add(NULL, L"Hardware IDs reported by this device");
    for (i = 0; i < g_preoHardwareIdCount; i++)
        lv_addf(L"", L"%s", g_preoHardwareIds[i]);

    set_status(g_dev.started ? L"MG14 is working with the standard Windows HID driver."
                             : L"MG14 found, but Windows reports a problem with it.");
    EnableWindow(GetDlgItem(g_main, ID_BTN_RESTART),   TRUE);
    EnableWindow(GetDlgItem(g_main, ID_BTN_REINSTALL), TRUE);
    EnableWindow(GetDlgItem(g_main, ID_BTN_REMOVE),    TRUE);
}

/* ------------------------------------------------------------------ */
/* Buttons page                                                        */
/* ------------------------------------------------------------------ */

static ButtonAction combo_action(HWND c)
{
    int i = (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= BTN_COUNT) return BTN_DISABLED;
    return (ButtonAction)i;
}

static void refresh_buttons_page(void)
{
    HWND c1 = GetDlgItem(g_main, ID_CMB_X1);
    HWND c2 = GetDlgItem(g_main, ID_CMB_X2);
    HWND c3 = GetDlgItem(g_main, ID_CMB_DPI);
    HWND l3 = GetDlgItem(g_main, ID_LBL_DPI);
    int i;

    set_check(ID_BTN_REMAP_ON, g_settings.remapEnabled);
    set_check(ID_CHK_WHEEL, g_settings.wheelInvert);

    SendMessageW(c1, CB_RESETCONTENT, 0, 0);
    SendMessageW(c2, CB_RESETCONTENT, 0, 0);
    SendMessageW(c3, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < BTN_COUNT; i++) {
        SendMessageW(c1, CB_ADDSTRING, 0, (LPARAM)button_action_name((ButtonAction)i));
        SendMessageW(c2, CB_ADDSTRING, 0, (LPARAM)button_action_name((ButtonAction)i));
        SendMessageW(c3, CB_ADDSTRING, 0, (LPARAM)button_action_name((ButtonAction)i));
    }
    SendMessageW(c1, CB_SETCURSEL, (WPARAM)g_settings.x1Action, 0);
    SendMessageW(c2, CB_SETCURSEL, (WPARAM)g_settings.x2Action, 0);
    SendMessageW(c3, CB_SETCURSEL, (WPARAM)g_settings.dpiAction, 0);

    /* A mouse only exposes a button to Windows if it reports one in its HID
     * report descriptor. The MG14 is expected to report 6; if it reports
     * fewer, say so rather than offering a control that cannot work. */
    if (g_dev.found && g_dev.numberButtons) {
        wchar_t label[128];
        _snwprintf(label, 128, L"DPI switch button   (this mouse reports %u buttons)",
                   g_dev.numberButtons);
        label[127] = L'\0';
        if (l3) SetWindowTextW(l3, label);
        EnableWindow(c3, g_remappable > 5);
    } else {
        if (l3) SetWindowTextW(l3, L"DPI switch button");
        EnableWindow(c3, TRUE);
    }
}

static void apply_remap(void)
{
    remap_stop();
    if (g_settings.remapEnabled) {
        if (!remap_start(&g_settings)) {
            g_settings.remapEnabled = FALSE;
            set_check(ID_BTN_REMAP_ON, FALSE);
            settings_save(&g_settings);
            MessageBoxW(g_main,
                L"Windows refused to install the mouse hook.\n\n"
                L"Button remapping needs this program to be running. Try running it "
                L"as administrator, or restart your session and try again.",
                APP_NAME, MB_ICONWARNING);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pointer page                                                        */
/* ------------------------------------------------------------------ */

static void refresh_pointer_page(void)
{
    static const int dclicks[] = { 250, 300, 350, 400, 450, 500, 550, 600, 700, 800, 900 };
    HWND tb = GetDlgItem(g_main, ID_SLD_SPEED);
    HWND cb = GetDlgItem(g_main, ID_CMB_DCLICK);
    wchar_t buf[64];
    int i;

    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(1, 20));
    SendMessageW(tb, TBM_SETPOS,   TRUE, (LPARAM)g_settings.pointerSpeed);
    _snwprintf(buf, 64, L"%d", g_settings.pointerSpeed);
    set_text(ID_TXT_SPEED, buf);

    set_check(ID_CHK_ENHANCE,  g_settings.enhancePrecision);
    set_check(ID_CHK_SWAPXY,   g_settings.swapXY);
    set_check(ID_CHK_ENHCLICK, g_settings.enhanceClickPrecision);
    set_check(ID_CHK_AUTOSTART, g_settings.startWithWindows);

    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < (int)(sizeof(dclicks) / sizeof(dclicks[0])); i++) {
        _snwprintf(buf, 64, L"%d ms", dclicks[i]);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)buf);
        if (dclicks[i] == g_settings.doubleClickSpeed) {
            SendMessageW(cb, CB_SETCURSEL, (WPARAM)i, 0);
            g_settings.doubleClickSpeed = dclicks[i];
        }
    }
    if (SendMessageW(cb, CB_GETCURSEL, 0, 0) == CB_ERR)
        SendMessageW(cb, CB_SETCURSEL, 6, 0);   /* 500 ms */
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static RECT page_rect(int idx)
{
    RECT r;
    GetWindowRect(g_tab, &r);
    MapWindowPoints(NULL, g_main, (POINT *)&r, 2);
    r.left  += 6;
    r.top   += 28;
    r.right -= 6;
    r.bottom -= 8;
    (void)idx;
    return r;
}

static void show_hide_range(int first, int last, BOOL show)
{
    int id;
    for (id = first; id <= last; id++) {
        HWND h = GetDlgItem(g_main, id);
        if (h) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
    }
}

static void layout_page(int page)
{
    RECT r = page_rect(page);
    int  x = r.left, y = r.top, w = r.right - r.left;
    int  cx, cy;

    show_hide_range(110, 199, page == 0);
    show_hide_range(210, 299, page == 1);
    show_hide_range(300, 399, page == 2);
    show_hide_range(400, 499, page == 3);

    if (page == 0) {
        MoveWindow(GetDlgItem(g_main, ID_DEV_STATUS), x, y, w, 26, TRUE);
        MoveWindow(GetDlgItem(g_main, ID_DEV_LIST), x, y + 32, w, r.bottom - y - 74, TRUE);
        cy = r.bottom - 34;
        cx = x;
        MoveWindow(GetDlgItem(g_main, ID_BTN_RESTART),   cx, cy, 130, 28, TRUE); cx += 136;
        MoveWindow(GetDlgItem(g_main, ID_BTN_REINSTALL), cx, cy, 130, 28, TRUE); cx += 136;
        MoveWindow(GetDlgItem(g_main, ID_BTN_REMOVE),    cx, cy, 150, 28, TRUE);
    } else if (page == 1) {
        y += 6;
        MoveWindow(GetDlgItem(g_main, ID_BTN_REMAP_ON), x, y, w, 22, TRUE); y += 32;
        MoveWindow(GetDlgItem(g_main, ID_LBL_X1), x, y, 190, 22, TRUE);
        MoveWindow(GetDlgItem(g_main, ID_CMB_X1),  x + 200, y - 4, 260, 300, TRUE); y += 34;
        MoveWindow(GetDlgItem(g_main, ID_LBL_X2), x, y, 190, 22, TRUE);
        MoveWindow(GetDlgItem(g_main, ID_CMB_X2),  x + 200, y - 4, 260, 300, TRUE); y += 34;
        MoveWindow(GetDlgItem(g_main, ID_LBL_DPI), x, y, 190, 22, TRUE);
        MoveWindow(GetDlgItem(g_main, ID_CMB_DPI), x + 200, y - 4, 260, 300, TRUE); y += 40;
        MoveWindow(GetDlgItem(g_main, ID_CHK_WHEEL), x, y, w, 22, TRUE); y += 34;
        MoveWindow(GetDlgItem(g_main, ID_TXT_BTN_INFO), x, y, w, r.bottom - y - 6, TRUE);
    } else if (page == 2) {
        y += 8;
        MoveWindow(GetDlgItem(g_main, ID_LBL_SPEED), x, y, 130, 22, TRUE);
        MoveWindow(GetDlgItem(g_main, ID_SLD_SPEED), x + 135, y + 2, w - 220, 28, TRUE);
        MoveWindow(GetDlgItem(g_main, ID_TXT_SPEED), x + w - 80, y + 4, 60, 22, TRUE);
        y += 44;
        MoveWindow(GetDlgItem(g_main, ID_CHK_ENHANCE),  x, y, w, 22, TRUE); y += 28;
        MoveWindow(GetDlgItem(g_main, ID_CHK_SWAPXY),   x, y, w, 22, TRUE); y += 28;
        MoveWindow(GetDlgItem(g_main, ID_LBL_DCLICK), x, y, 190, 22, TRUE);
        MoveWindow(GetDlgItem(g_main, ID_CMB_DCLICK), x + 200, y - 4, 140, 200, TRUE); y += 34;
        MoveWindow(GetDlgItem(g_main, ID_CHK_ENHCLICK), x, y, w, 22, TRUE); y += 34;
        MoveWindow(GetDlgItem(g_main, ID_CHK_PERDEVICE), x, y, w, 22, TRUE); y += 32;
        MoveWindow(GetDlgItem(g_main, ID_CHK_AUTOSTART), x, y, w, 22, TRUE); y += 40;
        MoveWindow(GetDlgItem(g_main, ID_BTN_APPLY_PTR), x, y, 170, 30, TRUE);
        MoveWindow(GetDlgItem(g_main, ID_BTN_RESET_PTR), x + 178, y, 170, 30, TRUE);
    } else {
        MoveWindow(GetDlgItem(g_main, ID_ABOUT_TEXT), x, y, w, r.bottom - y - 6, TRUE);
    }
}

static void on_resize(void)
{
    RECT c;
    GetClientRect(g_main, &c);
    MoveWindow(g_tab, 10, 10, c.right - 20, c.bottom - 74, TRUE);
    MoveWindow(g_status, 0, c.bottom - 26, c.right, 26, TRUE);
    MoveWindow(GetDlgItem(g_main, ID_BTN_RESCAN), 10, c.bottom - 42, 110, 28, TRUE);
    MoveWindow(GetDlgItem(g_main, ID_BTN_CLOSE), c.right - 120, c.bottom - 42, 110, 28, TRUE);
    layout_page(g_page);
}

/* ------------------------------------------------------------------ */

static void create_controls(void)
{
    NONCLIENTMETRICSW ncm;
    HINSTANCE inst = GetModuleHandleW(NULL);

    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        LOGFONTW lf = ncm.lfMessageFont;
        g_font = CreateFontIndirectW(&lf);
        lf.lfHeight = -MulDiv(15, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72);
        lf.lfWeight = FW_SEMIBOLD;
        g_fontBold = CreateFontIndirectW(&lf);
    } else {
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g_fontBold = g_font;
    }

    g_tab = add_ctl(L"SysTabControl", NULL, TCS_TABS, 0, 0, 0, 0, ID_TAB);
    {
        TCITEMW tci;
        static const wchar_t *tabs[] = { L"Device", L"Buttons", L"Pointer", L"About" };
        int i;
        ZeroMemory(&tci, sizeof(tci));
        tci.mask = TCIF_TEXT;
        for (i = 0; i < 4; i++) {
            tci.pszText = (LPWSTR)tabs[i];
            TabCtrl_InsertItem(g_tab, i, &tci);
        }
    }

    /* --- Device page --- */
    add_ctl(L"STATIC", L"MG14 not detected", SS_LEFT | SS_CENTERIMAGE,
            0, 0, 0, 0, ID_DEV_STATUS);
    SendMessageW(GetDlgItem(g_main, ID_DEV_STATUS), WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    add_ctl(L"SysListView32", NULL, LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            0, 0, 0, 0, ID_DEV_LIST);
    {
        HWND lv = GetDlgItem(g_main, ID_DEV_LIST);
        LVCOLUMNW col;
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = (LPWSTR)L"Property";
        col.cx = 210;
        col.iSubItem = 0;
        ListView_InsertColumn(lv, 0, &col);
        col.pszText = (LPWSTR)L"Value";
        col.cx = 430;
        col.iSubItem = 1;
        ListView_InsertColumn(lv, 1, &col);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        EnableWindow(lv, FALSE);        /* read-only */
    }
    add_ctl(L"BUTTON", L"Restart driver", BS_PUSHBUTTON, 0, 0, 0, 0, ID_BTN_RESTART);
    add_ctl(L"BUTTON", L"Reinstall driver", BS_PUSHBUTTON, 0, 0, 0, 0, ID_BTN_REINSTALL);
    add_ctl(L"BUTTON", L"Remove and rescan", BS_PUSHBUTTON, 0, 0, 0, 0, ID_BTN_REMOVE);

    /* --- Buttons page --- */
    add_ctl(L"BUTTON", L"Enable button remapping (applies while this program is running)",
            BS_AUTOCHECKBOX, 0, 0, 0, 0, ID_BTN_REMAP_ON);
    add_ctl(L"STATIC", L"X1  -  back side button",    0, 0, 0, 0, 0, ID_LBL_X1);
    add_ctl(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, ID_CMB_X1);
    add_ctl(L"STATIC", L"X2  -  forward side button", 0, 0, 0, 0, 0, ID_LBL_X2);
    add_ctl(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, ID_CMB_X2);
    add_ctl(L"STATIC", L"DPI switch button",         0, 0, 0, 0, 0, ID_LBL_DPI);
    add_ctl(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, ID_CMB_DPI);
    add_ctl(L"BUTTON", L"Invert vertical scrolling", BS_AUTOCHECKBOX, 0, 0, 0, 0, ID_CHK_WHEEL);
    add_ctl(L"STATIC",
            L"How this works\r\n\r\n"
            L"The Preo MG14 is a standard HID mouse. It has no firmware interface that a "
            L"program can use, so there is nothing for a software package to talk to. "
            L"When remapping is on, this program watches mouse clicks with a normal "
            L"Windows hook and re-sends the ones you assigned a different action to.\r\n\r\n"
            L"That has three consequences worth knowing:\r\n"
            L"  -  Remapping is active only while this window is open. Close it and the "
            L"mouse goes back to its normal behaviour.\r\n"
            L"  -  The hook is not tied to this particular mouse, so a second mouse on "
            L"the same computer would be remapped the same way.\r\n"
            L"  -  The DPI switch on many of these mice is handled entirely on-board and "
            L"is never reported to Windows, so it often cannot be remapped here. The box "
            L"above is still offered in case yours does report it.",
            SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, 215);

    /* --- Pointer page --- */
    add_ctl(L"STATIC", L"Pointer speed", 0, 0, 0, 0, 0, ID_LBL_SPEED);
    add_ctl(L"msctls_trackbar32", NULL, TBS_AUTOTICKS | TBS_HORZ, 0, 0, 0, 0, ID_SLD_SPEED);
    add_ctl(L"STATIC", L"10", SS_CENTER, 0, 0, 0, 0, ID_TXT_SPEED);
    add_ctl(L"BUTTON", L"Enhance pointer precision (turn acceleration off)",
            BS_AUTOCHECKBOX, 0, 0, 0, 0, ID_CHK_ENHANCE);
    add_ctl(L"BUTTON", L"Swap the left and right mouse buttons", BS_AUTOCHECKBOX,
            0, 0, 0, 0, ID_CHK_SWAPXY);
    add_ctl(L"STATIC", L"Double-click speed", 0, 0, 0, 0, 0, ID_LBL_DCLICK);
    add_ctl(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, ID_CMB_DCLICK);
    add_ctl(L"BUTTON", L"Enhance click precision (hold Ctrl to lock the buttons)",
            BS_AUTOCHECKBOX, 0, 0, 0, 0, ID_CHK_ENHCLICK);
    add_ctl(L"BUTTON", L"Apply these settings to this mouse only, not other mice",
            BS_AUTOCHECKBOX, 0, 0, 0, 0, ID_CHK_PERDEVICE);
    add_ctl(L"BUTTON", L"Start this program when I sign in",
            BS_AUTOCHECKBOX, 0, 0, 0, 0, ID_CHK_AUTOSTART);
    add_ctl(L"BUTTON", L"Apply pointer settings", BS_DEFPUSHBUTTON, 0, 0, 0, 0, ID_BTN_APPLY_PTR);
    add_ctl(L"BUTTON", L"Reset to defaults", BS_PUSHBUTTON, 0, 0, 0, 0, ID_BTN_RESET_PTR);

    /* --- About page --- */
    add_ctl(L"STATIC",
            APP_NAME L"  " APP_VERSION L"\r\n"
            L"------------------------------------------------------------------\r\n\r\n"
            L"What this is\r\n"
            L"A settings manager for the Preo MG14 gaming mouse "
            L"(USB VID 30FA / PID 0400). It detects the mouse, shows which driver "
            L"Windows has bound to it, can repair that binding if it is broken, and "
            L"lets you set the pointer and button behaviour Windows exposes.\r\n\r\n"
            L"What this is not\r\n"
            L"It is not a kernel driver, and it does not install one. A replacement "
            L"mouse driver would have to be a signed .sys file, and since Windows 10 "
            L"1607 such a driver can only load if Microsoft has signed it through the "
            L"Hardware Dev Center. There is no way to ship one that is both useful "
            L"and safe to load, so this program does the job in user mode instead.\r\n\r\n"
            L"Safety\r\n"
            L"  -  No network access. The program does not import any networking "
            L"library, so it cannot phone home even if it wanted to.\r\n"
            L"  -  No kernel driver, no service, no scheduled task.\r\n"
            L"  -  Secure Boot, driver signature enforcement and test mode are never "
            L"touched.\r\n"
            L"  -  It never asks for Administrator rights just to open. The three "
            L"driver buttons on the Device page do need them, and each one shows a "
            L"normal UAC prompt naming the action.\r\n"
            L"  -  Settings live in one text file under %LOCALAPPDATA%\\" APP_SHORT
            L". Delete that file and the program is back to a clean state.\r\n"
            L"  -  Start-with-Windows is off by default and is a single HKCU value you "
            L"can delete at any time.\r\n\r\n"
            L"If Windows SmartScreen warns you about an unsigned program\r\n"
            L"This build is not code-signed, because code signing costs money. Smart "
            L"Screen warns about every unsigned program it has never seen. You can "
            L"check this one instead: run it, and look at the Device tab. It imports "
            L"no networking library, installs no driver file, and shows you every "
            L"device property it reads before it acts on anything.\r\n\r\n"
            L"Source code: distributed with this program under the MIT licence.",
            SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, ID_ABOUT_TEXT);

    /* --- chrome --- */
    g_status = add_ctl(L"msctls_statusbar32", NULL, SBARS_SIZEGRIP, 0, 0, 0, 0, ID_STATUSBAR);
    add_ctl(L"BUTTON", L"Rescan", BS_PUSHBUTTON, 0, 0, 0, 0, ID_BTN_RESCAN);
    add_ctl(L"BUTTON", L"Close", BS_PUSHBUTTON, 0, 0, 0, 0, ID_BTN_CLOSE);

    (void)inst;
}

/* ------------------------------------------------------------------ */

static void do_rescan(void)
{
    refresh_device();
    g_remappable = (g_dev.found && g_dev.numberButtons > 3)
                     ? (int)g_dev.numberButtons : 5;
    if (g_remappable > 6) g_remappable = 6;
}

static void request_elevated(const wchar_t *what, int action)
{
    wchar_t args[32];
    if (MessageBoxW(g_main,
        L"Windows needs Administrator rights for this action.\r\n\r\n"
        L"You will be asked to confirm. Windows will then restart the mouse driver, "
        L"which may interrupt pointer input for a second.",
        what, MB_ICONINFORMATION | MB_OKCANCEL) != IDOK)
        return;
    _snwprintf(args, 32, L"--action=%d", action);
    args[31] = L'\0';
    if (relaunch_elevated(args)) {
        PostMessageW(g_main, WM_CLOSE, 0, 0);
    } else {
        MessageBoxW(g_main, L"Could not request Administrator rights.", APP_NAME, MB_ICONERROR);
    }
}

static void do_driver_action(int action)
{
    BOOL reboot = FALSE;
    DrvOpResult r;

    if (!g_dev.found) {
        MessageBoxW(g_main, L"The Preo MG14 is not connected.", APP_NAME, MB_ICONWARNING);
        return;
    }
    switch (action) {
    case 0:
        r = driver_restart(&g_dev, &reboot);
        break;
    case 1:
        r = driver_install(&g_dev, &reboot);
        break;
    default:
        if (MessageBoxW(g_main,
                L"This removes the mouse from the device tree.\r\n\r\n"
                L"Windows will forget it and install the standard HID driver again the "
                L"next time the mouse is plugged in. Unplug and replug the mouse after "
                L"this finishes.\r\n\r\n"
                L"Continue?",
                L"Remove and rescan", MB_ICONWARNING | MB_YESNO) != IDYES)
            return;
        r = driver_remove(&g_dev, &reboot);
        break;
    }
    set_status(L"%s", drvop_result_message(r));
    MessageBoxW(g_main, drvop_result_message(r), APP_NAME,
                (r == DRVOP_OK) ? MB_ICONINFORMATION : MB_ICONWARNING);
    do_rescan();
}

static void collect_settings(void)
{
    HWND tb = GetDlgItem(g_main, ID_SLD_SPEED);
    HWND cb = GetDlgItem(g_main, ID_CMB_DCLICK);
    int  i;

    g_settings.pointerSpeed     = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
    g_settings.enhancePrecision = get_check(ID_CHK_ENHANCE);
    g_settings.swapXY           = get_check(ID_CHK_SWAPXY);
    g_settings.enhanceClickPrecision = get_check(ID_CHK_ENHCLICK);
    g_settings.startWithWindows = get_check(ID_CHK_AUTOSTART);
    g_settings.remapEnabled     = get_check(ID_BTN_REMAP_ON);
    g_settings.wheelInvert      = get_check(ID_CHK_WHEEL);
    g_settings.x1Action = combo_action(GetDlgItem(g_main, ID_CMB_X1));
    g_settings.x2Action = combo_action(GetDlgItem(g_main, ID_CMB_X2));
    g_settings.dpiAction = combo_action(GetDlgItem(g_main, ID_CMB_DPI));

    i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (i != CB_ERR) {
        static const int dclicks[] = { 250, 300, 350, 400, 450, 500, 550, 600, 700, 800, 900 };
        if (i >= 0 && i < (int)(sizeof(dclicks) / sizeof(dclicks[0])))
            g_settings.doubleClickSpeed = dclicks[i];
    }
    if (g_settings.pointerSpeed < 1)  g_settings.pointerSpeed = 1;
    if (g_settings.pointerSpeed > 20) g_settings.pointerSpeed = 20;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        /* CreateWindowExW has not returned yet, so the caller has not stored the
         * handle. Child controls need a real parent, so bind it here. */
        g_main = h;
        create_controls();
        do_rescan();
        refresh_buttons_page();
        read_pointer_settings(&g_settings);
        refresh_pointer_page();
        on_resize();
        SetTimer(h, 1, RESCAN_INTERVAL_MS, NULL);
        remap_set_notify(h);
        if (g_settings.remapEnabled) remap_start(&g_settings);
        return 0;

    case WM_SIZE:
        on_resize();
        return 0;

    case WM_TIMER:
        if (wp == 1) do_rescan();
        return 0;

    case WM_APP_CAPTURED:
        set_status(L"Last button pressed: %s", mouse_button_label(remap_last_button()));
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_RESCAN:
            do_rescan();
            set_status(L"Rescan finished.");
            return 0;
        case ID_BTN_CLOSE:
            PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        case ID_BTN_RESTART:
            if (is_elevated()) do_driver_action(0);
            else request_elevated(L"Restart driver", 0);
            return 0;
        case ID_BTN_REINSTALL:
            if (is_elevated()) do_driver_action(1);
            else request_elevated(L"Reinstall driver", 1);
            return 0;
        case ID_BTN_REMOVE:
            if (is_elevated()) do_driver_action(2);
            else request_elevated(L"Remove and rescan", 2);
            return 0;

        case ID_BTN_REMAP_ON:
            collect_settings();
            apply_remap();
            settings_save(&g_settings);
            set_status(g_settings.remapEnabled
                       ? L"Button remapping is on while this program stays open."
                       : L"Button remapping is off.");
            return 0;
        case ID_CHK_WHEEL:
            collect_settings();
            apply_remap();
            settings_save(&g_settings);
            return 0;

        case ID_SLD_SPEED: {
            wchar_t buf[32];
            int v = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
            _snwprintf(buf, 32, L"%d", v);
            set_text(ID_TXT_SPEED, buf);
            return 0;
        }
        case ID_TAB:
            if ((int)HIWORD(wp) == PREO_TCN_SELCHANGE) {
                collect_settings();
                settings_save(&g_settings);
                g_page = (int)TabCtrl_GetCurSel(g_tab);
                /* The background rescan may have learned something new about
                 * the device while this tab was hidden. */
                if (g_page == 1) refresh_buttons_page();
                layout_page(g_page);
                return 0;
            }
            return 0;

        case ID_CHK_AUTOSTART:
            collect_settings();
            if (!apply_autostart(g_settings.startWithWindows))
                MessageBoxW(g_main, L"Could not update the start-with-Windows setting.",
                            APP_NAME, MB_ICONERROR);
            settings_save(&g_settings);
            return 0;

        case ID_BTN_APPLY_PTR:
            collect_settings();
            apply_pointer_settings(&g_settings, get_check(ID_CHK_PERDEVICE), h);
            set_status(L"Pointer settings applied.");
            return 0;
        case ID_BTN_RESET_PTR:
            settings_defaults(&g_settings);
            apply_pointer_settings(&g_settings, FALSE, h);
            apply_autostart(FALSE);
            remap_stop();
            settings_save(&g_settings);
            refresh_pointer_page();
            refresh_buttons_page();
            set_status(L"Settings reset to defaults.");
            return 0;
        }
        break;

    case WM_CLOSE:
        collect_settings();
        settings_save(&g_settings);
        apply_autostart(g_settings.startWithWindows);
        remap_stop();
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        KillTimer(h, 1);
        remap_set_notify(NULL);
        if (g_font && g_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(g_font);
        if (g_fontBold && g_fontBold != g_font) DeleteObject(g_fontBold);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    HANDLE   mutex;
    MSG      msg;
    int      elevatedAction = -1;

    (void)prev;

    /* Console report: no window, no elevation, no side effects. */
    if (diagnostics_requested(cmdline)) return run_diagnostics(cmdline);

    /* One instance only: two copies would fight over the low-level hook.
     * The elevated half is a deliberate second copy, so let that one through. */
    mutex = CreateMutexW(NULL, TRUE, L"Local\\PreoMG14DriverSingleton");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!cmdline || wcsstr(cmdline, L"--elevated") == NULL) {
            MessageBoxW(NULL, APP_NAME L" is already running.", APP_NAME, MB_ICONINFORMATION);
            return 0;
        }
    }

    /* Parse the command line before anything is shown: the elevated half runs
     * exactly one action and exits, and must not flash a window at the user. */
    if (cmdline) {
        const wchar_t *p = wcsstr(cmdline, L"--action=");
        if (p) {
            elevatedAction = _wtoi(p + 9);
            if (elevatedAction < 0 || elevatedAction > 2) elevatedAction = -1;
        }
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES |
                                         ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    settings_load(&g_settings);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = LoadCursorW(NULL, MAKEINTRESOURCEW(32512))  /* IDC_ARROW */;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"PreoMG14DriverWindow";
    if (!RegisterClassExW(&wc)) return 1;

    g_main = CreateWindowExW(0, wc.lpszClassName, APP_NAME L" " APP_VERSION,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 780, 600,
                             NULL, NULL, inst, NULL);
    if (!g_main) return 1;

    if (elevatedAction >= 0) {
        do_rescan();
        if (g_dev.found) do_driver_action(elevatedAction);
        else MessageBoxW(g_main, L"The Preo MG14 is not connected.", APP_NAME, MB_ICONWARNING);
        DestroyWindow(g_main);
        if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
        return 0;
    }

    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_APP_CAPTURED) {
            set_status(L"Last button pressed: %s", mouse_button_label(remap_last_button()));
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
