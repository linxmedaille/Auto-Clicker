#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webview/webview.h"

#include "clicker.h"
#include "resource.h"
#include "ui_html.h" /* generated at build time from ui.html */

#define WINDOW_WIDTH 360
#define WINDOW_HEIGHT 480

#define HOTKEY_TOGGLE 1   /* start/stop, F6 by default, user-configurable */
#define HOTKEY_ESCAPE 2   /* Esc, registered only while clicking or picking */

#define WM_APP_PICK_FINISHED (WM_APP + 1)
#define WM_APP_TRAY          (WM_APP + 2)   /* tray icon callbacks */
#define WM_APP_SHOW_WINDOW   (WM_APP + 3)   /* sent by a second launch */

#define MAX_INTERVAL_MS 86400000.0   /* 24 hours */
#define MAX_CLICK_COUNT 1000000000.0

#define WEBVIEW2_DOWNLOAD_URL L"https://go.microsoft.com/fwlink/p/?LinkId=2124703"

static webview_t g_webview;
static HWND g_message_window;   /* hidden window: hotkeys, hook notifications, tray icon */
static HWND g_main_window;      /* the webview window */
typedef struct {
    UINT vk;
    UINT mods; /* MOD_CONTROL | MOD_ALT | MOD_SHIFT */
} hotkey_combo;

static hotkey_combo g_toggle_hotkey = { VK_F6, 0 };
static bool g_toggle_hotkey_registered;
static bool g_escape_hotkey_registered;

typedef enum {
    PICK_IDLE,
    PICK_WAITING_FOR_CLICK,
    PICK_WAITING_FOR_RELEASE,   /* swallowing the button-up of the picking click */
} pick_phase;

static HHOOK g_mouse_hook;
static pick_phase g_pick_phase;
static WPARAM g_pick_release_message;
static bool g_pick_cancelled;
static POINT g_pick_candidate;
static POINT g_picked_point;
static bool g_has_picked_point;

/* ------------------------------------------------------------------ C -> JS */

/* Runs on the UI thread via webview_dispatch. */
static void eval_dispatched_js(webview_t w, void *script)
{
    webview_eval(w, (const char *)script);
    free(script);
}

/* The only way C talks to the page: safe to call from any thread. */
static void post_js(const char *script)
{
    size_t length = strlen(script) + 1;
    char *copy = malloc(length);
    if (!copy) return;
    memcpy(copy, script, length);
    webview_dispatch(g_webview, eval_dispatched_js, copy);
}

/* ------------------------------------------------------------------ Hotkeys */

static bool pick_in_progress(void)
{
    return g_pick_phase != PICK_IDLE;
}

/* Esc is only claimed while something is running, so it isn't stolen from other apps. */
static void update_escape_hotkey(void)
{
    bool wanted = clicker_is_active() || pick_in_progress();
    if (wanted && !g_escape_hotkey_registered) {
        g_escape_hotkey_registered = RegisterHotKey(g_message_window, HOTKEY_ESCAPE, MOD_NOREPEAT, VK_ESCAPE) != 0;
    } else if (!wanted && g_escape_hotkey_registered) {
        UnregisterHotKey(g_message_window, HOTKEY_ESCAPE);
        g_escape_hotkey_registered = false;
    }
}

static void tray_set_tip(const wchar_t *tip);
static void hotkey_label(hotkey_combo combo, char *out, size_t out_size);

/* ------------------------------------------------------------------ Clicker status */

static void handle_status_on_ui_thread(webview_t w, void *arg)
{
    clicker_status *status = arg;
    const char *state = status->state == CLICKER_RUNNING ? "running" : "idle";
    const char *reason = status->end_reason == CLICKER_END_COMPLETED ? "completed"
                       : status->end_reason == CLICKER_END_STOPPED ? "stopped" : "";

    char script[256];
    snprintf(script, sizeof script,
             "window.app&&app.onStatus({state:'%s',waiting:%s,clicks:%llu,reason:'%s'})",
             state, status->waiting ? "true" : "false", (unsigned long long)status->clicks, reason);
    webview_eval(w, script);

    tray_set_tip(status->state != CLICKER_RUNNING ? L"Autoclicker: idle"
               : status->waiting ? L"Autoclicker: waiting" : L"Autoclicker: running");
    if (status->state == CLICKER_IDLE) update_escape_hotkey();
    free(status);
}

