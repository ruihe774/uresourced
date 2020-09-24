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
