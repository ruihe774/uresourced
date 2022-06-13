/* SPDX-License-Identifier: LGPL-2.1+ */

#include <glib-object.h>
#include <glib.h>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

#include "r-app-monitor.h"
#include "r-pw-monitor.h"
#include "utils.h"

typedef struct _ProxyData
{
  RPwMonitor      *data;
  RAppMonitor     *app_monitor;
  gboolean         first;
  struct pw_proxy *proxy;
  uint32_t         id;
  uint32_t         permissions;
  uint32_t         version;
  char            *type;
  void            *info;
  pw_destroy_t     destroy;
  struct spa_hook  proxy_listener;
  struct spa_hook  object_listener;
} ProxyData;

typedef struct _PipeWireSource
{
  GSource         base;

  struct pw_loop *pipewire_loop;
} PipeWireSource;

struct _RPwMonitor
{
  GObject             parent_instance;

  PipeWireSource     *pipewire_source;
  struct pw_context  *pipewire_context;
  struct pw_core     *pipewire_core;

  struct pw_registry *pipewire_registry;
  struct spa_hook     pipewire_registry_listener;

  RAppMonitor        *app_monitor;
};

G_DEFINE_TYPE (RPwMonitor, r_pw_monitor, G_TYPE_OBJECT);

RPwMonitor *
r_pw_monitor_new (void)
{
  return g_object_new (R_TYPE_PW_MONITOR, NULL);
}

static void
r_pw_monitor_finalize (GObject *object)
{
  RPwMonitor *self = (RPwMonitor *) object;

  if (self->pipewire_registry)
    pw_proxy_destroy ((struct pw_proxy *) self->pipewire_registry);
  g_clear_pointer (&self->pipewire_core, pw_core_disconnect);
  g_clear_pointer (&self->pipewire_context, pw_context_destroy);
  if (self->pipewire_source)
    {
      g_source_destroy (&self->pipewire_source->base);
      g_source_unref (&self->pipewire_source->base);
    }
  pw_deinit ();

  G_OBJECT_CLASS (r_pw_monitor_parent_class)->finalize (object);
}

/**
 * handle_inotify_event:
 * @object: Proxy data
 * @info: Pipewire node info struct
 *
 * Filters incoming node event for pipewire-pulse api,
 * It acts on the following 3 audio states:
 * running - give boost to application (set BOOST_AUDIO flag)
 * idle - reset BOOST_AUDIO flag
 * suspended - reset BOOST_AUDIO flag
 */
static void
node_event_info (void *object, const struct pw_node_info *info)
{
  ProxyData *data = object;
  g_autofree gchar *client_api = NULL;
  g_autofree gchar *app_state = NULL;
  g_autofree gchar *app_path = NULL;
  pid_t app_pid;
  RAppInfo *app;

  info = data->info = pw_node_info_update (data->info, info);

  if (data->first)
    data->first = false;

  app_state = g_strdup (pw_node_state_as_string (info->state));
  if (info->state == PW_NODE_STATE_ERROR && info->error)
    g_debug ("Error: %s", info->error);

  if (info->props == NULL || info->props->n_items == 0)
    return;

  client_api = g_strdup (spa_dict_lookup (info->props, "client.api"));
  if (!client_api || strcmp (client_api, "pipewire-pulse"))
    return;

  app_pid = g_ascii_strtoll (spa_dict_lookup (info->props, "application.process.id"), NULL, 10);
  app_path = get_unit_cgroup_path_from_pid (app_pid);
  if (!app_path)
    return;

  g_debug ("Audio App PID: %d, Audio state: %s", app_pid, app_state);

  app = r_app_monitor_get_app_info_from_path (data->app_monitor, app_path);
  if (!app)
    return;

  if (strcmp (app_state, "running") == 0)
    {
      app->boosted |= BOOST_AUDIO;
      r_app_monitor_app_info_changed (data->app_monitor, app);
    }
  else if (strcmp (app_state, "idle") == 0)
    {
      app->boosted &= ~BOOST_AUDIO;
      r_app_monitor_app_info_changed (data->app_monitor, app);
    }
  else if (strcmp (app_state, "suspended") == 0)
    {
      app->boosted &= ~BOOST_AUDIO;
      r_app_monitor_app_info_changed (data->app_monitor, app);
    }
}

static const struct pw_node_events node_events
  = { PW_VERSION_NODE_EVENTS, .info = node_event_info };

static void
removed_proxy (void *data)
{
  ProxyData *pd = data;

  pw_proxy_destroy (pd->proxy);
}

static void
destroy_proxy (void *data)
{
  ProxyData *pd = data;

  if (!pd->info)
    return;

  if (pd->destroy)
    pd->destroy (pd->info);
  pd->info = NULL;
  free (pd->type);
}

