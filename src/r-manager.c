//* SPDX-License-Identifier: LGPL-2.1+ */

#include <gio/gio.h>
#include "r-manager.h"
#include "r-sd-login.h"
#include "utils.h"
#include "uresourced-config.h"

#define WEIGHT_IGNORE G_MININT

typedef struct {
  gint    cpu_weight;
  gint    io_weight;
  guint64 memory_min;
  guint64 memory_low;
} RAllocation;

struct _RManager
{
  GObject parent_instance;

  GArray *graphical_users;
  GArray *all_users;

  gint pending_calls;
  GDBusConnection *connection;
  guint bus_name;
  guint dbus_obj;
  RSdLogin *login;

  /* Configuration */
  guint64     available_ram;
  guint64     max_users_memory_min;
  guint64     max_users_memory_low;
  guint64     session_memory_min;
  guint64     session_memory_low;

  RAllocation active_user;
  RAllocation inactive_user;

  /* Fixed session_slice configuration for drop-in */
  RAllocation session_slice;
};

G_DEFINE_TYPE (RManager, r_manager, G_TYPE_OBJECT)

static gboolean
user_has_systemd_graphical (uid_t uid)
{
  g_autofree char *cg_path = NULL;

  cg_path = g_strdup_printf ("/sys/fs/cgroup/user.slice/user-%1$i.slice/user@%1$i.service/uresourced.service", uid);
  g_debug ("Testing existance of %s: %i", cg_path, g_file_test (cg_path, G_FILE_TEST_IS_DIR));

  return g_file_test (cg_path, G_FILE_TEST_IS_DIR);
}

static void
set_unit_resources_cb (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (source_object);
  RManager *self = R_MANAGER (user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) var = NULL;

  var = g_dbus_connection_call_finish (connection, res, &error);
  if (error)
    {
      g_warning ("Failed to set resource properties on unit: %s", error->message);
    }

  self->pending_calls -= 1;
}

static void
set_unit_resources (RManager    *self,
                    const char  *unit,
                    RAllocation *allocation)
{
  GVariantBuilder builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("(sba(sv))"));
  g_autofree char *cpu_weight = NULL;
  g_autofree char *io_weight = NULL;

  g_variant_builder_add (&builder, "s", unit);
  g_variant_builder_add (&builder, "b", FALSE);
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("a(sv)"));

  g_variant_builder_add (&builder, "(sv)", "MemoryMin", g_variant_new_uint64 (allocation->memory_min));
  g_variant_builder_add (&builder, "(sv)", "MemoryLow", g_variant_new_uint64 (allocation->memory_low));
  if (allocation->cpu_weight != WEIGHT_IGNORE)
    {
      g_variant_builder_add (&builder, "(sv)", "CPUWeight", g_variant_new_uint64 (allocation->cpu_weight));
      cpu_weight = g_strdup_printf ("%d", allocation->cpu_weight);
    }
  else
    {
      cpu_weight = g_strdup ("-");
    }
  if (allocation->io_weight != WEIGHT_IGNORE)
    {
      g_variant_builder_add (&builder, "(sv)", "IOWeight", g_variant_new_uint64 (allocation->io_weight));
      io_weight = g_strdup_printf ("%d", allocation->io_weight);
    }
  else
    {
      io_weight = g_strdup ("-");
    }


  g_variant_builder_close (&builder);

  g_message ("Setting resources on %s (MemoryMin: %" G_GUINT64_FORMAT ", MemoryLow: %" G_GUINT64_FORMAT ", CPUWeight: %s, IOWeight: %s)",
             unit,
             allocation->memory_min,
             allocation->memory_low,
             cpu_weight,
             io_weight);
  g_dbus_connection_call (self->connection,
                          "org.freedesktop.systemd1",
                          "/org/freedesktop/systemd1",
                          "org.freedesktop.systemd1.Manager",
                          "SetUnitProperties",
                          g_variant_builder_end (&builder),
                          G_VARIANT_TYPE_UNIT,
                          G_DBUS_CALL_FLAGS_NO_AUTO_START,
                          1000,
                          NULL,
                          set_unit_resources_cb,
                          self);
  self->pending_calls += 1;
}

