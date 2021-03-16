/* SPDX-License-Identifier: LGPL-2.1+ */

#include "r-sd-login.h"
#include "utils.h"

#include <glib-unix.h>
#include <systemd/sd-login.h>

struct _RSdLogin
{
  GObject           parent_instance;

  sd_login_monitor *mon;
  guint             mon_source;
  guint             mon_delay;


  /* We only care about which users are active (on any seat). */
  GArray *graphical_users;
  GArray *all_users;
};

G_DEFINE_TYPE (RSdLogin, r_sd_login, G_TYPE_OBJECT)

RSdLogin *
r_sd_login_new (void)
{
  return g_object_new (R_TYPE_SD_LOGIN, NULL);
}

static void
r_sd_login_finalize (GObject *object)
{
  RSdLogin *self = (RSdLogin *) object;

  g_clear_handle_id (&self->mon_delay, g_source_remove);
  g_clear_handle_id (&self->mon_source, g_source_remove);
  g_clear_pointer (&self->mon, sd_login_monitor_unref);
  g_clear_pointer (&self->graphical_users, g_array_unref);
  g_clear_pointer (&self->all_users, g_array_unref);

  G_OBJECT_CLASS (r_sd_login_parent_class)->finalize (object);
}

static void
freevp (char ***v)
{
  char **f;

  if (!*v)
    return;

  for (f = *v; *f; f++)
    {
      free (*f);
    }

  free (*v);
  *v = NULL;
}

static gboolean
logind_quiet (gpointer user_data)
{
  RSdLogin *self = R_SD_LOGIN (user_data);
  __attribute__((__cleanup__(freevp))) char **seats = NULL;
  char **seat = NULL;
  uid_t *all_uids = NULL;
  int r = 0;

  self->mon_delay = 0;

  /* Update our user active/inactive state. */
  g_array_set_size (self->graphical_users, 0);
  g_array_set_size (self->all_users, 0);


  r = sd_get_seats (&seats);
  if (r < 0)
    {
      g_critical ("Failed to get seats: %m");
      goto err;
    }

  /* NOTE: We need to look at all sessions, going through the seat should be
   *       a reasonable way of doing so.
   */
  for (seat = seats; seat && *seat; seat++)
    {
      __attribute__((__cleanup__(freevp))) char **sessions = NULL;
      char **session = NULL;

      /* We only take graphical seats, and just assume the user will
       * have a graphical session that should get protection.
       */
      r = sd_seat_can_graphical (*seat);
      if (r < 0)
        {
          g_warning ("Failed to get whether seat is graphical, ignoring the seat: %m");
          continue;
        }
      if (r == 0)
        continue;

      r = sd_seat_get_sessions (*seat, &sessions, NULL, NULL);
      if (r < 0)
        {
          g_warning ("Failed to get sessions for seat, ignoring the seat: %m");
          continue;
        }

      for (session = sessions; session && *session; session++)
        {
          uid_t uid;
          gboolean is_active;

          r = sd_session_get_uid (*session, &uid);
          if (r < 0)
            {
              /* This can happen after sessions disappear (i.e. not just a transient issue). */
              g_debug ("Failed to get user for session %s, ignoring the session: %m", *session);
              continue;
            }

          r = sd_session_is_active (*session);
          if (r < 0)
            {
              g_warning ("Failed to get whether session is active, ignoring the session: %m");
              continue;
            }
          is_active = !!r;

          if (!is_active)
            continue;

          /* If we find the user, then they are actually active. */
          if (!g_array_binary_search (self->graphical_users, &uid, uid_cmp, NULL))
            {
              g_array_append_val (self->graphical_users, uid);
              g_array_sort (self->graphical_users, uid_cmp);
            }
        }
    }

  r = sd_get_uids (&all_uids);
  if (r < 0)
    {
      g_critical ("Failed to get list of all UIDs: %m");
      goto err;
    }

  g_array_append_vals (self->all_users, all_uids, r);
  free(all_uids);

  g_array_sort (self->all_users, uid_cmp);
  g_array_sort (self->graphical_users, uid_cmp);

  g_signal_emit_by_name (self, "changed");

  return G_SOURCE_REMOVE;

err:

  g_array_set_size (self->graphical_users, 0);
  g_array_set_size (self->all_users, 0);

  g_signal_emit_by_name (self, "changed");

  return G_SOURCE_REMOVE;
}

static gboolean
logind_changed (G_GNUC_UNUSED gint         fd,
                G_GNUC_UNUSED GIOCondition condition,
                gpointer                   user_data)
{
  RSdLogin *self = R_SD_LOGIN (user_data);

  sd_login_monitor_flush (self->mon);

  /* Consider everything quiet/settled after 100ms */
  g_clear_handle_id (&self->mon_delay, g_source_remove);
  self->mon_delay = g_timeout_add (100, logind_quiet, self);

  return G_SOURCE_CONTINUE;
}

static void
r_sd_login_class_init (RSdLoginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = r_sd_login_finalize;

  g_signal_new ("changed",
                R_TYPE_SD_LOGIN, G_SIGNAL_RUN_LAST,
                0,
                NULL, NULL,
                NULL,
                G_TYPE_NONE, 0);
}

void
r_sd_login_get_users (RSdLogin *login,
                      GArray **all_users,
                      GArray **graphical_users)
{
  g_return_if_fail (graphical_users || all_users);

  if (all_users)
    *all_users = login->all_users;
  if (graphical_users)
    *graphical_users = login->graphical_users;
}

static void
r_sd_login_init (RSdLogin *self)
{
  if (sd_login_monitor_new (NULL, &self->mon) < 0)
    g_error ("Could not create login monitor!");

  self->graphical_users = g_array_new (FALSE, FALSE, sizeof(uid_t));
  self->all_users = g_array_new (FALSE, FALSE, sizeof(uid_t));

  /* Note: We ignore the timeout here, this *might* break and if it does,
   *       it really is on us to fix it.
   */
  self->mon_source = g_unix_fd_add (sd_login_monitor_get_fd (self->mon),
                                    sd_login_monitor_get_events (self->mon),
                                    logind_changed,
                                    self);

  /* Read current state right away. */
  logind_quiet (self);
}
