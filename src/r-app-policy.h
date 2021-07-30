/* SPDX-License-Identifier: LGPL-2.1+ */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define R_TYPE_APP_POLICY (r_app_policy_get_type ())

G_DECLARE_FINAL_TYPE (RAppPolicy, r_app_policy, R, APP_POLICY, GObject)

RAppPolicy *r_app_policy_new (void);

void r_app_policy_start (RAppPolicy  *self,
                         RAppMonitor *monitor);
void r_app_policy_stop (RAppPolicy *self);

G_END_DECLS