static void
set_user_slice_resources (RManager *self, gint active_users)
{
  RAllocation alloc;

  g_debug ("User slice now has %d active users", active_users);

  alloc.io_weight = WEIGHT_IGNORE;
  alloc.cpu_weight = WEIGHT_IGNORE;
  alloc.memory_min = MIN (active_users * self->active_user.memory_min, self->max_users_memory_min);
  alloc.memory_low = MIN (active_users * self->active_user.memory_low, self->max_users_memory_low);

  set_unit_resources (self, "user.slice", &alloc);
}

static void
set_user_resources (RManager *self, uid_t uid, gboolean active)
{
  g_autofree char *user_slice = NULL;
  g_autofree char *user_service = NULL;

  g_debug ("User %d is now %s", uid, active ? "active" : "inactive");

  user_slice = g_strdup_printf ("user-%i.slice", uid);
  user_service = g_strdup_printf ("user@%i.service", uid);

  if (active)
    {
      set_unit_resources (self, user_slice, &self->active_user);

      /* Only delegate memory allocation to user manager if the user appears
       * to run their graphical session using systemd.
       * Otherwise most memory should be inside the session scope. Which is
       * elsewhere in the hierarchy.
       */
      if (user_has_systemd_graphical (uid))
        set_unit_resources (self, user_service, &self->active_user);
      else
        set_unit_resources (self, user_service, &self->inactive_user);
    }
  else
    {
      set_unit_resources (self, user_slice, &self->inactive_user);
      set_unit_resources (self, user_service, &self->inactive_user);
    }
}

static void
updat_user_allocations (RManager *self, gboolean force_active)
{
  GArray *all_users;
  GArray *graphical_users;
  guint i;

  g_debug ("Updating user resource allocations");

  /* Users are "graphical" if they have at least one active graphical session. */
  r_sd_login_get_users (self->login, &all_users, &graphical_users);

  /* Sync up user slice if resources might have increased */
  if (graphical_users->len > self->graphical_users->len)
    set_user_slice_resources (self, graphical_users->len);

  /* First check which users should be (possibly) revoked resources.
   * - Revoke anyone who is not active anymore
   * - Revoke from any previously unknown user
   */
  for (i = 0; i < self->graphical_users->len; i++)
    {
      uid_t uid = g_array_index (self->graphical_users, uid_t, i);

      if (!g_array_binary_search (graphical_users, &uid, uid_cmp, NULL))
        set_user_resources (self, uid, FALSE);
    }

  for (i = 0; i < all_users->len; i++)
    {
      uid_t uid = g_array_index (all_users, uid_t, i);

      if (g_array_binary_search (graphical_users, &uid, uid_cmp, NULL))
        continue;

      if (!g_array_binary_search (self->all_users, &uid, uid_cmp, NULL))
        set_user_resources (self, uid, FALSE);
    }

  /* Now assign resources to active graphical user. */
  for (i = 0; i < graphical_users->len; i++)
    {
      uid_t uid = g_array_index (graphical_users, uid_t, i);

      if (force_active || !g_array_binary_search (self->graphical_users, &uid, uid_cmp, NULL))
        set_user_resources (self, uid, TRUE);
    }

  /* Sync up user resources if allocations have decreased */
  if (graphical_users->len < self->graphical_users->len)
    set_user_slice_resources (self, graphical_users->len);

  /* Finally, store the current state. */
  g_clear_pointer (&self->graphical_users, g_array_unref);
  self->graphical_users = g_array_copy (graphical_users);

  g_clear_pointer (&self->all_users, g_array_unref);
  self->all_users = g_array_copy (all_users);
}

static void
active_users_changed_cb (RManager *self)
{
  updat_user_allocations (self, FALSE);
}

RManager *
r_manager_new (void)
{
  return g_object_new (R_TYPE_MANAGER, NULL);
}

