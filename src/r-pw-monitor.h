/* SPDX-License-Identifier: LGPL-2.1+ */

#pragma once

#include <glib-object.h>

#include <pipewire/pipewire.h>

G_BEGIN_DECLS

#define R_TYPE_PW_MONITOR (r_pw_monitor_get_type ())

G_DECLARE_FINAL_TYPE (RPwMonitor, r_pw_monitor, R, PW_MONITOR, GObject)

RPwMonitor *r_pw_monitor_new (void);

void r_pw_monitor_start (RPwMonitor  *self,
                         RAppMonitor *monitor);
void r_pw_monitor_stop (RPwMonitor *self);

G_END_DECLS