#include "clicker.h"

#include <mmsystem.h>

#define STATUS_UPDATES_PER_SECOND 30

static clicker_status_callback g_callback;
static void *g_callback_data;

static HANDLE g_thread;
static HANDLE g_stop_event;          /* wakes the worker instantly on stop */
static volatile LONG g_stop_requested;
static volatile LONG g_active;
static clicker_config g_config;      /* written before the worker starts, read-only after */
static LONGLONG g_qpc_frequency;

static LONGLONG qpc_now(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static bool stop_requested(void)
{
    return InterlockedCompareExchange(&g_stop_requested, 0, 0) != 0;
}

static void report(const clicker_status *status)
{
    if (g_callback) g_callback(status, g_callback_data);
}

/*
 * Sleeps until an absolute QPC deadline. Sleeping on the stop event (rather
 * than Sleep) means a stop request interrupts the wait immediately. The last
 * millisecond is spent yielding, because a timed wait can overshoot by ~1 ms
 * even with timeBeginPeriod(1).
 * Returns false if a stop was requested.
 */
static bool wait_until(LONGLONG deadline)
{
    for (;;) {
        if (stop_requested()) return false;

        LONGLONG remaining = deadline - qpc_now();
        if (remaining <= 0) return true;

        LONGLONG remaining_ms = remaining * 1000 / g_qpc_frequency;
        if (remaining_ms >= 2) {
            LONGLONG wait_ms = remaining_ms - 1;
            if (wait_ms > 1000) wait_ms = 1000;
            if (WaitForSingleObject(g_stop_event, (DWORD)wait_ms) == WAIT_OBJECT_0) return false;
        } else {
            SwitchToThread();
        }
    }
}

/* True if the screen point is over one of this process's windows. The WebView2
   content lives in a child window owned by the browser process, so compare the
   top-level ancestor rather than the window directly under the point. */
static bool is_over_own_window(POINT point)
{
    HWND hwnd = WindowFromPoint(point);
    if (!hwnd) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    DWORD process_id = 0;
    GetWindowThreadProcessId(root ? root : hwnd, &process_id);
    return process_id == GetCurrentProcessId();
}

static POINT click_target(const clicker_config *config)
{
    POINT point = config->fixed_point;
    if (!config->use_fixed_point) GetCursorPos(&point);
    return point;
}

static void send_click(const clicker_config *config)
{
    DWORD down_flag, up_flag;
    switch (config->button) {
    case CLICK_BUTTON_RIGHT:  down_flag = MOUSEEVENTF_RIGHTDOWN;  up_flag = MOUSEEVENTF_RIGHTUP;  break;
    case CLICK_BUTTON_MIDDLE: down_flag = MOUSEEVENTF_MIDDLEDOWN; up_flag = MOUSEEVENTF_MIDDLEUP; break;
    default:                  down_flag = MOUSEEVENTF_LEFTDOWN;   up_flag = MOUSEEVENTF_LEFTUP;   break;
    }

    INPUT inputs[4];
    ZeroMemory(inputs, sizeof inputs);
    UINT input_count = config->double_click ? 4 : 2;
    for (UINT i = 0; i < input_count; i++) {
        inputs[i].type = INPUT_MOUSE;
        inputs[i].mi.dwFlags = (i % 2 == 0) ? down_flag : up_flag;
    }

    if (config->use_fixed_point) {
        SetCursorPos(config->fixed_point.x, config->fixed_point.y);
    }
    SendInput(input_count, inputs, sizeof(INPUT));
}

/* Returns true if the configured click count was reached. */
static bool run_clicks(const clicker_config *config, uint64_t *clicks)
{
    clicker_status status = { CLICKER_RUNNING, 0, false, CLICKER_END_NONE };

    LONGLONG interval_ticks = (LONGLONG)config->interval_ms * g_qpc_frequency / 1000;
    if (interval_ticks < 1) interval_ticks = 1;
    LONGLONG report_ticks = g_qpc_frequency / STATUS_UPDATES_PER_SECOND;

    LONGLONG next_click = qpc_now();
    LONGLONG last_report = next_click;
    bool completed = false;

    status.waiting = is_over_own_window(click_target(config));
    report(&status);

    timeBeginPeriod(1);
    for (;;) {
        bool waiting = is_over_own_window(click_target(config));
        if (waiting != status.waiting) {
            status.waiting = waiting;
            report(&status);
        }

        if (!waiting) {
            send_click(config);
            status.clicks++;

            if (config->max_clicks != 0 && status.clicks >= config->max_clicks) {
                completed = true;
                break;
            }
        }

        LONGLONG now = qpc_now();
        if (!waiting && now - last_report >= report_ticks) {
            report(&status);
            last_report = now;
        }

        /* Absolute deadlines keep the average interval exact. If we fall more
           than a second behind (system stall), resync instead of bursting. */
        next_click += interval_ticks;
        if (now - next_click > g_qpc_frequency) next_click = now;

        if (!wait_until(next_click)) break;
    }
    timeEndPeriod(1);

    *clicks = status.clicks;
    return completed;
}

static DWORD WINAPI worker_main(LPVOID unused)
{
    (void)unused;
    clicker_config config = g_config;
    uint64_t clicks = 0;
    bool completed = run_clicks(&config, &clicks);

    InterlockedExchange(&g_active, 0);

    clicker_status status = { CLICKER_IDLE, clicks, false,
                              completed ? CLICKER_END_COMPLETED : CLICKER_END_STOPPED };
    report(&status);
    return 0;
}

bool clicker_init(clicker_status_callback callback, void *user_data)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_qpc_frequency = frequency.QuadPart;

    g_callback = callback;
    g_callback_data = user_data;
    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    return g_stop_event != NULL;
}

bool clicker_start(const clicker_config *config)
{
    if (InterlockedCompareExchange(&g_active, 1, 0) != 0) return false;

    /* The previous worker has already cleared g_active, so at most it is
       returning from its final status report; this wait is effectively instant. */
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
    }

    g_config = *config;
    InterlockedExchange(&g_stop_requested, 0);
    ResetEvent(g_stop_event);

    g_thread = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (!g_thread) {
        InterlockedExchange(&g_active, 0);
        return false;
    }
    SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return true;
}

void clicker_stop(void)
{
    InterlockedExchange(&g_stop_requested, 1);
    if (g_stop_event) SetEvent(g_stop_event);
}

bool clicker_is_active(void)
{
    return InterlockedCompareExchange(&g_active, 0, 0) != 0;
}

void clicker_shutdown(void)
{
    clicker_stop();
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stop_event) {
        CloseHandle(g_stop_event);
        g_stop_event = NULL;
    }
    g_callback = NULL;
}
