/* SPDX-License-Identifier: LGPL-2.1+ */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define R_TYPE_MANAGER (r_manager_get_type())

G_DECLARE_FINAL_TYPE (RManager, r_manager, R, MANAGER, GObject)

RManager *r_manager_new (void);

void r_manager_start (RManager *manager);
void r_manager_stop (RManager *manager);
void r_manager_flush (RManager *manager);

G_END_DECLS
