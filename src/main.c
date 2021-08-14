/* SPDX-License-Identifier: LGPL-2.1+ */

#include <systemd/sd-daemon.h>
#include "uresourced-config.h"
#include "r-manager.h"

#ifdef HAVE_APP_MANAGEMENT
#include "r-app-monitor.h"
#include "r-app-policy.h"
#include "r-pw-monitor.h"
#include "r-game-monitor.h"
#endif

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <stdlib.h>

static gboolean
quit_mainloop (GMainLoop *loop)
{
  g_debug ("Exiting mainloop");

  if (g_main_loop_is_running (loop))
    g_main_loop_quit (loop);

  return G_SOURCE_CONTINUE;
}

static gboolean
idle_system_daemon_update (gpointer user_data G_GNUC_UNUSED)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(GVariant) res = NULL;

  /* The daemon is idle anyway, no need to bother with async routines. */
  bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!bus)
    {
      g_warning ("Could not get system bus: %s", error->message);
      return G_SOURCE_REMOVE;
    }

  res = g_dbus_connection_call_sync (bus,
                                     "org.freedesktop.UResourced",
                                     "/org/freedesktop/UResourced",
                                     "org.freedesktop.UResourced",
                                     "Update",
                                     NULL,
                                     NULL,
                                     G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                     1000,
                                     NULL,
                                     &error);

  if (!res)
    {
      g_warning ("Could not call system daemon update routine: %s", error->message);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_REMOVE;
}

gint
main (gint   argc,
      gchar *argv[])
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(RManager) manager = NULL;
#ifdef HAVE_APP_MANAGEMENT
  g_autoptr(RAppPolicy) app_policy = NULL;
  g_autoptr(RPwMonitor) pw_monitor = NULL;
  g_autoptr(RGameMonitor) game_monitor = NULL;
  RAppMonitor *app_monitor = NULL;
#endif
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

  /* In user session mode the daemon currently signals to the system
   * daemon that it is running. This resolves a race condition where the
   * cgroup of the user daemon was not yet active when the user became active
   * after login.
   */

  if (!user_mode)
    {
      manager = r_manager_new ();
      g_signal_connect_swapped (manager, "quit", G_CALLBACK (quit_mainloop), loop);

      r_manager_start (manager);
    }
  else
    {
      /* Register an idle handler to poke the system daemon. */
      g_idle_add (idle_system_daemon_update, NULL);

#ifdef HAVE_APP_MANAGEMENT
      app_monitor = r_app_monitor_get_default ();
      r_app_monitor_start (app_monitor);

      app_policy = r_app_policy_new ();
      r_app_policy_start (app_policy, app_monitor);

      pw_monitor = r_pw_monitor_new ();
      r_pw_monitor_start (pw_monitor, app_monitor);

      game_monitor = r_game_monitor_new ();
      r_game_monitor_start (game_monitor, app_monitor);
#endif
    }

  sd_notify (0, "READY=1");

  g_main_loop_run (loop);

  sd_notify (0, "STOPPING=1");

  if (!user_mode)
    {
      r_manager_stop (manager);
      r_manager_flush (manager);
    }
#ifdef HAVE_APP_MANAGEMENT
  else
    {
      r_game_monitor_stop (game_monitor);
      r_pw_monitor_stop (pw_monitor);
      r_app_policy_stop (app_policy);
      r_app_monitor_stop (app_monitor);

      g_object_unref (app_monitor);
    }
#endif

  return EXIT_SUCCESS;
}
