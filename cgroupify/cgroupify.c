/* gcc -Wall -Werror -lsystemd -o cgroupify cgroupify.c */

#define _GNU_SOURCE 1

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <systemd/sd-event.h>
#include <systemd/sd-bus.h>

/* 1 seconds with 0.5 second accuracy */
#define UPDATE_DELAY_USEC 1000000
#define UPDATE_DELAY_ACCURACY_USEC 500000

struct globals
{
  sd_event *event;
  char     *cgroup_path;
  int       cgroup_fd;
};

char *
resolve_cgroup (char *unit)
{
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus *bus = NULL;
  sd_bus_message *get_unit_reply = NULL;
  sd_bus_message *get_property_reply = NULL;
  char *path = NULL;
  char *cgroup = NULL;
  char *res = NULL;
  int is_scope;

  if (strlen (unit) < 6)
    {
      fprintf (stderr, "Unit name %s is too short to be valid.\n", unit);
      return NULL;
    }
  is_scope = strcmp (".scope", unit + strlen (unit) - 6) == 0;

  if (sd_bus_open_user (&bus) < 0)
    {
      fprintf (stderr, "Error opening bus connection: %d\n", errno);
      return NULL;
    }

  if (sd_bus_call_method (bus,
                          "org.freedesktop.systemd1",
                          "/org/freedesktop/systemd1",
                          "org.freedesktop.systemd1.Manager",
                          "GetUnit",
                          &error,
                          &get_unit_reply,
                          "s", unit) < 0)
    {
      fprintf (stderr, "Error getting unit object path for %s: %s\n",
               unit, error.message);
      goto out;
    }

  if (sd_bus_message_read_basic (get_unit_reply, 'o', &path) < 0)
    {
      fprintf (stderr, "Error retrieving unit object path from systemd reply\n");
      goto out;
    }


  if (sd_bus_call_method (bus,
                          "org.freedesktop.systemd1",
                          path,
                          "org.freedesktop.DBus.Properties",
                          "Get",
                          &error,
                          &get_property_reply,
                          "ss",
                          is_scope ? "org.freedesktop.systemd1.Scope" : "org.freedesktop.systemd1.Service",
                          "ControlGroup") < 0)
    {
      fprintf (stderr, "Error getting ControlGroup property for %s: %s\n",
               path, error.message);
      goto out;
    }

  if (sd_bus_message_enter_container (get_property_reply, 'v', "s") < 0)
    {
      fprintf (stderr, "Unexpected return type from GetProperty\n");
      goto out;
    }

  if (sd_bus_message_read_basic (get_property_reply, 's', &cgroup) < 0)
    {
      fprintf (stderr, "Error retrieving unit control group from systemd reply\n");
      goto out;
    }

  res = strdup (cgroup);

out:
  sd_bus_message_unref (get_unit_reply);
  sd_bus_message_unref (get_property_reply);
  sd_bus_error_free (&error);
  sd_bus_unref (bus);
  return res;
}

int
open_cgroup (char *unit, char **out_path)
{
  const char *prefix = "/sys/fs/cgroup/";
  char *cgroup;
  char *cgroup_path;
  int cgroup_fd;

  cgroup = resolve_cgroup (unit);
  if (!cgroup)
    {
      fprintf (stderr, "Could not resolve cgroup for unit %s\n", unit);
      return -1;
    }

  cgroup_path = malloc (strlen (cgroup) + strlen (prefix) + 1);
  strcpy (cgroup_path, prefix);
  strcat (cgroup_path, cgroup);

  free (cgroup);

  /* Open the cgroup directory, after that we just keep using that */
  cgroup_fd = open (cgroup_path, O_DIRECTORY);
  if (cgroup_fd < 0)
    {
      fprintf (stderr, "Failed to open cgroup directory %s\n", cgroup_path);
      return cgroup_fd;
    }

  if (out_path)
    *out_path = cgroup_path;
  else
    free (cgroup_path);

  return cgroup_fd;
}

