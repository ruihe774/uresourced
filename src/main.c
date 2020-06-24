/* SPDX-License-Identifier: LGPL-2.1+ */

#include <systemd/sd-daemon.h>
#include "uresourced-config.h"
#include "r-manager.h"

#include <glib.h>
#include <glib-unix.h>
#include <stdlib.h>

static gboolean
quit_mainloop (GMainLoop *loop)
{
  g_debug ("Exiting mainloop");

  g_main_loop_quit (loop);

  return G_SOURCE_CONTINUE;
}

gint
main (gint   argc,
      gchar *argv[])
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(RManager) manager = NULL;
  gboolean user_mode = FALSE;
  gboolean version = FALSE;
  GOptionEntry main_entries[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &version, "Show program version", NULL },
    { "user", 0, 0, G_OPTION_ARG_NONE, &user_mode, "Run user session part", NULL },
    { NULL }
  };

  context = g_option_context_new ("- my command line tool");
  g_option_context_add_main_entries (context, main_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      return EXIT_FAILURE;
    }

  if (version)
    {
      g_printerr ("%s\n", PACKAGE_VERSION);
      return EXIT_SUCCESS;
    }

  loop = g_main_loop_new (NULL, FALSE);

  /* Gracefully shutdown on SIGTERM and SIGINT */
  g_unix_signal_add (SIGTERM,
                     G_SOURCE_FUNC (quit_mainloop),
                     loop);
  g_unix_signal_add (SIGINT,
                     G_SOURCE_FUNC (quit_mainloop),
                     loop);

  /* In user session mode the daemon does ... nothing. It currently only exists
   * to have a defined cgroup that can be detected by the system daemon.
   * Possible further uses are:
   * - Signal system daemon to avoid race conditions (i.e. slow session startup)
   * - Reload user daemon after system daemon restart
   */

  if (!user_mode)
    {
      manager = r_manager_new ();

      r_manager_start (manager);
    }

  sd_notify (0, "READY=1");

  g_main_loop_run (loop);

  sd_notify (0, "STOPPING=1");

  if (manager)
    {
      r_manager_stop (manager);
      r_manager_flush (manager);
    }

  return EXIT_SUCCESS;
}
