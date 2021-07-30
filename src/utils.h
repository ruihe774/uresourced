/* SPDX-License-Identifier: LGPL-2.1+ */

#include <glib-2.0/glib.h>
#include <sys/types.h>

int uid_cmp (gconstpointer a, gconstpointer b);
guint64 get_available_ram ();
gchar *get_unit_cgroup_path_from_pid (pid_t pid);
gchar *get_unit_name_from_path (const gchar *path);