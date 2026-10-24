#ifndef CLICKER_H
#define CLICKER_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

typedef enum {
    CLICK_BUTTON_LEFT,
    CLICK_BUTTON_RIGHT,
    CLICK_BUTTON_MIDDLE
} click_button;

typedef struct {
    uint32_t interval_ms;  /* >= 1 */
    click_button button;
    bool double_click;
    uint32_t max_clicks;   /* 0 = until stopped */
    bool use_fixed_point;
    POINT fixed_point;     /* physical screen pixels */
} clicker_config;

typedef enum {
    CLICKER_IDLE,
    CLICKER_RUNNING
} clicker_state;

/* Why the clicker returned to idle. */
typedef enum {
    CLICKER_END_NONE,
    CLICKER_END_STOPPED,
    CLICKER_END_COMPLETED
} clicker_end_reason;

typedef struct {
    clicker_state state;
    uint64_t clicks;
    /* Running, but holding off because the click target is over this app's own
       window (otherwise the first click would hit the Stop button). */
    bool waiting;
    clicker_end_reason end_reason; /* valid in CLICKER_IDLE */
} clicker_status;

/*
 * Called on the worker thread. Counter updates while running are throttled
 * to ~30 per second; state and waiting changes are always reported.
 */
typedef void (*clicker_status_callback)(const clicker_status *status, void *user_data);

bool clicker_init(clicker_status_callback callback, void *user_data);

/* Starts clicking immediately. Returns false if already active. */
bool clicker_start(const clicker_config *config);

/* Requests a stop. Never blocks; the worker reports CLICKER_IDLE when done. */
void clicker_stop(void);

bool clicker_is_active(void);

/* Stops and waits for the worker thread, then frees resources. */
void clicker_shutdown(void);

#endif
