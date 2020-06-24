/* SPDX-License-Identifier: LGPL-2.1+ */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct
{
  char *session;
  uid_t uid;
} RSdLoginSession;

#define R_TYPE_SD_LOGIN (r_sd_login_get_type ())

G_DECLARE_FINAL_TYPE (RSdLogin, r_sd_login, R, SD_LOGIN, GObject)

RSdLogin *r_sd_login_new (void);

void r_sd_login_get_users (RSdLogin *login,
                           GArray **all_users,
                           GArray **graphical_users);

G_END_DECLS
