/* SPDX-License-Identifier: LGPL-2.1+ */

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "r-app-monitor.h"
#include "r-game-monitor.h"
#include "utils.h"

struct _RGameMonitor
{
  GObject      parent_instance;

  GDBusProxy  *proxy;
  RAppMonitor *app_monitor;
};

G_DEFINE_TYPE (RGameMonitor, r_game_monitor, G_TYPE_OBJECT);

RGameMonitor *
r_game_monitor_new (void)
{
  return g_object_new (R_TYPE_GAME_MONITOR, NULL);
}

static void
r_game_monitor_finalize (GObject *object)
{
  RGameMonitor *self = (RGameMonitor *) object;

  g_clear_object (&self->proxy);

  G_OBJECT_CLASS (r_game_monitor_parent_class)->finalize (object);
}

static void
r_game_monitor_boost_game_from_pid (RGameMonitor *self, pid_t pid, gboolean is_registered)
{
  g_autofree gchar *app_path = NULL;
  RAppInfo *app;

  app_path = get_unit_cgroup_path_from_pid (pid);
  if (!app_path)
    return;

  app = r_app_monitor_get_app_info_from_path (self->app_monitor, app_path);
  if (!app)
    return;

  if (is_registered)
    app->boosted |= BOOST_GAME;
  else
    app->boosted &= ~BOOST_GAME;

  r_app_monitor_app_info_changed (self->app_monitor, app);
}

static void
gamemode_on_signal_received (G_GNUC_UNUSED GDBusProxy *proxy,
                             G_GNUC_UNUSED gchar      *sender_name,
                             gchar                    *signal_name,
                             GVariant                 *params,
                             gpointer                  user_data)
{
  RGameMonitor *self = R_GAME_MONITOR (user_data);
  pid_t pid = 0;
  gboolean is_registered;

  if (g_strcmp0 (signal_name, "GameRegistered") == 0)
    is_registered = TRUE;
  else if (g_strcmp0 (signal_name, "GameUnregistered") == 0)
    is_registered = FALSE;
  else
    return;

  if (!g_variant_is_of_type (params, G_VARIANT_TYPE ("(io)")))
    return;

  g_variant_get_child (params, 0, "i", &pid);
  if (!pid)
    return;

  r_game_monitor_boost_game_from_pid (self, pid, is_registered);
}

void
r_game_monitor_start (RGameMonitor *self, RAppMonitor *monitor)
{
  self->app_monitor = monitor;
}

void
r_game_monitor_stop (RGameMonitor *self)
{
  g_clear_object (&self->proxy);
}

static void
gamemode_bus_proxy_ready_cb (G_GNUC_UNUSED GObject *source, GAsyncResult *res,
                             gpointer data)
{
  RGameMonitor *self = R_GAME_MONITOR (data);

  g_autoptr(GError) error = NULL;
  GDBusProxy *proxy;

  proxy = g_dbus_proxy_new_for_bus_finish (res, &error);

  if (!proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_error ("Failed to create GDBusProxy: %s\n", error->message);
      return;
    }

  self->proxy = proxy;

  g_signal_connect (self->proxy,
                    "g-signal",
                    G_CALLBACK (gamemode_on_signal_received),
                    self);
}

static void
r_game_monitor_class_init (RGameMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = r_game_monitor_finalize;
}

static void
r_game_monitor_init (RGameMonitor *self)
{
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START
                            | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                            NULL, "com.feralinteractive.GameMode",
                            "/com/feralinteractive/GameMode",
                            "com.feralinteractive.GameMode", NULL,
                            gamemode_bus_proxy_ready_cb, self);
}
