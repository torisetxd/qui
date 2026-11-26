#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Message for the last failed call on this thread. */
const char *qsys_last_error(void);

typedef struct qsys_app {
  /* The one identity used for the Wayland app id, the .desktop basename, the
   * portal registration, the systemd unit and the state directory. */
  const char *id;
  const char *name;
  const char *comment;
  const char *icon;
  const char *categories;
  /* Absolute path, or NULL for the running executable. */
  const char *exec;
  /* When set, the executable is copied to ~/.local/bin/<install_as>. */
  const char *install_as;
  const char *args;
  int hidden;
  int service;
  const char *service_args;
  int service_always_restart;
  int service_needs_pipewire;
  /* Starts with the user manager instead of the graphical session. */
  int service_headless;
} qsys_app;

enum {
  QSYS_SERVICE_START,
  QSYS_SERVICE_STOP,
  QSYS_SERVICE_RESTART,
  QSYS_SERVICE_ENABLE,
  QSYS_SERVICE_DISABLE,
};

int qsys_install(const qsys_app *app);
int qsys_uninstall(const qsys_app *app);
int qsys_service(const char *id, int action);
int qsys_service_active(const char *id);

/* Associates this process with an installed app id on the desktop portal. The
 * capture calls do this for you, and it has to happen before any other portal
 * call on the connection. */
int qsys_portal_register(const char *id);

typedef struct qsys_screen qsys_screen;

typedef struct qsys_screen_options {
  const char *app_id;
  /* Names the saved permission, so one app can hold several. NULL for "screen". */
  const char *slot;
  int windows;
  int multiple;
  int show_cursor;
  /* Forget the permission when the session ends instead of saving it. */
  int no_persist;
  int fps;
  /* How long to wait for the permission dialog. 0 for five minutes. */
  int timeout_ms;
} qsys_screen_options;

typedef struct qsys_stream_info {
  uint32_t node_id;
  int x, y, width, height;
} qsys_stream_info;

/* BGRx pixels. Valid until the next qsys_screen_next on the same stream. */
typedef struct qsys_frame {
  const uint8_t *data;
  int width, height, stride;
  uint64_t sequence;
  double time;
} qsys_frame;

qsys_screen *qsys_screen_open(const qsys_screen_options *options);
void qsys_screen_close(qsys_screen *screen);
int qsys_screen_count(const qsys_screen *screen);
void qsys_screen_info(const qsys_screen *screen, int stream, qsys_stream_info *out);
/* True when a saved permission was presented, so no dialog was expected. */
int qsys_screen_had_permission(const qsys_screen *screen);
/* Waits for the newest frame. Returns 1 for a frame, 0 on timeout, -1 once the
 * session has ended. Reconnects by itself if the stream silently stalls. */
int qsys_screen_next(qsys_screen *screen, int stream, qsys_frame *out, int timeout_ms);

/* Global shortcuts through org.freedesktop.portal.GlobalShortcuts. The portal
 * shows its dialog once per app id and the compositor owns the keys after
 * that; install the app first so the portal knows the id. */
typedef struct qsys_shortcuts qsys_shortcuts;

typedef struct qsys_shortcut {
  /* Reported back through the callback when its key is pressed. */
  const char *id;
  /* Shown in the portal dialog and in the desktop's shortcut settings. */
  const char *description;
  /* A trigger to suggest when the portal can, like "ALT+R". NULL for none. */
  const char *preferred_trigger;
} qsys_shortcut;

/* Runs on the portal thread for every activated shortcut. Wake the UI from it.
 * The id is valid for the duration of the call. */
typedef void (*qsys_shortcut_fn)(const char *id, void *user);

/* Binds the shortcuts and returns before the user has answered the dialog, so
 * the keys come alive a moment later. NULL means the portal is unavailable or
 * refused. */
qsys_shortcuts *qsys_shortcuts_open(const char *app_id, const qsys_shortcut *shortcuts, int count, qsys_shortcut_fn fn, void *user);
void qsys_shortcuts_close(qsys_shortcuts *shortcuts);

typedef struct qsys_audio qsys_audio;

typedef struct qsys_audio_options {
  const char *name;
  /* Captures what the default output is playing instead of the microphone. */
  int system;
  /* A PipeWire node name or serial to capture instead of the default. */
  const char *target;
  /* 0 for 48000 and 2. */
  int rate;
  int channels;
} qsys_audio_options;

typedef struct qsys_audio_source {
  /* The PipeWire node name, to pass back as qsys_audio_options.target. */
  char name[256];
  char description[256];
  /* Set for a sink's monitor, which captures what is playing. */
  int is_monitor;
} qsys_audio_source;

/* Lists the capture sources. Returns how many were written, 0 when there are
 * none, -1 on failure. */
int qsys_audio_sources(qsys_audio_source *out, int max);

qsys_audio *qsys_audio_open(const qsys_audio_options *options);
void qsys_audio_close(qsys_audio *audio);

/* Reads interleaved float samples. Returns frames read, 0 on timeout, -1 once
 * the stream failed. `time` receives the monotonic time of the first frame. */
int qsys_audio_read(qsys_audio *audio, float *out, int max_frames, int timeout_ms, double *time);

#ifdef __cplusplus
}
#endif
