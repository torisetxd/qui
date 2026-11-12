#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qui_app qui_app;

/* Sets the identity used for the Wayland app id. Call before qui_open. It must
 * match the basename of the app's .desktop file. */
void qui_set_app_id(const char *id);

/* Opens the window without the system title bar and border. Call before
 * qui_open. Boxes with the Drag style move it; the edges still resize it. */
void qui_set_borderless(int on);
void qui_set_transparent(int on);

qui_app *qui_open(const char *title, int width, int height);
void qui_close(qui_app *app);

/* Requests a frame. Safe to call from any thread while the app is open. */
void qui_wake(qui_app *app);

/* Requests a frame after `ms` milliseconds. */
void qui_wake_after(qui_app *app, uint32_t ms);

/* Tells the backend the app acted on this frame's Escape key. On Android an
 * unclaimed Back press sends the app to the background. */
void qui_consume_back(qui_app *app);

/* Opens the system file picker without blocking. Chosen paths are queued and
 * the app is woken; take them one at a time with qui_picked_file. */
void qui_pick_files(qui_app *app, int multiple);

/* Pops the next chosen path, or NULL when none. Valid until the next call. */
const char *qui_picked_file(qui_app *app);

/* Whether files are being dragged over the window. Dropped files arrive
 * through qui_picked_file. */
int qui_files_hovering(qui_app *app);

/* Puts UTF-8 text on the system clipboard. */
void qui_set_clipboard(qui_app *app, const char *text);

/* 0 minimizes the window, 1 maximizes or restores it, 2 closes it as its
 * close button would. */
void qui_window_action(qui_app *app, int action);

/* Whether the window is maximized. */
int qui_window_maximized(qui_app *app);

/* Eases mouse wheel steps toward their target instead of jumping. Off by
 * default. */
void qui_set_smooth_scroll(qui_app *app, int on);

/* The text selected in a `Select` subtree as of the last frame, or "". Valid
 * until the next qui_frame_end. */
const char *qui_selected_text(qui_app *app);

/* Blocks until a frame is needed. Returns the results table, or NULL on quit.
 * The pointer stays valid until the next qui_frame_begin. */
const uint8_t *qui_frame_begin(qui_app *app, size_t *len);

/* Consumes one wire-format frame. The engine does not retain `wire`. */
void qui_frame_end(qui_app *app, const uint8_t *wire, size_t len);

/* Lends the engine `len` bytes of straight RGBA for an ImageShared op naming
 * `token`, without copying them. `release(ctx)` runs once the engine and its
 * worker thread are done with the bytes, from whichever thread lets go last. */
void qui_stage_image(qui_app *app, uint64_t token, uint32_t w, uint32_t h, const uint8_t *rgba, size_t len, void *ctx, void (*release)(void *));

#ifdef __cplusplus
}
#endif
