/*
 * ProFTPD - mod_statsd
 * Copyright (c) 2017 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 */

#include "mod_statsd.h"
#include "statsd.h"

#define STATSD_DEFAULT_PORT	8125

extern xaset_t *server_list;

module statsd_module;

static int statsd_engine = FALSE;
static struct statsd *statsd = NULL;

static const char *trace_channel = "statsd";

/* Configuration handlers
 */

/* usage: StatsdEngine on|off */
MODRET set_statsdengine(cmd_rec *cmd) {
  int engine = -1;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  engine = get_boolean(cmd, 1);
  if (engine == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = engine;

  return PR_HANDLED(cmd);
}

/* usage: StatsdServer host[:port] */
MODRET set_statsdserver(cmd_rec *cmd) {
  config_rec *c;
  char *server, *ptr;
  size_t server_len;
  int port = STATSD_DEFAULT_PORT;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  server = pstrdup(cmd->tmp_pool, cmd->argv[1]);
  server_len = strlen(server);

  ptr = strrchr(server, ':');
  if (ptr != NULL) {
    /* We also need to check for IPv6 addresses, e.g. "[::1]" or "[::1]:8125",
     * before assuming that the text following our discovered ':' is indeed
     * a port number.
     */

    if (*server == '[') {
      if (*(ptr-1) == ']') {
        /* We have an IPv6 address with an explicit port number. */
        server = pstrndup(cmd->tmp_pool, server + 1, (ptr - 1) - (server + 1));
        *ptr = '\0';
        port = atoi(ptr + 1);

      } else if (server[server_len-1] == ']') {
        /* We have an IPv6 address without an explicit port number. */
        server = pstrndup(cmd->tmp_pool, server + 1, server_len - 2);
        port = STATSD_DEFAULT_PORT;
      }

    } else {
      *ptr = '\0';
      port = atoi(ptr + 1);
    }
  }

  c = add_config_param(cmd->argv[0], 2, NULL, NULL);
  c->argv[0] = pstrdup(c->pool, server);
  c->argv[1] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[1]) = port;

  return PR_HANDLED(cmd);
}

/* Command handlers
 */

MODRET statsd_log_any(cmd_rec *cmd) {
  config_rec *c;

  c = find_config(TOPLEVEL_CONF, CONF_PARAM, "TarEngine", FALSE);
  if (c) {
    int enable_tar = *((int *) c->argv[0]);

    if (enable_tar) {
      tar_engine = TRUE;
    }
  }

  if (tar_engine) {
    pr_event_register(&tar_module, "core.exit", tar_exit_ev, NULL);

    c = find_config(TOPLEVEL_CONF, CONF_PARAM, "TarOptions", FALSE);
    while (c != NULL) {
      unsigned long opts;

      pr_signals_handle();

      opts = *((unsigned long *) c->argv[0]);
      tar_opts |= opts;

      c = find_config_next(c, c->next, CONF_PARAM, "TarOptions", FALSE);
    }

    c = find_config(TOPLEVEL_CONF, CONF_PARAM, "TarTempPath", FALSE);
    if (c) {
      tar_tmp_path = dir_canonical_path(session.pool, c->argv[0]);

      if (session.chroot_path) {
        size_t chroot_len;

        chroot_len = strlen(session.chroot_path);
        if (strncmp(tar_tmp_path, session.chroot_path, chroot_len) == 0) {
          tar_tmp_path += chroot_len;
        }
      }

      (void) pr_log_writefile(tar_logfd, MOD_TAR_VERSION,
        "using '%s' as the staging directory for temporary .tar files",
        tar_tmp_path);
    }
  }

  return PR_DECLINED(cmd);
}

/* Event handlers
 */

static void statsd_exit_ev(const void *event_data, void *user_data) {
  if (statsd != NULL) {
    statsd_close(statsd);
    statsd = NULL;
  }
}

#if defined(PR_SHARED_MODULE)
static void statsd_mod_unload_ev(const void *event_data, void *user_data) {
  if (strcmp("mod_statsd.c", (const char *) event_data) == 0) {
    pr_event_unregister(&statsd_module, NULL, NULL);
  }
}
#endif /* PR_SHARED_MODULE */

static void statsd_postparse_ev(const void *event_data, void *user_data) {
  server_rec *s;

  for (s = (server_rec *) server_list->xas_list; s; s = s->next) {
    config_rec *c;
    int engine;

    c = find_config(s->conf, CONF_PARAM, "StatsdEngine", FALSE);
    if (c == NULL) {
      continue;
    }

    engine = *((int *) c->argv[0]);
    if (engine == FALSE) {
      continue;
    }

    c = find_config(s->conf, CONF_PARAM, "StatsdServer", FALSE);
    if (c == NULL) {
      pr_log_pri(PR_LOG_NOTICE, MOD_STATSD_VERSION
        ": Server %s: missing required StatsdServer directive", s->ServerName);
      pr_session_disconnect(&tls_module, PR_SESS_DISCONNECT_BAD_CONFIG, NULL);
    }
  }
}

static void statsd_shutdown_ev(const void *event_data, void *user_data) {
  if (statsd != NULL) {
    statsd_close(statsd);
    statsd = NULL;
  }
}

/* Initialization functions
 */

static int statsd_sess_init(void) {
  config_rec *c;

  c = find_config(main_server->conf, CONF_PARAM, "StatsdEngine", FALSE);
  if (c != NULL) {
    statsd_engine = *((int *) c->argv[0]);
  }

  if (statsd_engine == FALSE) {
    return 0;
  }

  c = find_config(main_server->conf, CONF_PARAM, "StatsdServer", FALSE);
  if (c != NULL) {
  }

  pr_event_register(&statsd_module, "core.exit", statsd_exit_ev, NULL);
  return 0;
}

static int statsd_init(void) {
#if defined(PR_SHARED_MODULE)
  pr_event_register(&statsd_module, "core.module-unload", statsd_mod_unload_ev,
    NULL);
#endif
  pr_event_register(&statsd_module, "core.postparse", statsd_postparse_ev,
    NULL);
  pr_event_register(&statsd_module, "core.shutdown", statsd_shutdown_ev,
    NULL);

  return 0;
}

/* Module API tables
 */

static conftable statsd_conftab[] = {
  { "StatsdEngine",	set_statsdengine,	NULL },
  { "StatsdServer",	set_statsserver,	NULL },
  { NULL }
};

static cmdtable statsd_cmdtab[] = {
  { PRE_CMD,		C_ANY,	G_NONE,	statsd_pre_any,	FALSE,	FALSE },
  { LOG_CMD,		C_ANY,	G_NONE,	statsd_log_any,	FALSE,	FALSE },
  { LOG_CMD_ERR,	C_ANY,	G_NONE,	statsd_log_any,	FALSE,	FALSE },
};

module statsd_module = {
  NULL, NULL,

  /* Module API version 2.0 */
  0x20,

  /* Module name */
  "statsd",

  /* Module config handler table */
  statsd_conftab,

  /* Module command handler table */
  statsd_cmdtab,

  /* Module auth handler table */
  NULL,

  /* Module init function */
  statsd_init,

  /* Session init function */
  statsd_sess_init,

  /* Module version */
  MOD_STATSD_VERSION
};
