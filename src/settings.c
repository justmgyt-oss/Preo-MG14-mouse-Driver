/*
 * settings.c - Settings persistence in a plain, human-editable INI file.
 *
 * We deliberately avoid the registry for our own preferences: a single INI
 * under %LOCALAPPDATA% is inspectable, portable and trivially removable by the
 * uninstaller. Registry writes only happen for the *Windows* pointer settings
 * the user explicitly asks us to change (see pointer.c / main.c).
 */
#include "app.h"
#include <stdio.h>

static const wchar_t *ini_path(void)
{
    static wchar_t path[MAX_PATH];
    if (path[0] == L'\0') {
        wchar_t dir[MAX_PATH];
        if (!get_settings_dir(dir, MAX_PATH)) return NULL;
        _snwprintf(path, MAX_PATH, L"%s\\settings.ini", dir);
        path[MAX_PATH - 1] = L'\0';
    }
    return path;
}

void settings_defaults(Settings *s)
{
    ZeroMemory(s, sizeof(*s));
    s->pointerSpeed        = 10;    /* 10 == "no acceleration" in Windows    */
    s->enhancePrecision    = TRUE;
    s->swapXY              = FALSE;
    s->doubleClickSpeed    = 500;
    s->enhanceClickPrecision = FALSE;
    s->remapEnabled        = FALSE; /* opt-in: the hook runs only while this  */
                                   /* app is running, so default to off.     */
    s->x1Action            = BTN_BACK;
    s->x2Action            = BTN_FORWARD;
    s->dpiAction           = BTN_DISABLED;
    s->wheelInvert         = FALSE;
    s->startWithWindows    = FALSE;
}

static int read_int(const wchar_t *section, const wchar_t *key, int def)
{
    wchar_t buf[64];
    DWORD  n = GetPrivateProfileStringW(section, key, L"", buf, 64, ini_path());
    if (n == 0) return def;
    return _wtoi(buf);
}

static int read_bool(const wchar_t *section, const wchar_t *key, int def)
{
    wchar_t buf[16];
    DWORD  n = GetPrivateProfileStringW(section, key, L"", buf, 16, ini_path());
    if (n == 0) return def;
    return (_wcsicmp(buf, L"true") == 0 || _wcsicmp(buf, L"1") == 0 ||
            _wcsicmp(buf, L"yes") == 0);
}

static void write_int(const wchar_t *section, const wchar_t *key, int v)
{
    wchar_t buf[32];
    _snwprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(section, key, buf, ini_path());
}

static void write_bool(const wchar_t *section, const wchar_t *key, int v)
{
    WritePrivateProfileStringW(section, key, v ? L"true" : L"false", ini_path());
}

BOOL settings_load(Settings *s)
{
    settings_defaults(s);
    if (!ini_path() || GetFileAttributesW(ini_path()) == INVALID_FILE_ATTRIBUTES)
        return FALSE;

    s->pointerSpeed = read_int(L"Pointer", L"Speed", s->pointerSpeed);
    s->enhancePrecision = read_bool(L"Pointer", L"EnhancePrecision", s->enhancePrecision);
    s->swapXY = read_bool(L"Pointer", L"SwapXY", s->swapXY);
    s->doubleClickSpeed = read_int(L"Pointer", L"DoubleClickSpeed", s->doubleClickSpeed);
    s->enhanceClickPrecision = read_bool(L"Pointer", L"EnhanceClickPrecision", s->enhanceClickPrecision);

    s->remapEnabled = read_bool(L"Remap", L"Enabled", s->remapEnabled);
    s->x1Action = (ButtonAction)read_int(L"Remap", L"X1Action", s->x1Action);
    s->x2Action = (ButtonAction)read_int(L"Remap", L"X2Action", s->x2Action);
    s->dpiAction = (ButtonAction)read_int(L"Remap", L"DPISwitchAction", s->dpiAction);
    s->wheelInvert = read_bool(L"Remap", L"WheelInvert", s->wheelInvert);

    s->startWithWindows = read_bool(L"App", L"StartWithWindows", s->startWithWindows);

    /* Clamp anything hand-edited into a sane range. */
    if (s->pointerSpeed < 1)  s->pointerSpeed = 1;
    if (s->pointerSpeed > 20) s->pointerSpeed = 20;
    if (s->doubleClickSpeed < 250)  s->doubleClickSpeed = 250;
    if (s->doubleClickSpeed > 900)  s->doubleClickSpeed = 900;
    if (s->x1Action < 0 || s->x1Action >= BTN_COUNT) s->x1Action = BTN_BACK;
    if (s->x2Action < 0 || s->x2Action >= BTN_COUNT) s->x2Action = BTN_FORWARD;
    if (s->dpiAction < 0 || s->dpiAction >= BTN_COUNT) s->dpiAction = BTN_DISABLED;
    return TRUE;
}

