//go:build linux && !android && cgo

#include "internal.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace qui::sys {

constexpr const char *kPortalBus = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char *kShortcuts = "org.freedesktop.portal.GlobalShortcuts";
constexpr const char *kRequest = "org.freedesktop.portal.Request";
constexpr const char *kPortalRegistry = "org.freedesktop.host.portal.Registry";
constexpr const char *kProperties = "org.freedesktop.DBus.Properties";

constexpr auto kStartTimeout = std::chrono::seconds(10);

std::string random_token(const char *prefix) {
  static std::mt19937 generator{std::random_device{}()};
  return std::string(prefix) + std::to_string(generator());
}

}  // namespace qui::sys

using namespace qui::sys;

struct qsys_shortcuts {
  std::string app_id;
  std::vector<std::string> ids, descriptions, triggers;
  qsys_shortcut_fn fn = nullptr;
  void *user = nullptr;

  GMainContext *context = nullptr;
  GDBusConnection *connection = nullptr;
  GMainLoop *loop = nullptr;
  std::string sender;
  std::string session;
  std::string session_request, bind_request;
  guint activated = 0;
  int token = 0;
  bool bound = false;

  std::thread thread;
  std::mutex mutex;
  std::condition_variable cv;
  bool started = false;
  std::string error;

  std::string next_token(const char *prefix) { return std::string(prefix) + std::to_string(++token); }

  std::string request_path(const std::string &handle) const {
    return std::string(kPortalPath) + "/request/" + sender + "/" + handle;
  }

  void set_error(std::string message) {
    std::lock_guard lock(mutex);
    if (error.empty()) error = std::move(message);
  }