int
open_procs (int cgroup_fd, char *subgroup, int mode)
{
  char *procs_file;

  if (subgroup)
    {
      const char *cgroup_procs = "/cgroup.procs";

      procs_file = alloca (strlen (subgroup) + strlen (cgroup_procs) + 1);
      strcpy (procs_file, subgroup);
      strcat (procs_file, cgroup_procs);
    }
  else
    {
      procs_file = "cgroup.procs";
    }

  return openat (cgroup_fd, procs_file, mode);
}

int
subgroup_inotify_cb (sd_event_source *s, const struct inotify_event *event, void *userdata)
{
  int r;
  char *full_path = userdata;

  (void) event;

  /* cgroup is probably empty, try to reap it (if this fails with EBUSY,
   * then it wasn't empty and we just continue watching).
   */
  r = rmdir (full_path);
  if (r < 0 && errno == EBUSY)
    return 0;

  if (r < 0 && errno != ENOENT)
    fprintf (stderr, "Could not unlink %s, ignoring from now on: %m\n", full_path);

  /* We are done, disable event source and free resources. */
  sd_event_source_disable_unref (s);
  free (full_path);

  return 0;
}

int
move_to_subgroup (struct globals *globals, char *pid)
{
  sd_event_source *inotify_source = NULL;
  char *full_path;
  char *full_events_path;
  int r, res;
  int fd;

  /* The directory should not yet exist */
  r = mkdirat (globals->cgroup_fd, pid, 0777);
  if (r < 0)
    return -errno;

  /* Add an inotify watch for the directory. */
  full_path = malloc (strlen (globals->cgroup_path) + strlen (pid) + 2);
  strcpy (full_path, globals->cgroup_path);
  strcat (full_path, "/");
  strcat (full_path, pid);

  full_events_path = malloc (strlen (globals->cgroup_path) + strlen (pid) + strlen ("/cgroup.events") + 2);
  strcpy (full_events_path, globals->cgroup_path);
  strcat (full_events_path, "/");
  strcat (full_events_path, pid);
  strcat (full_events_path, "/cgroup.events");

  /* This creates a non-floating event source. */
  r = sd_event_add_inotify (globals->event, &inotify_source,
                            full_events_path, IN_MODIFY,
                            subgroup_inotify_cb, full_path);
  free (full_events_path);
  if (r < 0)
    {
      fprintf (stderr, "Could not add inotify watch!\n");
      free (full_path);
      goto out;
    }

  /* And, get ready to move the process */
  fd = open_procs (globals->cgroup_fd, pid, O_WRONLY);
  if (fd < 0)
    {
      r = -errno;
      goto out;
    }

  r = write (fd, pid, strlen (pid));
  /* ESRCH is expected if the PID does not exist anymore. */
  if (r < 0 && errno == ESRCH)
    r = 0;
  else if (r < 0)
    r = -errno;
  else
    r = 0;
  close (fd);

out:
  /* The cgroup should be filled at this point. However, it will not be
   * filled if the PID is gone or if it was/is a zombie.
   * So just try to delete the cgroup again. If that succeeds (or the cgroup is
   * already gone for some reason), then the cgroup contains no processes. If
   * that happens then we are not going to get an inotify event, so remove it
   * explicitly.
   */
  res = unlinkat (globals->cgroup_fd, pid, AT_REMOVEDIR);
  if (res == 0 || (res < 0 && errno == ENOENT))
    sd_event_source_disable_unref (inotify_source);

  return r;
}

