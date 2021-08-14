/* SPDX-License-Identifier: LGPL-2.1+ */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define R_TYPE_GAME_MONITOR (r_game_monitor_get_type ())

G_DECLARE_FINAL_TYPE (RGameMonitor, r_game_monitor, R, GAME_MONITOR, GObject)

RGameMonitor *r_game_monitor_new (void);

void r_game_monitor_start (RGameMonitor *self,
                           RAppMonitor  *monitor);
void r_game_monitor_stop (RGameMonitor *self);

G_END_DECLS