static void
r_manager_finalize (GObject *object)
{
  RManager *self = (RManager *)object;

  g_clear_object (&self->connection);
  g_clear_object (&self->login);
  g_clear_pointer (&self->all_users, g_array_unref);
  g_clear_pointer (&self->graphical_users, g_array_unref);

  /* Currently we rely on things to be flushed before destroyed, which really is fair ... */
  g_assert (self->pending_calls == 0);

  G_OBJECT_CLASS (r_manager_parent_class)->finalize (object);
}

static void
r_manager_class_init (RManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = r_manager_finalize;

  g_signal_new ("quit",
                R_TYPE_MANAGER, G_SIGNAL_RUN_LAST,
                0,
                NULL, NULL,
                NULL,
                G_TYPE_NONE, 0);
}

static void
r_manager_init (RManager *self)
{
  self->pending_calls = 0;
  self->graphical_users = g_array_new (FALSE, FALSE, sizeof(uid_t));
  self->all_users = g_array_new (FALSE, FALSE, sizeof(uid_t));

  /* Neutral/no-protection values. */
  self->active_user.io_weight = WEIGHT_IGNORE;
  self->active_user.cpu_weight = WEIGHT_IGNORE;

  self->inactive_user.io_weight = 100;
  self->inactive_user.cpu_weight = 100;

  self->session_slice.io_weight = WEIGHT_IGNORE;
  self->session_slice.cpu_weight = WEIGHT_IGNORE;

  /* Get the amount of available RAM (we assume this never changes after start) */
  self->available_ram = get_available_ram ();
  g_debug ("Detected %" G_GUINT64_FORMAT " bytes of RAM", self->available_ram);
}

static guint64
config_get_memory (RManager *self,
                   GKeyFile *file,
                   const char *group_name,
                   const char *key,
                   GError **error)
{
  g_autofree char* value_string = NULL;
  guint64 res;
  char *end = NULL;

  value_string = g_key_file_get_string (file, group_name, key, error);
  if (!value_string)
    return 0;

  res = g_ascii_strtoll (value_string, &end, 10);
  if (end == value_string)
    {
      g_set_error_literal (error,
                           G_KEY_FILE_ERROR,
                           G_KEY_FILE_ERROR_INVALID_VALUE,
                           "Could not parse memory key");
      return 0;
    }

  if (end && *end)
    {
      switch (*end)
        {
          case 'K':
            res = res * 1024;
            break;
          case 'M':
            res = res * 1024 * 1024;
            break;
          case 'G':
            res = res * 1024 * 1024 * 1024;
            break;
          case 'T':
            res = res * 1024 * 1024 * 1024 * 1024;
            break;
          case '%':
            res = MIN(100, res) * self->available_ram / 100;
            break;

          default:
            g_set_error (error,
                         G_KEY_FILE_ERROR,
                         G_KEY_FILE_ERROR_INVALID_VALUE,
                         "Unknown unit %c", *end);
            return 0;
        }
    }

  return res;
}

