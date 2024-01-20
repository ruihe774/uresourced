/* SPDX-License-Identifier: LGPL-2.1+ */

#include <sys/inotify.h>
#include <sys/types.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "r-app-monitor.h"
#include "utils.h"

#define INOTIFY_EVENT_BUF_LEN                                                 \
  (10 * (sizeof (struct inotify_event) + NAME_MAX + 1))

struct _RAppMonitor
{
  GObject     parent_instance;

  uid_t       uid;
  gchar      *app_slice_path;
  gint        inotify_fd;

  GIOChannel *channel;
  guint       channel_watch_id;

  GHashTable *path_to_wd_map;
  GHashTable *wd_to_path_map;
  GHashTable *app_info_map;
};

G_DEFINE_TYPE (RAppMonitor, r_app_monitor, G_TYPE_OBJECT);

RAppMonitor *
r_app_monitor_new (void)
{
  return g_object_new (R_TYPE_APP_MONITOR, NULL);
}

RAppMonitor *
r_app_monitor_get_default (void)
{
  static RAppMonitor *self = NULL;

  if (G_UNLIKELY (self == NULL))
    self = r_app_monitor_new ();

  return self;
}

static void
r_app_monitor_finalize (GObject *object)
{
  RAppMonitor *self = (RAppMonitor *) object;

  if (self->channel)
    {
      g_io_channel_shutdown (self->channel, TRUE, NULL);
      g_io_channel_unref (self->channel);
    }

  g_clear_pointer (&self->app_slice_path, g_free);
  g_clear_pointer (&self->path_to_wd_map, g_hash_table_destroy);
  g_clear_pointer (&self->wd_to_path_map, g_hash_table_destroy);
  g_clear_pointer (&self->app_info_map, g_hash_table_destroy);

  if (self->inotify_fd >= 0)
    close (self->inotify_fd);

  G_OBJECT_CLASS (r_app_monitor_parent_class)->finalize (object);
}

gboolean
inotify_add_cgroup_dir (RAppMonitor *self, gchar *path)
{
  gpointer old_path;
  gpointer old_wd;
  gint wd;

  wd = inotify_add_watch (self->inotify_fd, path,
                          IN_ATTRIB | IN_CREATE | IN_DELETE);
  if (wd == -1)
    return FALSE;

  if (g_hash_table_steal_extended(self->path_to_wd_map, path, &old_path, &old_wd))
    {
      g_free (old_path);
      g_hash_table_remove (self->wd_to_path_map, old_wd);

      inotify_rm_watch (self->inotify_fd, GPOINTER_TO_INT (old_wd));
    }

  g_hash_table_replace (self->path_to_wd_map, g_strdup (path),
                        GINT_TO_POINTER (wd));
  g_hash_table_replace (self->wd_to_path_map, GINT_TO_POINTER (wd),
                        g_strdup (path));

  if (strcmp (path, self->app_slice_path) && !g_str_has_suffix (path, ".slice"))
    r_app_monitor_get_app_info_from_path (self, path);

  g_debug ("Watching %s using wd %d", path, wd);

  return TRUE;
}

void
inotify_add_recursive_watch_on_dir (RAppMonitor *self, const gchar *dir_path)
{
  GDir *dir;
  gchar *sub_dir_name, *sub_dir_path;

  dir = g_dir_open (dir_path, 0, NULL);
  if (!dir)
    {
      g_debug ("Failed to open directory: %s", dir_path);
      return;
    }

  sub_dir_name = g_strdup (g_dir_read_name (dir));
  while (sub_dir_name)
    {
      sub_dir_path = g_strdup_printf ("%s/%s", dir_path, sub_dir_name);
      if (g_file_test (sub_dir_path, G_FILE_TEST_IS_DIR))
        {
          if (!inotify_add_cgroup_dir (self, sub_dir_path))
            {
              g_debug ("inotify_add_watch failed for directory: %s",
                       sub_dir_path);
              continue;
            }
          inotify_add_recursive_watch_on_dir (self, sub_dir_path);
        }

      g_free (sub_dir_name);
      g_free (sub_dir_path);
      sub_dir_name = g_strdup (g_dir_read_name (dir));
    }

  g_dir_close (dir);
}


/**
 * get_weight:
 * @path: Application Path
 *
 * This function reads content from given path and removes non-numeric
 * text like "default" to get only the weight.
 *
 * Returns: A guint64 containing the CPU or IO weight for the unit, or 0
 */
guint64
get_weight (const gchar *path)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *contents = NULL;
  g_autofree gchar *sanitized = NULL;
  gint64 value;
  gboolean ok;

  ok = g_file_get_contents (path, &contents, NULL, &error);
  if (!ok)
    {
      g_debug ("Failed to get weight from file: %s", error->message);
      return 0;
    }

  g_strstrip (contents);
  if (g_str_has_prefix (contents, "default "))
    sanitized = g_strdup (&contents[8]);
  else
    sanitized = g_strdup (contents);

  value = g_ascii_strtoull (sanitized, NULL, 0);
  return value;
}