int
move_pids_to_subgroups (struct globals *globals, char *subgroup)
{
  int result = -1;
  int found;
  ssize_t pids_len;

  /* If subgroup is NULL we are moving away from the root node. */

  do
    {
      int fd;
      char pids[1024] = { 0 };
      char *pid;
      char *sep;

      found = 0;

      /* XXX: Can we lseek(0) instead? */
      fd = open_procs (globals->cgroup_fd, subgroup, O_RDONLY);
      /* Expected to happen if the cgroup disappears. */
      if (fd < 0)
        return fd;

      pids_len = read (fd, pids, sizeof (pids) - 1);
      close (fd);
      if (pids_len < 0)
        {
          fprintf (stderr, "Error reading cgroup.procs\n");
          exit (1);
        }

      /* We assume that the file has a trailing \n */
      pid = pids;
      for (sep = strchr (pid, '\n'); sep; pid = sep + 1, sep = strchr (pid, '\n'))
        {
          *sep = '\0';

          if (strlen (pid) == 0)
            break;

          if (subgroup && strcmp (pid, subgroup) == 0)
            continue;

          found += 1;

          result = move_to_subgroup (globals, pid);
          if (result < 0)
            {
              fprintf (stderr, "Error moving pid %s into new cgroup (%d)\n", pid, -result);
              goto out;
            }
        }
    }
  while (found);

  result = 0;
out:
  return result;
}

int
move_pids_from_subgroups (sd_event_source *s, uint64_t usec, void *userdata)
{
  struct globals *globals = userdata;
  uint64_t next;
  int i, n;
  struct dirent **namelist = NULL;

  (void) usec;

  n = scandirat (globals->cgroup_fd, ".", &namelist, NULL, NULL);
  if (n <= 0)
    {
      sd_event_exit (globals->event, 0);
      return 0;
    }
  for (i = 0; i < n; i++)
    {
      if (namelist[i]->d_type != DT_DIR)
        continue;

      /* Skip ".", ".." (and anything else starting with a .) */
      if (namelist[i]->d_name[0] == '.')
        continue;

      move_pids_to_subgroups (globals, namelist[i]->d_name);
    }
  for (i = 0; i < n; i++)
    free (namelist[i]);
  free (namelist);


  /* Reschedule again relative to the current time (could also use usec) */
  sd_event_now (sd_event_source_get_event (s),
                CLOCK_MONOTONIC,
                &next);
  next += UPDATE_DELAY_USEC;
  sd_event_source_set_time (s, usec + UPDATE_DELAY_USEC);

  return 0;
}

int
main (int argc, char **argv)
{
  struct globals globals = { 0 };
  sd_event_source *move_timer = NULL;
  char *unit;
  uint64_t next;
  int fd, r;

  if (argc != 2)
    {
      fprintf (stderr, "Exactly one argument with a unit name is required\n");
      exit (1);
    }

  if (sd_event_new (&globals.event) < 0)
    exit (1);

  unit = argv[1];

  globals.cgroup_fd = open_cgroup (unit, &globals.cgroup_path);
  /* Funtion already warned, so just exit. */
  if (globals.cgroup_fd < 0)
    exit (1);

  /* Move everything away from the main cgroup */
  if (move_pids_to_subgroups (&globals, NULL) < 0)
    exit (1);

  /* We are doing this for systemd-oomd, so we are interested in the
   * memory controller to be enabled for the child groups.
   *
   * We can only do this after having created child cgroups.
   */
  fd = openat (globals.cgroup_fd, "cgroup.subtree_control", O_WRONLY);
  if (fd < 0)
    {
      fprintf (stderr, "Failed to open cgroup.subtree_control for %s\n", globals.cgroup_path);
      exit (1);
    }
  if (write (fd, "+memory", 7) < 0)
    {
      fprintf (stderr, "Failed to enable memory subtree controller for %s\n", globals.cgroup_path);
      exit (1);
    }
  close (fd);

  /* Now periodically check the cgroups and move everything out. */
  sd_event_now (globals.event,
                CLOCK_MONOTONIC,
                &next);
  next += UPDATE_DELAY_USEC;
  r = sd_event_add_time (globals.event,
                         &move_timer,
                         CLOCK_MONOTONIC,
                         next,
                         UPDATE_DELAY_ACCURACY_USEC,
                         move_pids_from_subgroups,
                         &globals);
  if (r < 0)
    exit (1);
  r = sd_event_source_set_enabled (move_timer, SD_EVENT_ON);
  if (r < 0)
    exit (1);

  sd_event_loop (globals.event);

  sd_event_unrefp (&globals.event);
  close (globals.cgroup_fd);
}