static inline void
check_clear_error (GError **error, const char *group, const char *key)
{
  if (!*error)
    return;

  if (g_error_matches (*error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND) ||
      g_error_matches (*error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
    {
      g_clear_error (error);
      return;
    }

  g_warning ("Could not parse key %s in group %s: %s", key, group, (*error)->message);
  g_clear_error (error);
}

static void
read_config (RManager *self)
{
  g_autoptr(GKeyFile) file = NULL;
  g_autoptr(GError) error = NULL;

  file = g_key_file_new ();

  if (!g_key_file_load_from_file (file, SYSCONFDIR "/uresourced.conf", G_KEY_FILE_NONE, &error))
    {
      g_warning ("Could not read configuration file: %s", error->message);
      return;
    }

  self->max_users_memory_min = config_get_memory (self, file, "Global", "MaxMemoryMin", &error);
  check_clear_error (&error, "Global", "MaxMemoryMin");
  self->max_users_memory_low = config_get_memory (self, file, "Global", "MaxMemoryLow", &error);
  check_clear_error (&error, "Global", "MaxMemoryLow");
  if (self->max_users_memory_low == 0 && self->max_users_memory_min == 0)
    g_warning ("No memory allocation set or available for user.slice; the daemon will not do anything useful!");

  /* Dynamic ActiveUser allocation */
  self->active_user.memory_min = config_get_memory (self, file, "ActiveUser", "MemoryMin", &error);
  check_clear_error (&error, "ActiveUser", "MemoryMin");

  self->active_user.memory_low = config_get_memory (self, file, "ActiveUser", "MemoryLow", &error);
  check_clear_error (&error, "ActiveUser", "MemoryLow");

  self->active_user.cpu_weight = g_key_file_get_integer (file, "ActiveUser", "CPUWeight", &error);
  check_clear_error (&error, "ActiveUser", "CPUWeight");

  self->active_user.io_weight = g_key_file_get_integer (file, "ActiveUser", "IOWeight", &error);
  check_clear_error (&error, "ActiveUser", "IOWeight");

  /* "Fixed" SessionSlice allocation inside the user */
  self->session_slice.memory_min = config_get_memory (self, file, "SessionSlice", "MemoryMin", &error);
  if (error)
    {
      check_clear_error (&error, "SessionSlice", "MemoryMin");
      self->session_slice.memory_min = self->active_user.memory_min;
    }

  self->session_slice.memory_low = config_get_memory (self, file, "SessionSlice", "MemoryLow", &error);
  if (error)
    {
      check_clear_error (&error, "SessionSlice", "MemoryLow");
      self->session_slice.memory_low = self->active_user.memory_low;
    }

  self->session_slice.cpu_weight = g_key_file_get_integer (file, "SessionSlice", "CPUWeight", &error);
  if (error)
    {
      check_clear_error (&error, "SessionSlice", "CPUWeight");
      self->session_slice.cpu_weight = self->active_user.cpu_weight;
    }

  self->session_slice.io_weight = g_key_file_get_integer (file, "SessionSlice", "IOWeight", &error);
  if (error)
    {
      check_clear_error (&error, "SessionSlice", "IOWeight");
      self->session_slice.io_weight = self->active_user.io_weight;
    }
}

static void
write_session_user_drop_ins (RManager *self)
{
  g_autofree char *session_slice = NULL;
  g_autoptr(GError) error = NULL;
  gint res;

  session_slice = g_strdup_printf (
    "[Slice]\n"
    "# Generated by uresourced to pass user memory allocations to the users session.slice\n"
    "MemoryMin=%" G_GUINT64_FORMAT "\n"
    "MemoryLow=%" G_GUINT64_FORMAT "\n"
    "%sCPUWeight=%i\n"
    "%sIOWeight=%i\n",
    self->session_slice.memory_min,
    self->session_slice.memory_low,
    self->session_slice.cpu_weight == WEIGHT_IGNORE ? "#" : "",
    self->session_slice.cpu_weight,
    self->session_slice.io_weight == WEIGHT_IGNORE ? "#" : "",
    self->session_slice.io_weight
  );

  res = g_mkdir_with_parents ("/run/systemd/user/session.slice.d", 0755);
  if (res < 0)
    {
      g_warning ("Error creating folder /run/systemd/user/session.slice.d: %m");
      return;
    }

  g_file_set_contents ("/run/systemd/user/session.slice.d/99-uresourced.conf",
                       session_slice,
                       -1,
                       &error);

  if (error)
    g_warning ("Could not write /run/systemd/user/session.slice.d/99-uresourced.conf: %s", error->message);
}

static void
decrease_pending_calls (RManager *self)
{
  self->pending_calls -= 1;
}

static void
handle_dbus_method_call (GDBusConnection       *connection G_GNUC_UNUSED,
                         const char            *sender G_GNUC_UNUSED,
                         const char            *object_path G_GNUC_UNUSED,
                         const char            *interface_name G_GNUC_UNUSED,
                         const char            *method_name G_GNUC_UNUSED,
                         GVariant              *parameters G_GNUC_UNUSED,
                         GDBusMethodInvocation *invocation,
                         gpointer               user_data)
{
  RManager *manager = R_MANAGER (user_data);

  /* Just trust that GDBus already ensures the correct method calls and types. */
  updat_user_allocations (manager, TRUE);

  g_dbus_method_invocation_return_value (invocation,
                                         NULL);
}

static const GDBusInterfaceVTable interface_vtable = {
  .method_call = handle_dbus_method_call,
  .get_property = NULL,
  .set_property = NULL,
};

static void
bus_acquired_cb (GDBusConnection *connection,
                 const gchar     *name G_GNUC_UNUSED,
                 gpointer         user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GDBusNodeInfo) introspection_data = NULL;
  RManager *manager = R_MANAGER (user_data);

  g_debug ("bus acquired");
  manager->connection = g_object_ref (connection);

  bytes = g_resources_lookup_data ("/uresourced/org.freedesktop.UResourced.xml",
                                   G_RESOURCE_LOOKUP_FLAGS_NONE,
                                   NULL);
  introspection_data = g_dbus_node_info_new_for_xml (g_bytes_get_data (bytes, NULL), NULL);
  g_assert (introspection_data);

  /* Export object on bus. */
  manager->pending_calls += 1;
  manager->dbus_obj = g_dbus_connection_register_object (connection,
                                                         "/org/freedesktop/UResourced",
                                                         introspection_data->interfaces[0],
                                                         &interface_vtable,
                                                         manager,
                                                         (GDestroyNotify) decrease_pending_calls,
                                                         &error);

  if (!manager->dbus_obj)
    {
      manager->pending_calls -= 1;
      g_warning ("Failed to register object: %s", error->message);
      g_clear_handle_id (&manager->bus_name, g_bus_unown_name);

      g_signal_emit_by_name (manager, "quit");
    }

  /* We wait until we have the name to do anything else. */
}

static void
name_acquired_cb (GDBusConnection *connection G_GNUC_UNUSED,
                  const gchar     *name G_GNUC_UNUSED,
                  gpointer         user_data)
{
  RManager *manager = R_MANAGER (user_data);
  g_debug ("name acquired");

  /* At this point it is safe to connect signals and such. */
  g_signal_connect_object (manager->login,
                           "changed",
                           G_CALLBACK (active_users_changed_cb),
                           manager,
                           G_CONNECT_SWAPPED);
  active_users_changed_cb (manager);
}

static void
name_lost_cb (GDBusConnection *connection G_GNUC_UNUSED,
              const gchar     *name G_GNUC_UNUSED,
              gpointer         user_data)
{
  RManager *manager = R_MANAGER (user_data);
  g_debug ("name lost; shutting down...");

  g_signal_emit_by_name (manager, "quit");
}

void
r_manager_start (RManager *self)
{
  self->login = r_sd_login_new ();
  g_assert (self->login);

  read_config (self);

  write_session_user_drop_ins (self);

  /* Consider the existance of the bus name a "pending call" that we need to
   * wait for to finish. */
  self->pending_calls += 1;
  self->bus_name = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                                   "org.freedesktop.UResourced",
                                   G_BUS_NAME_OWNER_FLAGS_NONE,
                                   bus_acquired_cb,
                                   name_acquired_cb,
                                   name_lost_cb,
                                   self,
                                   (GDestroyNotify) decrease_pending_calls);
}

void
r_manager_stop (RManager *self)
{
  /* Shutting down gracefully, set as if no user is active (disable protections). */
  for (guint i = 0; i < self->all_users->len; i++)
    {
      set_user_resources (self, g_array_index (self->all_users, uid_t, i), FALSE);
    }

  set_user_slice_resources (self, 0);

  g_array_set_size (self->graphical_users, 0);
  g_array_set_size (self->all_users, 0);

  g_clear_object (&self->login);

  if (self->dbus_obj)
    {
      g_dbus_connection_unregister_object (self->connection, self->dbus_obj);
      self->dbus_obj = 0;
    }
  g_clear_handle_id (&self->bus_name, g_bus_unown_name);
  g_clear_object (&self->connection);
}

void
r_manager_flush (RManager *self)
{
  g_autoptr(GMainLoop) loop = NULL;

  while (self->pending_calls > 0)
    g_main_context_iteration (NULL, TRUE);
}
