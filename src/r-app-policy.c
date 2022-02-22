/* SPDX-License-Identifier: LGPL-2.1+ */

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "uresourced-config.h"
#include "r-app-monitor.h"
#include "r-app-policy.h"

struct _RAppPolicy
{
  GObject      parent_instance;

  GDBusProxy  *proxy;
  RAppMonitor *app_monitor;

  gint         default_cpu_weight;
  gint         default_io_weight;

  gint         active_cpu_weight;
  gint         active_io_weight;

  gint         boost_cpu_weight_inc;
  gint         boost_io_weight_inc;
};

G_DEFINE_TYPE (RAppPolicy, r_app_policy, G_TYPE_OBJECT);

RAppPolicy *
r_app_policy_new (void)
{
  return g_object_new (R_TYPE_APP_POLICY, NULL);
}

static void
r_app_policy_finalize (GObject *object)
{
  RAppPolicy *self = (RAppPolicy *) object;

  g_clear_object (&self->proxy);

  G_OBJECT_CLASS (r_app_policy_parent_class)->finalize (object);
}

static void
set_application_resources_cb (GObject *source_object, GAsyncResult *res,
                              G_GNUC_UNUSED gpointer user_data)
{
  GDBusProxy *proxy = G_DBUS_PROXY (source_object);

  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) var = NULL;

  var = g_dbus_proxy_call_finish (proxy, res, &error);
  if (error)
    g_debug ("Failed to set resource properties on app: %s", error->message);
}

static void
set_application_resources (RAppPolicy *self, RAppInfo *app)
{
  GVariantBuilder builder
    = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(sba(sv))"));

  g_variant_builder_add (&builder, "s", app->name);
  g_variant_builder_add (&builder, "b", TRUE);
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("a(sv)"));
  g_variant_builder_add (&builder, "(sv)", "CPUWeight",
                         g_variant_new_uint64 (app->cpu_weight));
  g_variant_builder_add (&builder, "(sv)", "IOWeight",
                         g_variant_new_uint64 (app->io_weight));
  g_variant_builder_close (&builder);

  g_info ("Setting resources on %s (CPUWeight: %ld, IOWeight: %ld)", app->name,
          app->cpu_weight, app->io_weight);

  g_dbus_proxy_call (self->proxy, "SetUnitProperties",
                     g_variant_builder_end (&builder), G_DBUS_CALL_FLAGS_NONE,
                     1000, NULL, set_application_resources_cb, self);
}

static void
app_info_changed (gpointer *data, gpointer arg, G_GNUC_UNUSED GObject *object)
{
  RAppPolicy *policy = R_APP_POLICY (data);
  RAppInfo *app = (RAppInfo *) arg;

  g_debug ("App Info changed: %s", app->name);
  g_debug ("Timestamp: %ld, Boosted: %d", app->timestamp, (int) app->boosted);

  /*
   * Timestamp is used for determining if an application's window is focused.
   * `boosted` is used by other sources like audio or games, to give an additional
   * boost irrespective of the application being focused.
   */
  app->cpu_weight = (app->timestamp == -1) ? policy->active_cpu_weight : policy->default_cpu_weight;
  app->io_weight = (app->timestamp == -1) ? policy->active_io_weight : policy->default_io_weight;
  if (app->boosted != 0)
    {
      app->cpu_weight += policy->boost_cpu_weight_inc;
      app->io_weight += policy->boost_io_weight_inc;
    }

  if (policy->proxy)
    set_application_resources (policy, app);
}

static inline void
set_integer_from_key_file (GKeyFile *file,
                           const char *group,
                           const char *key,
                           gint       *out)
{
  g_autoptr(GError) error = NULL;
  gint value;

  value = g_key_file_get_integer (file, group, key, &error);

  if (error)
    {
      g_debug ("Could not parse key %s in group %s, keeping value %d: %s",
               key, group, *out, error->message);
      return;
    }

  *out = value;
}

