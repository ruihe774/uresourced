/* SPDX-License-Identifier: LGPL-2.1+ */

#include "utils.h"
#include <glib/gstdio.h>

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