  bool connect() {
    GError *error_out_ptr = nullptr;
    char *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error_out_ptr);
    if (address) {
      connection = g_dbus_connection_new_for_address_sync(
          address, static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
          nullptr, nullptr, &error_out_ptr);
      g_free(address);
    }
    if (!connection) {
      set_error(std::string("could not reach the session bus: ") + (error_out_ptr ? error_out_ptr->message : "unknown error"));
      if (error_out_ptr) g_error_free(error_out_ptr);
      return false;
    }
    g_dbus_connection_set_exit_on_close(connection, FALSE);
    const char *unique = g_dbus_connection_get_unique_name(connection);
    sender = unique ? unique : "";
    sender.erase(0, sender.rfind(':') + 1);
    for (char &c : sender)
      if (c == '.') c = '_';
    return true;
  }

  // Portals only know who is calling for apps that have a desktop entry, so
  // tell them the id before any other call on this connection.
  void register_app() {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(connection, kPortalBus, kPortalPath, kPortalRegistry, "Register", g_variant_new("(sa{sv})", app_id.c_str(), &options),
                                                 G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
    if (reply) {
      g_variant_unref(reply);
      return;
    }
    const bool missing = error && (g_dbus_error_is_remote_error(error)) &&
                         (g_str_equal(g_dbus_error_get_remote_error(error), "org.freedesktop.DBus.Error.UnknownInterface") ||
                          g_str_equal(g_dbus_error_get_remote_error(error), "org.freedesktop.DBus.Error.UnknownMethod") ||
                          g_str_equal(g_dbus_error_get_remote_error(error), "org.freedesktop.DBus.Error.ServiceUnknown"));
    if (!missing)
      std::fprintf(stderr, "qui/sys: could not register '%s' with the portal (%s); is %s.desktop installed?\n", app_id.c_str(),
                   error ? error->message : "unknown error", app_id.c_str());
    if (error) g_error_free(error);
  }

  guint version() {
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(connection, kPortalBus, kPortalPath, kProperties, "Get", g_variant_new("(ss)", kShortcuts, "version"),
                                                 G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
    guint value = 0;
    if (reply) {
      GVariant *inner = nullptr;
      g_variant_get(reply, "(v)", &inner);
      if (inner) {
        if (g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32)) value = g_variant_get_uint32(inner);
        g_variant_unref(inner);
      }
      g_variant_unref(reply);
    }
    if (error) g_error_free(error);
    return value;
  }

  bool subscribe_response(const std::string &path) {
    static const GDBusSignalCallback callback = [](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *parameters,
                                                   gpointer data) { static_cast<qsys_shortcuts *>(data)->on_response(parameters); };
    const guint id = g_dbus_connection_signal_subscribe(connection, kPortalBus, kRequest, "Response", path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                        callback, this, nullptr);
    return id != 0;
  }

  // The spec types this as an object path; xdg-desktop-portal-kde sends a string.
  static std::string handle_from(GVariant *results) {
    GVariant *value = g_variant_lookup_value(results, "session_handle", nullptr);
    std::string out;
    if (value) {
      if (g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH) || g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
        out = g_variant_get_string(value, nullptr);
      g_variant_unref(value);
    }
    return out;
  }

  void create_session() {
    session_request = next_token("qui_shortcuts_");
    const std::string session_token = next_token("qui_session_");
    const std::string path = request_path(session_request);
    if (!subscribe_response(path)) {
      set_error("could not listen for the portal's reply");
      return;
    }
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(session_request.c_str()));
    g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(session_token.c_str()));
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(connection, kPortalBus, kPortalPath, kShortcuts, "CreateSession", g_variant_new("(a{sv})", &options),
                                                 G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    if (!reply) {
      set_error(std::string("the GlobalShortcuts portal is unavailable: ") + (error ? error->message : "unknown error"));
      if (error) g_error_free(error);
      return;
    }
    g_variant_unref(reply);
    session = std::string(kPortalPath) + "/session/" + sender + "/" + session_token;
  }

  void bind_shortcuts() {
    bind_request = next_token("qui_bind_");
    const std::string path = request_path(bind_request);
    if (!subscribe_response(path)) {
      set_error("could not listen for the portal's reply");
      return;
    }
    const bool suggest = version() >= 2;

    GVariantBuilder shortcuts;
    g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
    for (size_t i = 0; i < ids.size(); ++i) {
      GVariantBuilder properties;
      g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&properties, "{sv}", "description", g_variant_new_string(descriptions[i].c_str()));
      if (suggest && !triggers[i].empty()) g_variant_builder_add(&properties, "{sv}", "preferred_trigger", g_variant_new_string(triggers[i].c_str()));
      g_variant_builder_add(&shortcuts, "(sa{sv})", ids[i].c_str(), &properties);
    }
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(bind_request.c_str()));

    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(connection, kPortalBus, kPortalPath, kShortcuts, "BindShortcuts",
                                                 g_variant_new("(oa(sa{sv})s@a{sv})", session.c_str(), &shortcuts, "", g_variant_builder_end(&options)),
                                                 G_VARIANT_TYPE("(o)"),
                                                 G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    if (!reply) {
      set_error(std::string("BindShortcuts failed: ") + (error ? error->message : "unknown error"));
      if (error) g_error_free(error);
    } else {
      g_variant_unref(reply);
    }
  }

  void report_unassigned(GVariant *results) {
    GVariant *list = g_variant_lookup_value(results, "shortcuts", G_VARIANT_TYPE("a(sa{sv})"));
    if (!list) return;
    std::string unassigned;
    GVariantIter iterator;
    const gchar *id = nullptr;
    GVariant *properties = nullptr;
    g_variant_iter_init(&iterator, list);
    while (g_variant_iter_next(&iterator, "(&s@a{sv})", &id, &properties)) {
      const gchar *trigger = nullptr;
      const bool wanted = !g_variant_lookup(properties, "trigger_description", "&s", &trigger) || !trigger || !*trigger;
      const bool known = std::find(ids.begin(), ids.end(), id) != ids.end();
      if (wanted && known) {
        if (!unassigned.empty()) unassigned += ", ";
        unassigned += id;
      }
      g_variant_unref(properties);
    }
    g_variant_unref(list);
    if (!unassigned.empty())
      std::fprintf(stderr,
                   "qui/sys: no key assigned for [%s]; set one in the desktop's shortcut settings. Portals only show their dialog for shortcuts "
                   "they have not seen before, so a dismissed dialog leaves them unassigned.\n",
                   unassigned.c_str());
  }

  void on_response(GVariant *parameters) {
    guint32 status = 2;
    GVariant *results = nullptr;
    g_variant_get(parameters, "(u@a{sv})", &status, &results);
    const bool creating = !session.empty() && !bound && bind_request.empty();
    if (creating) {
      if (status != 0) {
        set_error("the portal refused the shortcut session (code " + std::to_string(status) + ")");
        if (results) g_variant_unref(results);
        return;
      }
      const std::string handle = handle_from(results);
      if (!handle.empty()) session = handle;
      if (results) g_variant_unref(results);
      bind_shortcuts();
      return;
    }
    if (status == 0) {
      bound = true;
      report_unassigned(results);
    } else {
      set_error("the portal refused the shortcut request (code " + std::to_string(status) + ")");
    }
    if (results) g_variant_unref(results);
  }

  void run() {
    g_main_context_push_thread_default(context);
    if (!connect()) {
      signal_started();
      return;
    }
    register_app();
    static const GDBusSignalCallback activated_callback = [](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *,
                                                            GVariant *parameters, gpointer data) {
      auto *self = static_cast<qsys_shortcuts *>(data);
      const gchar *session = nullptr, *id = nullptr;
      guint64 timestamp = 0;
      GVariant *options = nullptr;
      g_variant_get(parameters, "(&o&st@a{sv})", &session, &id, &timestamp, &options);
      if (options) g_variant_unref(options);
      if (!self->session.empty() && session && self->session != session) return;
      if (self->fn && id) self->fn(id, self->user);
    };
    activated = g_dbus_connection_signal_subscribe(connection, kPortalBus, kShortcuts, "Activated", kPortalPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                  activated_callback, this, nullptr);
    create_session();
    signal_started();
    loop = g_main_loop_new(context, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    loop = nullptr;
    g_main_context_pop_thread_default(context);
  }

  void signal_started() {
    std::lock_guard lock(mutex);
    started = true;
    cv.notify_all();
  }
};