/* Called on the worker thread: copy the status and hand it to the UI thread. */
static void on_clicker_status(const clicker_status *status, void *user_data)
{
    (void)user_data;
    clicker_status *copy = malloc(sizeof *copy);
    if (!copy) return;
    *copy = *status;
    webview_dispatch(g_webview, handle_status_on_ui_thread, copy);
}

/* ------------------------------------------------------------------ Point picking */

static LRESULT CALLBACK pick_mouse_hook(int code, WPARAM message, LPARAM lparam)
{
    if (code == HC_ACTION) {
        const MSLLHOOKSTRUCT *info = (const MSLLHOOKSTRUCT *)lparam;
        bool injected = (info->flags & LLMHF_INJECTED) != 0;

        if (!injected && g_pick_phase == PICK_WAITING_FOR_CLICK) {
            if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
                /* Left click picks, right click cancels. Either way the click is
                   swallowed so it doesn't reach the window underneath. */
                g_pick_cancelled = message == WM_RBUTTONDOWN;
                g_pick_candidate = info->pt;
                g_pick_release_message = message == WM_LBUTTONDOWN ? WM_LBUTTONUP : WM_RBUTTONUP;
                g_pick_phase = PICK_WAITING_FOR_RELEASE;
                return 1;
            }
        } else if (!injected && g_pick_phase == PICK_WAITING_FOR_RELEASE && message == g_pick_release_message) {
            PostMessageW(g_message_window, WM_APP_PICK_FINISHED, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(NULL, code, message, lparam);
}

static void end_pick(bool picked)
{
    if (g_mouse_hook) {
        UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = NULL;
    }
    if (!pick_in_progress()) return;
    g_pick_phase = PICK_IDLE;
    update_escape_hotkey();

    if (picked) {
        g_picked_point = g_pick_candidate;
        g_has_picked_point = true;
        char script[128];
        snprintf(script, sizeof script, "window.app&&app.onPicked(%ld,%ld)",
                 (long)g_picked_point.x, (long)g_picked_point.y);
        post_js(script);
    } else {
        post_js("window.app&&app.onPickCancelled()");
    }
}

static bool begin_pick(void)
{
    if (pick_in_progress()) return true;
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, pick_mouse_hook, GetModuleHandleW(NULL), 0);
    if (!g_mouse_hook) return false;
    g_pick_phase = PICK_WAITING_FOR_CLICK;
    update_escape_hotkey();
    return true;
}

/* ------------------------------------------------------------------ Start/stop toggle */

/* Shared by the hotkey and the tray menu. */
static void toggle_clicking(void)
{
    /* Queued ahead of any status update this causes, so the page can apply
       keyboard/tray-initiated changes without animation. */
    post_js("window.app&&app.onHotkey()");
    if (pick_in_progress()) end_pick(false);
    else if (clicker_is_active()) clicker_stop();
    else post_js("window.app&&app.onHotkeyStart()"); /* the page owns the settings */
}

/* ------------------------------------------------------------------ Tray icon
   Closing the window hides it to the tray; the hotkeys keep working. Quit lives
   in the tray menu. */

#define TRAY_ICON_ID 1
enum { TRAY_MENU_SHOW = 1, TRAY_MENU_TOGGLE, TRAY_MENU_QUIT };

static bool g_tray_added;
static bool g_tray_hint_shown;
static bool g_quitting;
static HICON g_tray_icon;
static UINT g_taskbar_created_message;
static wchar_t g_tray_tip[64] = L"Autoclicker: idle";

static void tray_data(NOTIFYICONDATAW *data, UINT flags)
{
    ZeroMemory(data, sizeof *data);
    data->cbSize = sizeof *data;
    data->hWnd = g_message_window;
    data->uID = TRAY_ICON_ID;
    data->uFlags = flags;
}

static void tray_add(void)
{
    if (!g_tray_icon) {
        g_tray_icon = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    }
    NOTIFYICONDATAW data;
    tray_data(&data, NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    data.uCallbackMessage = WM_APP_TRAY;
    data.hIcon = g_tray_icon;
    wcsncpy(data.szTip, g_tray_tip, ARRAYSIZE(data.szTip) - 1);
    g_tray_added = Shell_NotifyIconW(NIM_ADD, &data) != 0;
    if (g_tray_added) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

static void tray_remove(void)
{
    if (g_tray_added) {
        NOTIFYICONDATAW data;
        tray_data(&data, 0);
        Shell_NotifyIconW(NIM_DELETE, &data);
        g_tray_added = false;
    }
    if (g_tray_icon) {
        DestroyIcon(g_tray_icon);
        g_tray_icon = NULL;
    }
}

static void tray_set_tip(const wchar_t *tip)
{
    if (wcscmp(tip, g_tray_tip) == 0) return;
    wcsncpy(g_tray_tip, tip, ARRAYSIZE(g_tray_tip) - 1);
    if (!g_tray_added) return;
    NOTIFYICONDATAW data;
    tray_data(&data, NIF_TIP | NIF_SHOWTIP);
    wcsncpy(data.szTip, g_tray_tip, ARRAYSIZE(data.szTip) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

static void hotkey_label_wide(wchar_t *out, int out_count)
{
    char label[128];
    hotkey_label(g_toggle_hotkey, label, sizeof label);
    if (!MultiByteToWideChar(CP_UTF8, 0, label, -1, out, out_count)) out[0] = L'\0';
}

static void show_main_window(void)
{
    if (!g_main_window) return;
    ShowWindow(g_main_window, IsIconic(g_main_window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_main_window);
}

static void hide_main_window(void)
{
    ShowWindow(g_main_window, SW_HIDE);
    if (g_tray_hint_shown || !g_tray_added) return;
    g_tray_hint_shown = true;   /* explain it once per session, not on every close */

    wchar_t key[96];
    hotkey_label_wide(key, ARRAYSIZE(key));
    NOTIFYICONDATAW data;
    tray_data(&data, NIF_INFO);
    data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcsncpy(data.szInfoTitle, L"Autoclicker is still running", ARRAYSIZE(data.szInfoTitle) - 1);
    swprintf(data.szInfo, ARRAYSIZE(data.szInfo),
             L"%ls starts and stops clicking. Right-click the tray icon to quit.", key);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

static void quit_app(void)
{
    g_quitting = true;
    if (g_main_window) PostMessageW(g_main_window, WM_CLOSE, 0, 0);
}

static void show_tray_menu(void)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    wchar_t toggle[128], key[96];
    hotkey_label_wide(key, ARRAYSIZE(key));
    swprintf(toggle, ARRAYSIZE(toggle), L"%ls\t%ls",
             clicker_is_active() ? L"Stop clicking" : L"Start clicking", key);

    AppendMenuW(menu, MF_STRING, TRAY_MENU_SHOW, L"Show Autoclicker");
    AppendMenuW(menu, MF_STRING, TRAY_MENU_TOGGLE, toggle);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, TRAY_MENU_QUIT, L"Quit");
    SetMenuDefaultItem(menu, TRAY_MENU_SHOW, FALSE);

    /* The owner must be in the foreground or the menu won't close when the user clicks away. */
    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(g_message_window);
    UINT command = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        cursor.x, cursor.y, 0, g_message_window, NULL);
    PostMessageW(g_message_window, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command == TRAY_MENU_SHOW) show_main_window();
    else if (command == TRAY_MENU_TOGGLE) toggle_clicking();
    else if (command == TRAY_MENU_QUIT) quit_app();
}

/* Intercepts the webview window's close so it hides to the tray instead. */
static LRESULT CALLBACK main_window_subclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                             UINT_PTR subclass_id, DWORD_PTR data)
{
    (void)subclass_id; (void)data;
    if (message == WM_CLOSE && !g_quitting && g_tray_added) {
        hide_main_window();
        return 0;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

/* ------------------------------------------------------------------ Message window */

static LRESULT CALLBACK message_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    /* Explorer restarted: the tray icon is gone and has to be added again. */
    if (g_taskbar_created_message && message == g_taskbar_created_message) {
        tray_add();
        return 0;
    }

    switch (message) {
    case WM_HOTKEY:
        if (wparam == HOTKEY_ESCAPE) {
            post_js("window.app&&app.onHotkey()");
            if (pick_in_progress()) end_pick(false);
            if (clicker_is_active()) clicker_stop();
        } else if (wparam == HOTKEY_TOGGLE) {
            toggle_clicking();
        }
        return 0;

    case WM_APP_PICK_FINISHED:
        end_pick(!g_pick_cancelled);
        return 0;

    case WM_APP_TRAY:
        switch (LOWORD(lparam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            show_main_window();
            break;
        case WM_CONTEXTMENU:
            show_tray_menu();
            break;
        }
        return 0;

    case WM_APP_SHOW_WINDOW:
        show_main_window();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static HWND create_message_window(HINSTANCE instance)
{
    WNDCLASSEXW window_class = { 0 };
    window_class.cbSize = sizeof window_class;
    window_class.lpfnWndProc = message_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"AutoclickerMessages";
    if (!RegisterClassExW(&window_class)) return NULL;
    /* A never-shown top-level window rather than a message-only one: it must
       receive the TaskbarCreated broadcast and be findable by a second launch. */
    return CreateWindowExW(WS_EX_TOOLWINDOW, window_class.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0,
                           NULL, NULL, instance, NULL);
}

/* ------------------------------------------------------------------ JS -> C */

/* The page passes flat arrays of numbers and short strings; that's all we accept. */
typedef struct {
    bool is_string;
    char text[24];
    double number;
} json_value;

static const char *skip_whitespace(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Returns the number of values parsed, or -1 if the input isn't a flat array we accept. */
static int parse_flat_json_array(const char *json, json_value *values, int max_values)
{
    const char *p = skip_whitespace(json);
    if (*p++ != '[') return -1;
    p = skip_whitespace(p);
    if (*p == ']') return 0;

    int count = 0;
    for (;;) {
        if (count == max_values) return -1;
        json_value *value = &values[count++];
        memset(value, 0, sizeof *value);

        p = skip_whitespace(p);
        if (*p == '"') {
            p++;
            size_t length = 0;
            while (*p && *p != '"') {
                if (*p == '\\' || (unsigned char)*p < 0x20 || length + 1 >= sizeof value->text) return -1;
                value->text[length++] = *p++;
            }
            if (*p != '"') return -1;
            p++;
            value->text[length] = '\0';
            value->is_string = true;
        } else {
            char *end;
            value->number = strtod(p, &end);
            if (end == p || !isfinite(value->number)) return -1;
            p = end;
        }

        p = skip_whitespace(p);
        if (*p == ',') { p++; continue; }
        if (*p == ']') return count;
        return -1;
    }
}

static bool is_string_value(const json_value *value, const char *expected)
{
    return value->is_string && strcmp(value->text, expected) == 0;
}

static void reply_ok(const char *id)
{
    webview_return(g_webview, id, 0, "{\"ok\":true}");
}

/* message must be a plain literal without quotes or backslashes. */
static void reply_error(const char *id, const char *message)
{
    char json[160];
    snprintf(json, sizeof json, "{\"ok\":false,\"error\":\"%s\"}", message);
    webview_return(g_webview, id, 0, json);
}

/* clickerStart(intervalValue, "ms"|"s", "left"|"right"|"middle", "single"|"double",
                "forever"|"count", count, "cursor"|"point") */
static void bind_start(const char *id, const char *request, void *arg)
{
    (void)arg;
    json_value args[8];
    if (parse_flat_json_array(request, args, 8) != 7) { reply_error(id, "Invalid request"); return; }

    clicker_config config = { 0 };

    if (args[0].is_string || args[0].number <= 0) { reply_error(id, "Enter an interval"); return; }
    double interval_ms;
    if (is_string_value(&args[1], "ms")) interval_ms = args[0].number;
    else if (is_string_value(&args[1], "s")) interval_ms = args[0].number * 1000.0;
    else { reply_error(id, "Unknown interval unit"); return; }
    interval_ms = floor(interval_ms + 0.5);
    if (interval_ms < 1) { reply_error(id, "Interval must be at least 1 ms"); return; }
    if (interval_ms > MAX_INTERVAL_MS) { reply_error(id, "Interval must be 24 hours or less"); return; }
    config.interval_ms = (uint32_t)interval_ms;

    if (is_string_value(&args[2], "left")) config.button = CLICK_BUTTON_LEFT;
    else if (is_string_value(&args[2], "right")) config.button = CLICK_BUTTON_RIGHT;
    else if (is_string_value(&args[2], "middle")) config.button = CLICK_BUTTON_MIDDLE;
    else { reply_error(id, "Unknown mouse button"); return; }

    if (is_string_value(&args[3], "single")) config.double_click = false;
    else if (is_string_value(&args[3], "double")) config.double_click = true;
    else { reply_error(id, "Unknown click type"); return; }

    if (is_string_value(&args[4], "forever")) {
        config.max_clicks = 0;
    } else if (is_string_value(&args[4], "count")) {
        double count = args[5].number;
        if (args[5].is_string || count < 1 || count > MAX_CLICK_COUNT || count != floor(count)) {
            reply_error(id, "Count must be a whole number of at least 1");
            return;
        }
        config.max_clicks = (uint32_t)count;
    } else {
        reply_error(id, "Unknown repeat mode");
        return;
    }

    if (is_string_value(&args[6], "cursor")) {
        config.use_fixed_point = false;
    } else if (is_string_value(&args[6], "point")) {
        if (!g_has_picked_point) { reply_error(id, "Pick a point first"); return; }
        config.use_fixed_point = true;
        config.fixed_point = g_picked_point;
    } else {
        reply_error(id, "Unknown position mode");
        return;
    }

    if (pick_in_progress()) { reply_error(id, "Finish picking a point first"); return; }
    if (!clicker_start(&config)) { reply_error(id, "Already running"); return; }

    update_escape_hotkey();
    reply_ok(id);
}

static void bind_stop(const char *id, const char *request, void *arg)
{
    (void)request; (void)arg;
    clicker_stop();
    reply_ok(id);
}

static void bind_pick_start(const char *id, const char *request, void *arg)
{
    (void)request; (void)arg;
    if (clicker_is_active()) { reply_error(id, "Stop clicking first"); return; }
    if (!begin_pick()) { reply_error(id, "Couldn't watch the mouse"); return; }
    reply_ok(id);
}

static void bind_pick_cancel(const char *id, const char *request, void *arg)
{
    (void)request; (void)arg;
    end_pick(false);
    reply_ok(id);
}

/* ------------------------------------------------------------------ Customisable start/stop hotkey */

#define HOTKEY_MODIFIER_MASK (MOD_CONTROL | MOD_ALT | MOD_SHIFT)

static bool g_capturing_hotkey;   /* toggle hotkey is unregistered while the page records a new one */

/* Keys that are safe as a global shortcut on their own. Anything else (letters,
   digits, Space, Enter, arrows...) needs a modifier so normal typing keeps working. */
static bool can_be_used_alone(UINT vk)
{
    if (vk >= VK_F1 && vk <= VK_F24) return true;
    switch (vk) {
    case VK_PAUSE: case VK_SCROLL: case VK_INSERT:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        return true;
    }
    return false;
}

/* Returns NULL if the combo is acceptable, otherwise a message for the user. */
static const char *hotkey_problem(UINT vk, UINT mods)
{
    if (mods & ~HOTKEY_MODIFIER_MASK) return "Unsupported modifier";
    if (vk == 0 || vk > 0xFE) return "Unsupported key";
    if (vk <= VK_XBUTTON2 && vk != VK_CANCEL) return "Mouse buttons can't be hotkeys";
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
        return "Press a key along with the modifier";
    case VK_ESCAPE:
        return "Esc is reserved for stopping";
    }
    if (mods == 0 && !can_be_used_alone(vk)) return "Add Ctrl, Alt or Shift";
    return NULL;
}

static bool register_toggle_hotkey(hotkey_combo combo)
{
    return RegisterHotKey(g_message_window, HOTKEY_TOGGLE, MOD_NOREPEAT | combo.mods, combo.vk) != 0;
}

static void unregister_toggle_hotkey(void)
{
    if (g_toggle_hotkey_registered) UnregisterHotKey(g_message_window, HOTKEY_TOGGLE);
    g_toggle_hotkey_registered = false;
}

/* "Ctrl+Shift+F8" style label, UTF-8, for the page. */
static void hotkey_label(hotkey_combo combo, char *out, size_t out_size)
{
    wchar_t key_name[64] = L"";
    if (combo.vk >= VK_F1 && combo.vk <= VK_F24) {
        swprintf(key_name, 64, L"F%u", combo.vk - VK_F1 + 1);
    } else {
        LONG scan = (LONG)MapVirtualKeyW(combo.vk, MAPVK_VK_TO_VSC) << 16;
        switch (combo.vk) { /* these share scan codes with the numpad unless marked extended */
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
            scan |= 1 << 24;
        }
        if (!GetKeyNameTextW(scan, key_name, 64)) swprintf(key_name, 64, L"Key %u", combo.vk);
    }

    wchar_t label[96];
    swprintf(label, 96, L"%ls%ls%ls%ls",
             (combo.mods & MOD_CONTROL) ? L"Ctrl+" : L"",
             (combo.mods & MOD_ALT) ? L"Alt+" : L"",
             (combo.mods & MOD_SHIFT) ? L"Shift+" : L"",
             key_name);
    if (!WideCharToMultiByte(CP_UTF8, 0, label, -1, out, (int)out_size, NULL, NULL)) {
        snprintf(out, out_size, "Key %u", combo.vk);
    }
}

/* Writes s as a JSON string literal (with quotes) into out. */
static void json_string(const char *s, char *out, size_t out_size)
{
    size_t n = 0;
    if (out_size < 3) { if (out_size) out[0] = '\0'; return; }
    out[n++] = '"';
    for (; *s && n + 7 < out_size; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, out_size - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n++] = '"';
    out[n] = '\0';
}

static void reply_hotkey_state(const char *id)
{
    char label[128], label_json[300], json[400];
    hotkey_label(g_toggle_hotkey, label, sizeof label);
    json_string(label, label_json, sizeof label_json);
    snprintf(json, sizeof json, "{\"ok\":true,\"hotkey\":%s,\"label\":%s}",
             g_toggle_hotkey_registered ? "true" : "false", label_json);
    webview_return(g_webview, id, 0, json);
}

/* Settings live in %APPDATA%\Autoclicker\settings.ini. */
static bool settings_path(wchar_t *path, size_t path_size)
{
    wchar_t app_data[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"APPDATA", app_data, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    swprintf(path, path_size, L"%ls\\Autoclicker", app_data);
    CreateDirectoryW(path, NULL);
    swprintf(path, path_size, L"%ls\\Autoclicker\\settings.ini", app_data);
    return true;
}

static hotkey_combo load_hotkey_setting(void)
{
    hotkey_combo combo = { VK_F6, 0 };
    wchar_t path[MAX_PATH + 32];
    if (!settings_path(path, sizeof path / sizeof path[0])) return combo;
    UINT vk = GetPrivateProfileIntW(L"hotkey", L"vk", VK_F6, path);
    UINT mods = GetPrivateProfileIntW(L"hotkey", L"modifiers", 0, path);
    if (!hotkey_problem(vk, mods)) { combo.vk = vk; combo.mods = mods; }
    return combo;
}

static void save_hotkey_setting(hotkey_combo combo)
{
    wchar_t path[MAX_PATH + 32], value[16];
    if (!settings_path(path, sizeof path / sizeof path[0])) return;
    swprintf(value, 16, L"%u", combo.vk);
    WritePrivateProfileStringW(L"hotkey", L"vk", value, path);
    swprintf(value, 16, L"%u", combo.mods);
    WritePrivateProfileStringW(L"hotkey", L"modifiers", value, path);
}

/* The page is about to record a new combo: release the current one so pressing
   it is recorded instead of toggling the clicker. */
static void bind_hotkey_capture_begin(const char *id, const char *request, void *arg)
{
    (void)request; (void)arg;
    if (clicker_is_active() || pick_in_progress()) { reply_error(id, "Stop first"); return; }
    g_capturing_hotkey = true;
    unregister_toggle_hotkey();
    reply_ok(id);
}

/* Recording was cancelled: put the current hotkey back. */
static void bind_hotkey_capture_end(const char *id, const char *request, void *arg)
{
    (void)request; (void)arg;
    g_capturing_hotkey = false;
    if (!g_toggle_hotkey_registered) g_toggle_hotkey_registered = register_toggle_hotkey(g_toggle_hotkey);
    reply_hotkey_state(id);
}

/* hotkeySet(virtualKey, ctrl, alt, shift) with 0/1 flags. */
static void bind_hotkey_set(const char *id, const char *request, void *arg)
{
    (void)arg;
    json_value args[6];
    if (parse_flat_json_array(request, args, 6) != 4) { reply_error(id, "Invalid request"); return; }
    for (int i = 0; i < 4; i++) {
        if (args[i].is_string || args[i].number != floor(args[i].number) || args[i].number < 0) {
            reply_error(id, "Invalid request");
            return;
        }
    }
    if (args[0].number > 0xFE || args[1].number > 1 || args[2].number > 1 || args[3].number > 1) {
        reply_error(id, "Unsupported key");
        return;
    }

    hotkey_combo combo = { (UINT)args[0].number, 0 };
    if (args[1].number) combo.mods |= MOD_CONTROL;
    if (args[2].number) combo.mods |= MOD_ALT;
    if (args[3].number) combo.mods |= MOD_SHIFT;

    const char *problem = hotkey_problem(combo.vk, combo.mods);
    if (problem) { reply_error(id, problem); return; }

    unregister_toggle_hotkey();
    if (!register_toggle_hotkey(combo)) {
        /* Keep listening for another combo if still recording, otherwise restore the old one. */
        if (!g_capturing_hotkey) g_toggle_hotkey_registered = register_toggle_hotkey(g_toggle_hotkey);
        reply_error(id, "Another app is using that shortcut");
        return;
    }

    g_toggle_hotkey = combo;
    g_toggle_hotkey_registered = true;
    g_capturing_hotkey = false;
    save_hotkey_setting(combo);
    reply_hotkey_state(id);
}

static void bind_ready(const char *id, const char *request, void *arg)
{
    (void)request; (void)arg;
    reply_hotkey_state(id);
}

/* ------------------------------------------------------------------ Window setup */

static void show_missing_runtime_dialog(void)
{
    int choice = MessageBoxW(NULL,
        L"Autoclicker needs the Microsoft Edge WebView2 Runtime, which couldn't be found or started.\n\n"
        L"Download it from:\n" WEBVIEW2_DOWNLOAD_URL L"\n\n"
        L"Open the download page now?",
        L"Autoclicker", MB_ICONWARNING | MB_YESNO);
    if (choice == IDYES) {
        ShellExecuteW(NULL, L"open", WEBVIEW2_DOWNLOAD_URL, NULL, NULL, SW_SHOWNORMAL);
    }
}

static void apply_window_icon(HWND window, HINSTANCE instance)
{
    HICON large_icon = LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                           GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON small_icon = LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                             GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (large_icon) SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)large_icon);
    if (small_icon) SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
}

static void center_window(HWND window)
{
    RECT window_rect;
    MONITORINFO monitor = { sizeof monitor };
    if (!GetWindowRect(window, &window_rect)) return;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor)) return;

    int width = window_rect.right - window_rect.left;
    int height = window_rect.bottom - window_rect.top;
    int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
    int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    SetWindowPos(window, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous_instance, LPSTR command_line, int show_command)
{
    (void)previous_instance; (void)command_line; (void)show_command;

    /* One instance only: a second launch brings the (possibly hidden) window back. */
    HANDLE instance_mutex = CreateMutexW(NULL, TRUE, L"Local\\Autoclicker-v3-single-instance");
    if (instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"AutoclickerMessages", NULL);
        if (existing) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(existing, WM_APP_SHOW_WINDOW, 0, 0);
        }
        CloseHandle(instance_mutex);
        return 0;
    }

    /* DPI awareness comes from the PerMonitorV2 manifest (app.rc), so text is
       rendered at native resolution instead of being bitmap-stretched. */
    g_webview = webview_create(0, NULL);
    if (!g_webview) {
        show_missing_runtime_dialog();
        return 1;
    }

    g_message_window = create_message_window(instance);
    if (!g_message_window || !clicker_init(on_clicker_status, NULL)) {
        MessageBoxW(NULL, L"Autoclicker couldn't start.", L"Autoclicker", MB_ICONERROR);
        webview_destroy(g_webview);
        return 1;
    }
    g_toggle_hotkey = load_hotkey_setting();
    g_toggle_hotkey_registered = register_toggle_hotkey(g_toggle_hotkey);

    /* Sizes are in device-independent pixels; webview scales them for the monitor's DPI. */
    webview_set_title(g_webview, "Autoclicker");
    webview_set_size(g_webview, WINDOW_WIDTH, WINDOW_HEIGHT, WEBVIEW_HINT_FIXED);

    g_main_window = (HWND)webview_get_window(g_webview);
    if (g_main_window) {
        apply_window_icon(g_main_window, instance);
        center_window(g_main_window);
        SetWindowSubclass(g_main_window, main_window_subclass, 1, 0);
    }
    g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    tray_add();

    webview_bind(g_webview, "clickerStart", bind_start, NULL);
    webview_bind(g_webview, "clickerStop", bind_stop, NULL);
    webview_bind(g_webview, "pickStart", bind_pick_start, NULL);
    webview_bind(g_webview, "pickCancel", bind_pick_cancel, NULL);
    webview_bind(g_webview, "appReady", bind_ready, NULL);
    webview_bind(g_webview, "hotkeyCaptureBegin", bind_hotkey_capture_begin, NULL);
    webview_bind(g_webview, "hotkeyCaptureEnd", bind_hotkey_capture_end, NULL);
    webview_bind(g_webview, "hotkeySet", bind_hotkey_set, NULL);
    webview_set_html(g_webview, (const char *)ui_html);

    webview_run(g_webview);

    end_pick(false);
    clicker_shutdown();
    if (g_toggle_hotkey_registered) UnregisterHotKey(g_message_window, HOTKEY_TOGGLE);
    if (g_escape_hotkey_registered) UnregisterHotKey(g_message_window, HOTKEY_ESCAPE);
    tray_remove();
    DestroyWindow(g_message_window);
    webview_destroy(g_webview);
    if (instance_mutex) {
        ReleaseMutex(instance_mutex);
        CloseHandle(instance_mutex);
    }
    return 0;
}