static const struct pw_proxy_events proxy_events = {
  PW_VERSION_PROXY_EVENTS,
  .removed = removed_proxy,
  .destroy = destroy_proxy,
};

static void
registry_event_global (void *data, uint32_t id, uint32_t permissions,
                       const char *type, uint32_t version,
                       G_GNUC_UNUSED const struct spa_dict *props)
{
  RPwMonitor *self = R_PW_MONITOR (data);
  struct pw_proxy *proxy;
  ProxyData *pd;

  if (g_strcmp0 (type, PW_TYPE_INTERFACE_Node) != 0)
    return;

  proxy = pw_registry_bind (self->pipewire_registry, id, type, PW_VERSION_NODE,
                            sizeof (ProxyData));
  if (proxy == NULL)
    {
      g_warning ("Failed to create proxy");
      return;
    }

  pd = pw_proxy_get_user_data (proxy);
  pd->data = self;
  pd->app_monitor = self->app_monitor;
  pd->first = true;
  pd->proxy = proxy;
  pd->id = id;
  pd->permissions = permissions;
  pd->version = version;
  pd->type = strdup (type);
  pd->destroy = (pw_destroy_t) pw_node_info_free;
  pw_proxy_add_object_listener (proxy, &pd->object_listener, &node_events, pd);
  pw_proxy_add_listener (proxy, &pd->proxy_listener, &proxy_events, pd);
}

static const struct pw_registry_events registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global = registry_event_global,
};

static gboolean
pipewire_loop_source_prepare (G_GNUC_UNUSED GSource *base, int *timeout)
{
  *timeout = -1;
  return FALSE;
}

static gboolean
pipewire_loop_source_dispatch (GSource                  *source,
                               G_GNUC_UNUSED GSourceFunc callback,
                               G_GNUC_UNUSED gpointer    user_data)
{
  PipeWireSource *pipewire_source = (PipeWireSource *) source;
  int result;

  result = pw_loop_iterate (pipewire_source->pipewire_loop, 0);
  if (result < 0)
    g_warning ("pipewire_loop_iterate failed: %s", spa_strerror (result));

  return TRUE;
}

static void
pipewire_loop_source_finalize (GSource *source)
{
  PipeWireSource *pipewire_source = (PipeWireSource *) source;

  pw_loop_leave (pipewire_source->pipewire_loop);
  pw_loop_destroy (pipewire_source->pipewire_loop);
}

static GSourceFuncs pipewire_source_funcs
  = { pipewire_loop_source_prepare,  NULL, pipewire_loop_source_dispatch,
      pipewire_loop_source_finalize, NULL, NULL };

static PipeWireSource *
create_pipewire_source ()
{
  PipeWireSource *pipewire_source;

  pipewire_source = (PipeWireSource *) g_source_new (&pipewire_source_funcs,
                                                     sizeof (PipeWireSource));

  pipewire_source->pipewire_loop = pw_loop_new (NULL);
  if (!pipewire_source->pipewire_loop)
    {
      g_source_unref ((GSource *) pipewire_source);
      return NULL;
    }

  g_source_add_unix_fd (&pipewire_source->base,
                        pw_loop_get_fd (pipewire_source->pipewire_loop),
                        G_IO_IN | G_IO_ERR);

  pw_loop_enter (pipewire_source->pipewire_loop);

  return pipewire_source;
}

void
r_pw_monitor_start (RPwMonitor *self, RAppMonitor *monitor)
{
  if (!self->pipewire_source || !self->pipewire_context || !self->pipewire_core)
    return;

  self->app_monitor = monitor;

  g_source_attach (&self->pipewire_source->base, NULL);
}

void
r_pw_monitor_stop (RPwMonitor *self)
{
  if (!self->pipewire_source || !self->pipewire_context || !self->pipewire_core)
    return;

  g_source_destroy (&self->pipewire_source->base);
}

static void
r_pw_monitor_class_init (RPwMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = r_pw_monitor_finalize;
}

static void
r_pw_monitor_init (RPwMonitor *self)
{
  pw_init (NULL, NULL);

  self->pipewire_source = create_pipewire_source ();
  if (!self->pipewire_source)
    {
      g_message ("Failed to create PipeWire source");
      return;
    }

  self->pipewire_context
    = pw_context_new (self->pipewire_source->pipewire_loop, NULL, 0);
  if (!self->pipewire_context)
    {
      g_message ("Failed to create PipeWire context");
      return;
    }

  self->pipewire_core = pw_context_connect (
    self->pipewire_context,
    pw_properties_new (PW_KEY_REMOTE_NAME, NULL, NULL), 0);
  if (!self->pipewire_core)
    {
      g_message ("Can't connect to PipeWire context");
      return;
    }

  self->pipewire_registry
    = pw_core_get_registry (self->pipewire_core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener (self->pipewire_registry,
                            &self->pipewire_registry_listener,
                            &registry_events, self);
}