BOOL settings_save(const Settings *s)
{
    if (!ensure_settings_dir()) return FALSE;
    write_int (L"Pointer", L"Speed", s->pointerSpeed);
    write_bool(L"Pointer", L"EnhancePrecision", s->enhancePrecision);
    write_bool(L"Pointer", L"SwapXY", s->swapXY);
    write_int (L"Pointer", L"DoubleClickSpeed", s->doubleClickSpeed);
    write_bool(L"Pointer", L"EnhanceClickPrecision", s->enhanceClickPrecision);
    write_bool(L"Remap",   L"Enabled", s->remapEnabled);
    write_int (L"Remap",   L"X1Action", (int)s->x1Action);
    write_int (L"Remap",   L"X2Action", (int)s->x2Action);
    write_int (L"Remap",   L"DPISwitchAction", (int)s->dpiAction);
    write_bool(L"Remap",   L"WheelInvert", s->wheelInvert);
    write_bool(L"App",     L"StartWithWindows", s->startWithWindows);
    return TRUE;
}

BOOL settings_reset(void)
{
    const wchar_t *p = ini_path();
    if (!p) return FALSE;
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return TRUE;
    return DeleteFileW(p);
}

/* ------------------------------------------------------------------ */

const wchar_t *mouse_button_label(int b)
{
    switch (b) {
    case 0:  return L"Left button";
    case 1:  return L"Right button";
    case 2:  return L"Middle button (wheel click)";
    case 3:  return L"X1 - back side button";
    case 4:  return L"X2 - forward side button";
    case 5:  return L"DPI switch button";
    default: return L"Unknown";
    }
}

const wchar_t *button_action_name(ButtonAction a)
{
    switch (a) {
    case BTN_NONE:          return L"No action";
    case BTN_MIDDLE:        return L"Middle click";
    case BTN_X1:            return L"Back / X1";
    case BTN_X2:            return L"Forward / X2";
    case BTN_BACK:          return L"Browser Back";
    case BTN_FORWARD:       return L"Browser Forward";
    case BTN_SCROLL_UP:     return L"Scroll up";
    case BTN_SCROLL_DOWN:   return L"Scroll down";
    case BTN_COPY:          return L"Copy";
    case BTN_PASTE:         return L"Paste";
    case BTN_DOUBLE_CLICK:  return L"Double click";
    case BTN_MEDIA_PLAY_PAUSE: return L"Media play / pause";
    case BTN_MEDIA_NEXT:    return L"Media next track";
    case BTN_MEDIA_PREV:    return L"Media previous track";
    case BTN_BROWSER_BACK:  return L"Browser Back";
    case BTN_BROWSER_FORWARD: return L"Browser Forward";
    case BTN_MUTE:          return L"Mute";
    case BTN_VOL_UP:        return L"Volume up";
    case BTN_VOL_DOWN:      return L"Volume down";
    case BTN_DISABLED:      return L"Disabled (pass through)";
    default:                return L"Unknown";
    }
}

ButtonAction button_action_from_name(const wchar_t *name)
{
    int i;
    for (i = 0; i < BTN_COUNT; i++)
        if (_wcsicmp(button_action_name((ButtonAction)i), name) == 0)
            return (ButtonAction)i;
    return BTN_DISABLED;
}
