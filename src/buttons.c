/*
 * buttons.c - User-mode button remapping via a low-level mouse hook.
 *
 * Why a hook and not a driver: the MG14 is a plain HID mouse with no
 * user-programmable firmware interface (it reports a single
 * Generic Desktop / Mouse collection, USB\\VID_30FA&PID_0400&REV_0100). Which
 * Windows button a physical button produces is therefore decided above the
 * HID stack, and a user-mode WH_MOUSE_LL hook is the supported way to change
 * it without a kernel driver.
 *
 * Properties that matter for safety:
 *   - Nothing is injected until the user explicitly enables remapping.
 *   - Events we ourselves inject are tagged LLKHF_INJECTED and passed straight
 *     through, so remapping can never feed back into an infinite loop.
 *   - The hook lives on its own thread with its own message loop and is
 *     uninstalled the moment the program exits.
 *   - It is a session hook: it does not survive logoff, and it does not
 *     survive this program closing. That limitation is stated in the UI.
 */
#include "app.h"

static HHOOK  g_hook;
static HANDLE g_thread;
static HANDLE g_readyEvent;      /* signalled once the thread has a message loop */
static HWND   g_notifyWnd;
static Settings g_settings;
static volatile LONG g_lastButton = -1;   /* index of the last physical click */
static volatile LONG g_running = 0;

/* ------------------------------------------------------------------ */

/* There is no generic MOUSEEVENTF_KEYUP, so map each press flag to its
 * matching release flag. A wheel event is a single impulse and has none. */
static DWORD release_flags_for(DWORD downFlags)
{
    if (downFlags & MOUSEEVENTF_LEFTDOWN)   return MOUSEEVENTF_LEFTUP;
    if (downFlags & MOUSEEVENTF_RIGHTDOWN)  return MOUSEEVENTF_RIGHTUP;
    if (downFlags & MOUSEEVENTF_MIDDLEDOWN) return MOUSEEVENTF_MIDDLEUP;
    if (downFlags & MOUSEEVENTF_XDOWN)      return MOUSEEVENTF_XUP;
    return 0;
}

/* Every remapped action is a complete press+release, so one physical click
 * always produces exactly one logical action. */
static void send_button_action(DWORD downFlags, DWORD data)
{
    DWORD upFlags = release_flags_for(downFlags);
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = downFlags;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof(in));
    if (upFlags) {
        in.mi.dwFlags = upFlags;
        SendInput(1, &in, sizeof(in));
    }
}

static void send_click(BOOL left)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = left ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
    SendInput(1, &in, sizeof(in));
    in.mi.dwFlags = left ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
    SendInput(1, &in, sizeof(in));
}

static void send_key(int vkey, BOOL ctrl)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vkey;
    if (ctrl) {
        INPUT c = in;
        c.ki.wVk = VK_CONTROL;
        SendInput(1, &c, sizeof(c));
    }
    SendInput(1, &in, sizeof(in));
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
    if (ctrl) {
        INPUT c;
        ZeroMemory(&c, sizeof(c));
        c.type = INPUT_KEYBOARD;
        c.ki.wVk = VK_CONTROL;
        c.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &c, sizeof(c));
    }
}

static void perform_action(ButtonAction a)
{
    switch (a) {
    case BTN_NONE:
    case BTN_DISABLED:
        break;                                    /* pass the original through */
    case BTN_MIDDLE:
        send_button_action(MOUSEEVENTF_MIDDLEDOWN, 0);
        break;
    case BTN_X1:
    case BTN_BACK:
    case BTN_BROWSER_BACK:
        send_button_action(MOUSEEVENTF_XDOWN, XBUTTON1);
        break;
    case BTN_X2:
    case BTN_FORWARD:
    case BTN_BROWSER_FORWARD:
        send_button_action(MOUSEEVENTF_XDOWN, XBUTTON2);
        break;
    case BTN_SCROLL_UP:
        send_button_action(MOUSEEVENTF_WHEEL, (DWORD)WHEEL_DELTA);
        break;
    case BTN_SCROLL_DOWN:
        send_button_action(MOUSEEVENTF_WHEEL, (DWORD)(-(long)WHEEL_DELTA));
        break;
    case BTN_COPY:
        send_key('C', TRUE);
        break;
    case BTN_PASTE:
        send_key('V', TRUE);
        break;
    case BTN_DOUBLE_CLICK:
        send_click(TRUE);
        send_click(TRUE);
        break;
    case BTN_MEDIA_PLAY_PAUSE: send_key(VK_MEDIA_PLAY_PAUSE, FALSE); break;
    case BTN_MEDIA_NEXT:       send_key(VK_MEDIA_NEXT_TRACK,  FALSE); break;
    case BTN_MEDIA_PREV:       send_key(VK_MEDIA_PREV_TRACK,  FALSE); break;
    case BTN_MUTE:             send_key(VK_VOLUME_MUTE,       FALSE); break;
    case BTN_VOL_UP:           send_key(VK_VOLUME_UP,         FALSE); break;
    case BTN_VOL_DOWN:         send_key(VK_VOLUME_DOWN,       FALSE); break;
    default:
        break;
    }
}

