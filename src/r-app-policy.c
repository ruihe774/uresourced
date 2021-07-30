/* SPDX-License-Identifier: LGPL-2.1+ */

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "r-app-monitor.h"
#include "r-app-policy.h"

#define DEFAULT_WEIGHT 100
#define ACTIVE_WEIGHT 300
#define BOOST_WEIGHT 200

struct _RAppPolicy
{
  GObject     parent_instance;

  GDBusProxy *proxy;
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
  g_variant_builder_close (&builder);

  g_info ("Setting resources on %s (CPUWeight: %ld)", app->name,
             app->cpu_weight);

  g_dbus_proxy_call (self->proxy, "SetUnitProperties",
                     g_variant_builder_end (&builder), G_DBUS_CALL_FLAGS_NONE,
                     1000, NULL, set_application_resources_cb, self);
}

static void
app_info_changed (gpointer *data, gpointer arg, G_GNUC_UNUSED GObject *object)
{
  RAppPolicy *self = R_APP_POLICY (data);
  RAppInfo *app = (RAppInfo *) arg;

  g_debug ("App Info changed: %s", app->name);
  g_debug ("Timestamp: %ld, Boosted: %d", app->timestamp, (int)app->boosted);

  /*
   * For now, we only change the CPUWeight.
   * Timestamp is used for determining if an application's window is focused.
   * `boosted` is used by other sources like audio or games, to give an additional
   * boost irrespective of the application being focused.
   */
  app->cpu_weight = (app->timestamp == -1) ? ACTIVE_WEIGHT : DEFAULT_WEIGHT;
  if (app->boosted != 0)
    app->cpu_weight += BOOST_WEIGHT;

  if (self->proxy)
    set_application_resources (self, app);
}

void
r_app_policy_start (RAppPolicy *self, RAppMonitor *monitor)
{
  g_signal_connect_object (monitor, "changed", G_CALLBACK (app_info_changed),
                           self, G_CONNECT_SWAPPED);
}

void
r_app_policy_stop (RAppPolicy *self)
{
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
