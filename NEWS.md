Version 0.5.4
-------------

Changes:
 * Only run uresourced and cgroupify on cgroup v2
 * Fix inotify leak in uresourced application manager
 * Fix inotify leak in cgroupify

Version 0.5.3
-------------

Changes:
 * Proper fix for app management startup in user daemon
 * Fix use-after-free after an unknown pipewire event
 * Remove hard dependency on pipewire

Version 0.5.2
-------------

Changes:
 * Fix app management startup in user daemon

Version 0.5.1
-------------

Changes:
 * Fix incorrect error return when moving processes
 * Ignore non app.slice sub-cgroups in app manager

Version 0.5.0
-------------

Changes:
 * Add support to dynamically boost application

Version 0.4.1
-------------

Changes:
 * Fix uresourced logging
 * Fix uresourced configuration file and its parsing
 * Fix cgroupify busy looping
 * Fix cgroupify possibly not quitting correctly
 * Fix cgroupify error reporting

Version 0.4.0
-------------

Changes:
 * Add new cgroupify component. This is a separte service that is intended
   to be used together with systemd-oomd.
   It is *not* recommended to install it unless systemd-oomd is used. The
   services moves every process in a unit into its own cgroup, so that
   a systemd-oomd kill will only act on a single process.
 * Fix a warning printed too often
 * Fix possible crash at shutdown time

Version 0.3.0
-------------

Changes:

 * Use MemoryMin= to also give protection in OOM scenarios
 * Fix a race condition during login that can break memory protection
 * Change workaround for the lack of memory_recuriveprot
 * Only install memory_recuriveprot workaround if systemd is too old

Version 0.2.0
-------------

Changes:

 * Enable CPU/IO controllers for applications
 * Slightly improved logging
 * Fix a few minor memory leaks

Version 0.1.0
-------------

Initial release

Features are:

 * Assign MemoryLow, CPUWeight, IOWeight to user slice and systemd service
   (enables the corresponding cgroup controllers)
 * Delegate memory allocation to session.slice within the user
 * Detect whether user is running a graphical session using systemd
 * Modifies an existing GNOME session to take advantage of the allocation
 * Simple configuration through /etc/uresourced.conf

Please read the README for more details.