extern "C" {

qsys_shortcuts *qsys_shortcuts_open(const char *app_id, const qsys_shortcut *shortcuts, int count, qsys_shortcut_fn fn, void *user) {
  using namespace qui::sys;
  if (!app_id || !*app_id) {
    fail("an app id is required");
    return nullptr;
  }
  if (!shortcuts || count <= 0) {
    fail("at least one shortcut is required");
    return nullptr;
  }
  auto *self = new qsys_shortcuts();
  self->app_id = app_id;
  self->fn = fn;
  self->user = user;
  for (int i = 0; i < count; ++i) {
    if (!shortcuts[i].id || !*shortcuts[i].id) {
      fail("every shortcut needs an id");
      delete self;
      return nullptr;
    }
    self->ids.emplace_back(shortcuts[i].id);
    self->descriptions.emplace_back(shortcuts[i].description ? shortcuts[i].description : shortcuts[i].id);
    self->triggers.emplace_back(shortcuts[i].preferred_trigger ? shortcuts[i].preferred_trigger : "");
  }

  self->context = g_main_context_new();
  self->thread = std::thread([self] { self->run(); });

  std::unique_lock lock(self->mutex);
  const bool ready = self->cv.wait_for(lock, kStartTimeout, [self] { return self->started; });
  const std::string error = self->error;
  lock.unlock();
  if (!error.empty()) {
    self->thread.join();
    delete self;
    fail(error);
    return nullptr;
  }
  if (!ready) {
    // The bus never answered, so the thread is stuck in it with no way to stop.
    self->thread.detach();
    fail("the shortcut portal did not answer");
    return nullptr;
  }
  return self;
}

void qsys_shortcuts_close(qsys_shortcuts *shortcuts) {
  if (!shortcuts) return;
  g_main_context_invoke(shortcuts->context, [](gpointer data) -> gboolean {
    g_main_loop_quit(static_cast<qsys_shortcuts *>(data)->loop);
    return G_SOURCE_REMOVE;
  }, shortcuts);
  if (shortcuts->thread.joinable()) shortcuts->thread.join();
  if (shortcuts->connection) {
    if (!shortcuts->session.empty()) {
      GVariant *reply = g_dbus_connection_call_sync(shortcuts->connection, "org.freedesktop.portal.Desktop", shortcuts->session.c_str(),
                                                   "org.freedesktop.portal.Session", "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 2000,
                                                   nullptr, nullptr);
      if (reply) g_variant_unref(reply);
    }
    if (shortcuts->activated) g_dbus_connection_signal_unsubscribe(shortcuts->connection, shortcuts->activated);
    g_object_unref(shortcuts->connection);
  }
  if (shortcuts->context) g_main_context_unref(shortcuts->context);
  delete shortcuts;
}

}  // extern "C"
