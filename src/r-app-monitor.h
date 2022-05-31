/* SPDX-License-Identifier: LGPL-2.1+ */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define R_TYPE_APP_MONITOR (r_app_monitor_get_type ())

typedef enum {
  BOOST_NONE = 0,
  BOOST_AUDIO = (1 << 0),
  BOOST_GAME = (1 << 1)
} AppBoostFlags;

typedef struct
{
  gchar        *name;
  gchar        *path;
  guint64       cpu_weight;
  guint64       io_weight;
  gint64        timestamp;
  AppBoostFlags boosted;
} RAppInfo;

G_DECLARE_FINAL_TYPE (RAppMonitor, r_app_monitor, R, APP_MONITOR, GObject)

RAppMonitor *r_app_monitor_new (void);
RAppMonitor *r_app_monitor_get_default (void);

void r_app_monitor_start (RAppMonitor *self);
void r_app_monitor_stop (RAppMonitor *self);

RAppInfo *r_app_monitor_get_app_info_from_path (RAppMonitor *app_monitor,
                                                gchar       *app_path);
void r_app_monitor_reset_all_apps (RAppMonitor *self);

void r_app_monitor_app_info_changed (RAppMonitor *self,
                                     RAppInfo    *info);

G_END_DECLS