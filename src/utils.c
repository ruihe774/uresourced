/* SPDX-License-Identifier: LGPL-2.1+ */

#include "utils.h"
#include <glib/gstdio.h>
#include <systemd/sd-login.h>

int
uid_cmp (gconstpointer a, gconstpointer b)
{
  return *(uid_t*)a - *(uid_t*)b;
}

guint64
get_available_ram ()
{
  g_autoptr(GError) error = NULL;
  g_autofree char *contents = NULL;
  char *mem_total = NULL;

  if (!g_file_get_contents ("/proc/meminfo", &contents, NULL, &error))
    {
      g_warning ("Could not read /proc/meminfo: %s", error->message);
      return 0;
    }

  /* It should be in the first line, but let us not assume that. */
  mem_total = strstr(contents, "MemTotal:");
  if (!mem_total)
    {
      g_warning ("Could not find MemTotal in /proc/meminfo");
      return 0;
    }

  /* The value is given in kB, return bytes. */
  return g_ascii_strtoull (mem_total + 9, NULL, 10) * 1024;
}

/**
 * get_unit_cgroup_path_from_pid:
 * @pid: Application PID
 *
 * This function returns the systemd (user) unit cgroup path for the
 * given PID. Note that the application might manage a subhierarchy of
 * cgroups if Delegate= is set. As of now, such a sub-hierarchy is
 * ignored and will not be returned.
 *
 * Returns: The full path to cgroupfs for the systemd unit, or %NULL
 */
gchar *
get_unit_cgroup_path_from_pid (pid_t pid)
{
  g_autoptr (GError) error = NULL;
  g_autofree gchar *contents = NULL;
  g_autofree gchar *complete_path = NULL;
  g_autofree gchar *unit_name = NULL;
  g_autofree gchar *cgroup_path = NULL;

  if (sd_pid_get_cgroup (pid, &contents) < 0)
    {
      g_debug ("Could not get cgroup path for pid: %d", pid);
      return NULL;
    }
  g_strstrip (contents);

  complete_path = g_strdup_printf ("%s%s", "/sys/fs/cgroup", contents);

  if (sd_pid_get_user_unit (pid, &unit_name) < 0)
    {
      g_debug ("Could not get user unit name for pid: %d", pid);
      return NULL;
    }
  g_strstrip (unit_name);

  cgroup_path
      = g_strndup (complete_path, strstr (complete_path, unit_name)
                                      - complete_path + strlen (unit_name));

  return g_steal_pointer (&cgroup_path);
}

gchar *
get_unit_name_from_path (const gchar *path)
{
  g_autofree gchar *complete_path = NULL;
  g_autofree gchar *app_unit_name = NULL;
  gboolean after_user_service = FALSE;
  char *token;

  complete_path = g_strdup (path);

  token = strtok (complete_path, "/");
  while (token)
    {
      if (g_str_has_prefix (token, "user@") &&
          g_str_has_suffix (token, ".service"))
        {
          after_user_service = TRUE;
          token = strtok (NULL, "/");
          continue;
        }

      if (after_user_service && !g_str_has_suffix (token, ".slice"))
        {
          if (token[0] == '_')
            app_unit_name = g_strdup (token + 1);
          else
            app_unit_name = g_strdup (token);
          break;
        }
      token = strtok (NULL, "/");
    }

  return g_steal_pointer (&app_unit_name);
}