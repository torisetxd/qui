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

/* Call before qui_open. The window stays above other windows, where the
 * compositor allows it; Wayland has no protocol for this, so there it needs a
 * window rule. */
void qui_set_always_on_top(int on);

/* Call before qui_open. Leaves the window out of the task bar and the window
 * list, for an overlay or a palette. */
void qui_set_utility_window(int on);

/* Call before qui_open; on by default. Off keeps the window from taking the
 * keyboard focus, for an overlay that only shows something. */
void qui_set_focusable(int on);

/* Call before qui_open to start with the window hidden. */
void qui_set_start_hidden(int on);

/* Shows or hides the window while the loop keeps running. */
void qui_show_window(qui_app *app, int on);

enum {
  QUI_CLOSE_QUIT = 0,
  QUI_CLOSE_HIDE = 1,
  QUI_CLOSE_MINIMIZE = 2,
};

/* Controls what closing the window (close button or qui_window_action(2))
 * does: QUI_CLOSE_QUIT ends the loop (default), QUI_CLOSE_HIDE hides the
 * window while keeping the loop running, and QUI_CLOSE_MINIMIZE minimizes it
 * to the taskbar. */
void qui_set_close_action(qui_app *app, int action);

/* Requests the frame loop to exit, even when close action is not
 * QUI_CLOSE_QUIT. */
void qui_quit(qui_app *app);

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

/* Whether the window is currently visible (not hidden). */
int qui_window_visible(qui_app *app);

/* Whether the window is currently minimized. */
int qui_window_minimized(qui_app *app);

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

typedef struct qui_tray qui_tray;
typedef struct qui_tray_menu qui_tray_menu;
typedef struct qui_tray_entry qui_tray_entry;

/* Creates a system tray icon (no-op in headless/unsupported environments). */
qui_tray *qui_tray_create(qui_app *app, const char *tooltip);
void qui_tray_destroy(qui_tray *tray);
int qui_tray_valid(qui_tray *tray);
void qui_tray_set_icon(qui_tray *tray, int w, int h, const uint8_t *rgba);
void qui_tray_set_tooltip(qui_tray *tray, const char *tooltip);

qui_tray_menu *qui_tray_menu_root(qui_tray *tray);
qui_tray_entry *qui_tray_add_item(qui_tray_menu *menu, int pos, const char *label, uint32_t id);
qui_tray_entry *qui_tray_add_check(qui_tray_menu *menu, int pos, const char *label, int checked, uint32_t id);
qui_tray_entry *qui_tray_add_submenu(qui_tray_menu *menu, int pos, const char *label, qui_tray_menu **out_submenu);
qui_tray_entry *qui_tray_add_separator(qui_tray_menu *menu, int pos);
void qui_tray_remove_entry(qui_tray_entry *entry);
void qui_tray_entry_set_label(qui_tray_entry *entry, const char *label);
void qui_tray_entry_set_checked(qui_tray_entry *entry, int checked);
int qui_tray_entry_checked(qui_tray_entry *entry);
void qui_tray_entry_set_enabled(qui_tray_entry *entry, int enabled);

/* Pops the next clicked entry id, returning 1 if an event was available. */
int qui_tray_next_click(qui_tray *tray, uint32_t *out_id);

#ifdef __cplusplus
}
#endif