static void
read_config (RAppPolicy *self)
{
  g_autoptr(GKeyFile) file = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *user_config_path = NULL;

  self->default_cpu_weight = 100;
  self->default_io_weight = 100;
  self->active_cpu_weight = 100;
  self->active_io_weight = 100;
  self->boost_cpu_weight_inc = 0;
  self->boost_io_weight_inc = 0;

  file = g_key_file_new ();
  user_config_path = g_strdup_printf ("%s/uresourced.conf", g_get_user_config_dir ());

  if (!g_key_file_load_from_file (file, user_config_path, G_KEY_FILE_NONE, NULL))
    {
      g_debug ("Could not read %s, trying default location", user_config_path);

      if (!g_key_file_load_from_file (file, SYSCONFDIR "/uresourced.conf", G_KEY_FILE_NONE, &error))
        {
          g_warning ("Could not read default configuration file: %s", error->message);
          goto out;
        }
    }

  set_integer_from_key_file (file, "AppBoost", "DefaultCPUWeight", &self->default_cpu_weight);
  self->default_cpu_weight = CLAMP (self->default_cpu_weight, 1, 10000);

  set_integer_from_key_file (file, "AppBoost", "DefaultIOWeight", &self->default_io_weight);
  self->default_io_weight = CLAMP (self->default_io_weight, 1, 10000);

  set_integer_from_key_file (file, "AppBoost", "ActiveCPUWeight", &self->active_cpu_weight);
  self->active_cpu_weight = CLAMP (self->active_cpu_weight, 1, 10000);

  set_integer_from_key_file (file, "AppBoost", "ActiveIOWeight", &self->active_io_weight);
  self->active_io_weight = CLAMP (self->active_io_weight, 1, 10000);

  set_integer_from_key_file (file, "AppBoost", "BoostCPUWeightInc", &self->boost_cpu_weight_inc);
  self->boost_cpu_weight_inc = CLAMP (self->boost_cpu_weight_inc, 0, 10000 - self->active_cpu_weight);

  set_integer_from_key_file (file, "AppBoost", "BoostIOWeightInc", &self->boost_io_weight_inc);
  self->boost_io_weight_inc = CLAMP (self->boost_io_weight_inc, 0, 10000 - self->active_io_weight);

out:
  g_info ("CPU Configuration: Default CPUWeight: %d, Active CPUWeight: %d, Boost CPUWeight: %d",
          self->default_cpu_weight,
          self->active_cpu_weight,
          self->boost_cpu_weight_inc);
  g_info ("IO Configuration: Default IOWeight: %d, Active IOWeight: %d, Boost IOWeight: %d",
          self->default_io_weight,
          self->active_io_weight,
          self->boost_io_weight_inc);
}

void
r_app_policy_start (RAppPolicy *self, RAppMonitor *monitor)
{
  self->app_monitor = monitor;

  read_config (self);

  g_signal_connect_object (monitor, "changed", G_CALLBACK (app_info_changed),
                           self, G_CONNECT_SWAPPED);
}

void
r_app_policy_stop (RAppPolicy *self)
{
  r_app_monitor_reset_all_apps (self->app_monitor);

  g_dbus_connection_flush_sync (g_dbus_proxy_get_connection (self->proxy),
                                NULL, NULL);

  g_clear_object (&self->proxy);
}

static void
systemd_bus_proxy_ready_cb (G_GNUC_UNUSED GObject *source, GAsyncResult *res,
                            gpointer data)
{
  RAppPolicy *self = R_APP_POLICY (data);

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
}

static void
r_app_policy_class_init (RAppPolicyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = r_app_policy_finalize;
}

static void
r_app_policy_init (RAppPolicy *self)
{
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES
                            | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS
                            | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                            NULL, "org.freedesktop.systemd1",
                            "/org/freedesktop/systemd1",
                            "org.freedesktop.systemd1.Manager", NULL,
                            systemd_bus_proxy_ready_cb, self);
}