/**
 * create_app_info_default:
 *
 * This function creates from a RAppInfo struct with
 * all the default values set, except for the name and path.
 * By default a non active timestamp is set.
 *
 * Returns: Pointer to RAppInfo
 */
static RAppInfo *
create_app_info_default ()
{
  RAppInfo *app;

  app = g_new0 (RAppInfo, 1);

  app->cpu_weight = 100;
  app->io_weight = 100;
  app->timestamp = g_get_monotonic_time ();
  app->boosted = BOOST_NONE;

  return app;
}


/**
 * r_app_monitor_get_app_info_from_path:
 * @app_monitor: RAppMonitor
 * @app_path: Application Path
 *
 * This function either creates a new RAppInfo with values
 * from the given cgroup path or returns the existing RAppInfo
 * in the hash table using the app_path.
 *
 * Returns: RAppInfo* for a valid path,or %NULL
 */
RAppInfo *
r_app_monitor_get_app_info_from_path (RAppMonitor *app_monitor, gchar *app_path)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autofree gchar *cpu_weight_path = NULL;
  g_autofree gchar *io_weight_path = NULL;
  g_autofree gchar *contents = NULL;
  gboolean created = FALSE;
  RAppInfo *app;

  if (!g_file_test (app_path, G_FILE_TEST_IS_DIR))
    {
      g_debug ("Can't get app info, app path not valid");
      return NULL;
    }

  if (!g_str_has_prefix (app_path, app_monitor->app_slice_path))
    {
      g_debug ("Can't get app info. app cgroup not under app.slice, outside managed area.");
      return NULL;
    }

  if (g_hash_table_contains (app_monitor->app_info_map, app_path))
    {
      app = (RAppInfo *) g_hash_table_lookup (app_monitor->app_info_map, app_path);
    }
  else
    {
      created = TRUE;
      app = create_app_info_default ();
      app->path = g_strdup (app_path);
      app->name = get_unit_name_from_path (app_path);
      g_strstrip (app->path);
      g_strstrip (app->name);
    }

  cpu_weight_path = g_strconcat (app_path, "/cpu.weight", NULL);
  io_weight_path = g_strconcat (app_path, "/io.weight", NULL);

  app->cpu_weight = get_weight (cpu_weight_path);
  if (!app->cpu_weight)
    {
      g_debug ("Failed to get cpu weight for %s, using default(100)", app->name);
      app->cpu_weight = 100;
    }

  app->io_weight = get_weight (io_weight_path);
  if (!app->io_weight)
    {
      g_debug ("Failed to get io weight for %s, using default(100)", app->name);
      app->io_weight = 100;
    }

  file = g_file_new_for_path (app_path);
  file_info = g_file_query_info (file, "xattr::xdg.inactive-since",
                                 G_FILE_QUERY_INFO_NONE, NULL, &error);
  if (!file_info)
    {
      g_debug ("Failed to query xattr for file: %s", error->message);
    }
  else
    {
      contents = g_strdup (g_file_info_get_attribute_string (
                             file_info, "xattr::xdg.inactive-since"));

      if (contents)
        app->timestamp = g_ascii_strtoll (g_strstrip (contents), NULL, 0);
    }

  if (created)
    g_hash_table_replace (app_monitor->app_info_map, g_strdup (app_path), app);

  return app;
}

static void
reset_app_info (G_GNUC_UNUSED gpointer key,
                gpointer value, gpointer data)
{
  RAppMonitor *self = (RAppMonitor *) data;
  RAppInfo *app = (RAppInfo *) value;

  if (app->timestamp == -1 || app->boosted != 0)
    {
      app->timestamp = g_get_monotonic_time ();
      app->boosted = BOOST_NONE;

      r_app_monitor_app_info_changed (self, app);
    }
}

void
r_app_monitor_reset_all_apps (RAppMonitor *self)
{
  g_hash_table_foreach (self->app_info_map, reset_app_info, self);
}

static void
destroy_app_info (gpointer data)
{
  RAppInfo *app = (RAppInfo *) data;

  g_clear_pointer (&app->name, g_free);
  g_clear_pointer (&app->path, g_free);
  g_free (app);
}

void
r_app_monitor_app_info_changed (RAppMonitor *self, RAppInfo *info)
{
  g_signal_emit_by_name (self, "changed", info);
}

/**
 * handle_inotify_event:
 * @self: RAppMonitor
 * @i: inotify event
 *
 * Handles these 3 inotify events:
 * IN_ATTRIB: change of xattr, possible new timestamp set.
 *            Updates/Creates RAppInfo accordingly.
 * IN_CREATE: New directory created. Adds a recursive watch
 *            on the directory and all its subdirectories.
 * IN_DELETE: Removes tracking information from all the
 *            hash tables.
 */