static ButtonAction action_for_x_button(int xbutton)
{
    return (xbutton == XBUTTON1) ? g_settings.x1Action : g_settings.x2Action;
}

/* ------------------------------------------------------------------ */

static LRESULT CALLBACK low_level_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT *hs = (MSLLHOOKSTRUCT *)lParam;
        DWORD vk = (DWORD)wParam;   /* WM_LBUTTONDOWN, WM_XBUTTONDOWN, ... */

        /* Never react to what we injected ourselves. */
        if (hs->flags & LLKHF_INJECTED) return CallNextHookEx(g_hook, nCode, wParam, lParam);

        switch (vk) {
        case WM_XBUTTONDOWN: {
            int xbutton = HIWORD(hs->mouseData) ? XBUTTON2 : XBUTTON1;
            int idx = (xbutton == XBUTTON1) ? 3 : 4;
            InterlockedExchange(&g_lastButton, idx);
            if (g_notifyWnd) PostMessageW(g_notifyWnd, WM_APP_CAPTURED, 0, 0);
            {
                ButtonAction a = action_for_x_button(xbutton);
                if (a != BTN_NONE && a != BTN_DISABLED) {
                    perform_action(a);
                    return 1;                   /* consumed; suppress original */
                }
            }
            break;
        }
        case WM_MBUTTONDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            InterlockedExchange(&g_lastButton,
                                (vk == WM_MBUTTONDOWN) ? 2 :
                                (vk == WM_LBUTTONDOWN)  ? 0 : 1);
            if (g_notifyWnd) PostMessageW(g_notifyWnd, WM_APP_CAPTURED, 0, 0);
            break;
        case WM_MOUSEWHEEL:
            if (g_settings.wheelInvert) {
                if ((short)HIWORD(hs->mouseData) > 0)
                    perform_action(BTN_SCROLL_UP);
                else
                    perform_action(BTN_SCROLL_DOWN);
                return 1;
            }
            break;
        default:
            break;
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

static DWORD WINAPI hook_thread(LPVOID param)
{
    MSG msg;
    (void)param;

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, low_level_mouse_proc,
                               GetModuleHandleW(NULL), 0);

    /* Force the message queue into existence before signalling readiness,
     * otherwise remap_stop's PostThreadMessage(WM_QUIT) can race and be lost. */
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(g_readyEvent);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    return 0;
}

BOOL remap_start(const Settings *s)
{
    if (g_running) return TRUE;
    if (!s) return FALSE;
    g_settings = *s;
    InterlockedExchange(&g_lastButton, -1);
    g_running = 1;

    g_readyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_readyEvent) { g_running = 0; return FALSE; }

    g_thread = CreateThread(NULL, 0, hook_thread, NULL, 0, NULL);
    if (!g_thread) {
        CloseHandle(g_readyEvent);
        g_readyEvent = NULL;
        g_running = 0;
        return FALSE;
    }
    WaitForSingleObject(g_readyEvent, 2000);
    CloseHandle(g_readyEvent);
    g_readyEvent = NULL;

    if (!g_hook) {                         /* hook could not be installed */
        g_running = 0;
        CloseHandle(g_thread);
        g_thread = NULL;
        return FALSE;
    }
    return TRUE;
}

void remap_stop(void)
{
    if (!g_running) return;
    g_running = 0;
    if (g_thread) {
        PostThreadMessageW(GetThreadId(g_thread), WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    g_hook = NULL;
}

BOOL remap_is_running(void) { return g_running && g_hook != NULL; }

/* Index of the most recent physical button press, or -1. Lets the UI show the
 * user which of their buttons maps to which Windows button without them
 * having to guess. */
int remap_last_button(void) { return (int)g_lastButton; }
void remap_reset_last(void) { InterlockedExchange(&g_lastButton, -1); }

void remap_set_notify(HWND w) { g_notifyWnd = w; }