static void
handle_inotify_event (RAppMonitor *self, struct inotify_event *i)
{
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *app_path = NULL;
  gpointer wd_temp;
  RAppInfo *app;

  if (i->len == 0)
    return;

  parent_path = g_strdup ((gchar *) g_hash_table_lookup (
                            self->wd_to_path_map, GINT_TO_POINTER (i->wd)));
  app_path = g_strdup_printf ("%s/%s", parent_path, i->name);

  g_debug ("inotify event: Name = %s, Parent = %s", i->name, parent_path);

  if (i->mask == (IN_ATTRIB | IN_ISDIR))
    {
      app = r_app_monitor_get_app_info_from_path (self, app_path);
      if (app)
        r_app_monitor_app_info_changed (self, app);
    }

  if (i->mask == (IN_CREATE | IN_ISDIR))
    {
      inotify_add_cgroup_dir (self, app_path);
      inotify_add_recursive_watch_on_dir (self, app_path);
    }

  if (i->mask == (IN_DELETE | IN_ISDIR))
    {
      wd_temp = g_hash_table_lookup (self->path_to_wd_map, app_path);
      g_hash_table_remove (self->path_to_wd_map, app_path);
      g_hash_table_remove (self->wd_to_path_map, wd_temp);
      g_hash_table_remove (self->app_info_map, app_path);

      inotify_rm_watch (self->inotify_fd, GPOINTER_TO_INT (wd_temp));
    }
}

static gboolean
received_inotify_data (GIOChannel *channel, G_GNUC_UNUSED GIOCondition cond,
                       G_GNUC_UNUSED gpointer user_data)
{
  struct inotify_event *event;
  RAppMonitor *self = R_APP_MONITOR (user_data);
  GIOStatus status;
  gchar buffer[INOTIFY_EVENT_BUF_LEN];
  gsize bytes_read;
  gchar *p;

  status = g_io_channel_read_chars (channel, buffer, sizeof (buffer) - 1,
                                    &bytes_read, NULL);

  if (status != G_IO_STATUS_NORMAL && status != G_IO_STATUS_AGAIN)
    return FALSE;

  if (status == G_IO_STATUS_NORMAL)
    {
      for (p = buffer; p < buffer + bytes_read;)
        {
          event = (struct inotify_event *) p;
          handle_inotify_event (self, event);
          p += sizeof (struct inotify_event) + event->len;
        }
    }

  return TRUE;
}

void
r_app_monitor_start (RAppMonitor *self)
{
  if (!inotify_add_cgroup_dir (self, self->app_slice_path))
    g_error ("Failed inotify_add_watch for app.slice directory.");

  inotify_add_recursive_watch_on_dir (self, self->app_slice_path);

  self->channel_watch_id = g_io_add_watch (self->channel,
                                           G_IO_IN | G_IO_HUP | G_IO_NVAL | G_IO_ERR,
                                           received_inotify_data, self);
}

void
r_app_monitor_stop (RAppMonitor *self)
{
  g_clear_handle_id (&self->channel_watch_id, g_source_remove);
}

static GObject *
r_app_monitor_constructor (GType type, guint n_construct_params,
                           GObjectConstructParam *construct_params)
{
  static GObject *self = NULL;

  if (G_UNLIKELY (self == NULL))
    {
      self = G_OBJECT_CLASS (r_app_monitor_parent_class)
             ->constructor (type, n_construct_params, construct_params);
      g_object_add_weak_pointer (self, (gpointer) & self);
      return self;
    }

  return g_object_ref (self);
}

static void
r_app_monitor_class_init (RAppMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructor = r_app_monitor_constructor;
  object_class->finalize = r_app_monitor_finalize;

  g_signal_new ("changed", R_TYPE_APP_MONITOR, G_SIGNAL_RUN_LAST, 0, NULL,
                NULL, NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
r_app_monitor_init (RAppMonitor *self)
{
  self->uid = getuid ();
  self->app_slice_path = g_strdup_printf ("/sys/fs/cgroup/user.slice"
                                          "/user-%1$i.slice/"
                                          "user@%1$i.service/app.slice",
                                          self->uid);
  self->path_to_wd_map
    = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->wd_to_path_map
    = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  self->app_info_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                              destroy_app_info);

  self->inotify_fd = inotify_init ();
  if (self->inotify_fd < 0)
    g_error ("inotify_init failed");

  self->channel = g_io_channel_unix_new (self->inotify_fd);
  if (!self->channel)
    {
      close (self->inotify_fd);
      g_error ("g_io_channel_unix_new failed");
    }

  g_io_channel_set_close_on_unref (self->channel, TRUE);
  g_io_channel_set_encoding (self->channel, NULL, NULL);
  g_io_channel_set_buffered (self->channel, FALSE);
}
