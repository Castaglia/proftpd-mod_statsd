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
 *
 * -----DO NOT EDIT BELOW THIS LINE-----
 * $Archive: mod_statsd.a $
 */

#include "mod_statsd.h"
#include "statsd.h"
#include "metric.h"

extern xaset_t *server_list;

module statsd_module;

#define STATSD_DEFAULT_ENGINE			FALSE
#define STATSD_DEFAULT_SAMPLING			1.0F

static int statsd_engine = STATSD_DEFAULT_ENGINE;
static const char *statsd_exclude_filter = NULL;
#ifdef PR_USE_REGEX
static pr_regex_t *statsd_exclude_pre = NULL;
#endif /* PR_USE_REGEX */
static float statsd_sampling = STATSD_DEFAULT_SAMPLING;
static struct statsd *statsd = NULL;

static int statsd_sess_init(void);

static const char *trace_channel = "statsd";

static char *get_cmd_metric(pool *p, const char *cmd) {
  const char *resp_code = NULL;
  char *metric;

  if (strcasecmp(cmd, C_QUIT) != 0) {
    int res;

    res = pr_response_get_last(p, &resp_code, NULL);
    if (res < 0 ||
        resp_code == NULL) {
      resp_code = "-";
    }

  } else {
    resp_code = R_221;
  }

  metric = pstrcat(p, "command.", cmd, ".", resp_code, NULL);
  return metric;
}

static char *get_conn_metric(pool *p, const char *name) {
  char *metric;

  if (name == NULL) {
    metric = pstrdup(p, "connection");

  } else {
    metric = pstrcat(p, name, ".connection", NULL);
  }

  return metric;
}

static char *get_timeout_metric(pool *p, const char *name) {
  char *metric;

  metric = pstrcat(p, "timeout.", name, NULL);
  return metric;
}

static char *get_tls_metric(pool *p, const char *name) {
  char *metric;

  metric = pstrcat(p, "tls.", name, NULL);
  return metric;
}

static int should_exclude(cmd_rec *cmd) {
  int exclude = FALSE;

#ifdef PR_USE_REGEX
  if (pr_regexp_exec(statsd_exclude_pre, (char *) cmd->argv[0], 0, NULL, 0, 0,
      0) == 0) {
    exclude = TRUE;
  }
#endif /* PR_USE_REGEX */

  return exclude;
}

static int should_sample(float sampling) {
  float p;

  if (sampling >= 1.0) {
    return TRUE;
  }

#ifdef HAVE_RANDOM
  p = ((float) random() / RAND_MAX);
#else
  p = ((float) rand() / RAND_MAX);
#endif /* HAVE_RANDOM */

  pr_trace_msg(trace_channel, 19, "sampling: p = %f, sample percentage = %f", p, 
    sampling);
  if (p > sampling) {
    return FALSE;
  }

  return TRUE;
}

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

/* usage: StatsdExcludeFilter regex|"none" */
MODRET set_statsdexcludefilter(cmd_rec *cmd) {
#ifdef PR_USE_REGEX
  pr_regex_t *pre = NULL;
  config_rec *c;
  char *pattern;
  int res;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (strcasecmp(cmd->argv[1], "none") == 0) {
    (void) add_config_param(cmd->argv[0], 0);
    return PR_HANDLED(cmd);
  }

  pre = pr_regexp_alloc(&statsd_module);

  pattern = cmd->argv[1];
  res = pr_regexp_compile(pre, pattern, REG_EXTENDED|REG_NOSUB);
  if (res != 0) {
    char errstr[256] = {'\0'};

    pr_regexp_error(res, pre, errstr, sizeof(errstr));
    pr_regexp_free(NULL, pre);

    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", pattern,
      "' failed regex compilation: ", errstr, NULL));
  }

  c = add_config_param(cmd->argv[0], 2, NULL, NULL);
  c->argv[0] = pstrdup(c->pool, pattern);
  c->argv[1] = (void *) pre;

  return PR_HANDLED(cmd);
#else
  CONF_ERROR(cmd, "The StatsdExcludeFilter directive cannot be used on this "
    "system, as you do not have POSIX compliant regex support");
#endif
}

/* usage: StatsdSampling percentage */
MODRET set_statsdsampling(cmd_rec *cmd) {
  config_rec *c;
  char *ptr = NULL;
  float percentage, sampling;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  percentage = strtof(cmd->argv[1], &ptr);
  if (ptr && *ptr) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "badly formatted percentage value: ",
      cmd->argv[1], NULL));
  }

  if (percentage <= 0.0 ||
      percentage > 100.0) {
    CONF_ERROR(cmd, "percentage must be between 0 and 100");
  }

  /* For easier comparison with e.g. random(3) values, and for formatting
   * the statsd metric values, we convert from a 1-100 value to 0.00-1.00.
   */
  sampling = percentage / 100.0;

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(float));
  *((float *) c->argv[0]) = sampling;

  return PR_HANDLED(cmd);
}

/* usage: StatsdServer [scheme://]host[:port] [prefix] [suffix] */
MODRET set_statsdserver(cmd_rec *cmd) {
  config_rec *c;
  char *server, *ptr;
  size_t server_len;
  int port = STATSD_DEFAULT_PORT, use_tcp = FALSE;

  if (cmd->argc < 2 ||
      cmd->argc > 4) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  server = pstrdup(cmd->tmp_pool, cmd->argv[1]);

  if (strncasecmp(server, "tcp://", 6) == 0) {
    use_tcp = TRUE;
    server += 6;

  } else if (strncasecmp(server, "udp://", 6) == 0) {
    use_tcp = FALSE;
    server += 6;
  }

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

  c = add_config_param(cmd->argv[0], 5, NULL, NULL, NULL, NULL, NULL);
  c->argv[0] = pstrdup(c->pool, server);
  c->argv[1] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[1]) = port;
  c->argv[2] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[2]) = use_tcp;

  if (cmd->argc > 2) {
    char *prefix;

    prefix = cmd->argv[2];
    if (*prefix) {
      /* Automatically append a '.' here, to make construction of the metric
       * name easier.
       */
      c->argv[3] = pstrcat(c->pool, prefix, ".", NULL);
    }
  }

  if (cmd->argc == 4) {
    char *suffix;

    suffix = cmd->argv[3];
    if (*suffix) {
      /* Automatically prepend a '.' here, to make construction of the metric
       * name easier.
       */
      c->argv[4] = pstrcat(c->pool, ".", suffix, NULL);
    }
  }

  return PR_HANDLED(cmd);
}

/* Command handlers
 */

static void log_tls_auth_metrics(cmd_rec *cmd, uint64_t now_ms) {
  const uint64_t *start_ms;
  char *handshake_metric, *proto_metric, *protocol_env, *cipher_env;

  handshake_metric = get_tls_metric(cmd->tmp_pool, "handshake.ctrl");
  statsd_metric_counter(statsd, handshake_metric, 1);

  proto_metric = get_conn_metric(cmd->tmp_pool, "ftps");
  statsd_metric_counter(statsd, proto_metric, 1);
  statsd_metric_gauge(statsd, proto_metric, 1, STATSD_METRIC_GAUGE_FL_ADJUST);

  start_ms = pr_table_get(cmd->notes, "start_ms", NULL);
  if (start_ms != NULL) {
    uint64_t handshake_ms;

    handshake_ms = now_ms - *start_ms;
    statsd_metric_timer(statsd, handshake_metric, handshake_ms);
  }

  cipher_env = pr_env_get(cmd->tmp_pool, "TLS_CIPHER");
  if (cipher_env != NULL) {
    char *cipher_metric;

    cipher_metric = get_tls_metric(cmd->tmp_pool,
      pstrcat(cmd->tmp_pool, "cipher.", cipher_env, NULL));
    statsd_metric_counter(statsd, cipher_metric, 1);
  }

  protocol_env = pr_env_get(cmd->tmp_pool, "TLS_PROTOCOL");
  if (protocol_env != NULL) {
    char *protocol_metric;

    protocol_metric = get_tls_metric(cmd->tmp_pool,
      pstrcat(cmd->tmp_pool, "protocol.", protocol_env, NULL));
    statsd_metric_counter(statsd, protocol_metric, 1);
  }
}

static void log_tls_metrics(cmd_rec *cmd, int had_error, uint64_t now_ms) {
  if (pr_module_exists("mod_tls.c") != TRUE) {
    return;
  }

  if (pr_cmd_cmp(cmd, PR_CMD_AUTH_ID) == 0 &&
      cmd->argc == 2) {
    char *tls_mode;

    /* Find out if the args are one of the mod_tls (vs GSSAPI et al) ones. */

    tls_mode = cmd->argv[1];
    if (strcasecmp(tls_mode, "TLS") == 0 ||
        strcasecmp(tls_mode, "TLS-C") == 0 ||
        strcasecmp(tls_mode, "TLS-P") == 0 ||
        strcasecmp(tls_mode, "SSL") == 0) {
      /* We are only interested in tracking successful handshakes here; the
       * failed handshakes are tracked elsewhere.
       */
      if (had_error == FALSE) {
        log_tls_auth_metrics(cmd, now_ms);
      }
    }
  }
}

static void log_cmd_metrics(cmd_rec *cmd, int had_error) {
  char *metric;
  const uint64_t *start_ms = NULL;
  uint64_t now_ms = 0;

  if (statsd_engine == FALSE) {
    return;
  }

  pr_gettimeofday_millis(&now_ms);

  if (should_exclude(cmd) == TRUE) {
    pr_trace_msg(trace_channel, 9,
      "command '%s' excluded by StatsdExcludeFilter '%s'", (char *) cmd->argv[0],
      statsd_exclude_filter);
    return;
  }

  if (should_sample(statsd_sampling) != TRUE) {
    pr_trace_msg(trace_channel, 28, "skipping sampling of metric for '%s'",
      (char *) cmd->argv[0]);
    return;
  }

  metric = get_cmd_metric(cmd->tmp_pool, cmd->argv[0]);
  statsd_metric_counter(statsd, metric, 1);

  start_ms = pr_table_get(cmd->notes, "start_ms", NULL);
  if (start_ms != NULL) {
    uint64_t response_ms;

    response_ms = now_ms - *start_ms;
    statsd_metric_timer(statsd, metric, response_ms);
  }

  log_tls_metrics(cmd, had_error, now_ms);

  if (pr_cmd_cmp(cmd, PR_CMD_PASS_ID) == 0 &&
      had_error == FALSE) {
    const char *proto;

    proto = pr_session_get_protocol(0);
    if (strcmp(proto, "ftp") == 0) {
      char *proto_metric;

      /* At this point in time, we are certain that we have a plain FTP
       * connection, not FTPS or SFTP or anything else.
       */
      proto_metric = get_conn_metric(cmd->tmp_pool, "ftp");
      statsd_metric_counter(statsd, proto_metric, 1);
      statsd_metric_gauge(statsd, proto_metric, 1, STATSD_METRIC_GAUGE_FL_ADJUST);
    }
  }

  statsd_statsd_flush(statsd);
}

MODRET statsd_log_any(cmd_rec *cmd) {
  log_cmd_metrics(cmd, FALSE);
  return PR_DECLINED(cmd);
}

MODRET statsd_log_any_err(cmd_rec *cmd) {
  log_cmd_metrics(cmd, TRUE);
  return PR_DECLINED(cmd);
}

/* Event handlers
 */

static void statsd_exit_ev(const void *event_data, void *user_data) {
  if (statsd != NULL) {
    char *metric;
    const char *proto;

    metric = get_conn_metric(session.pool, NULL);
    statsd_metric_gauge(statsd, metric, -1, STATSD_METRIC_GAUGE_FL_ADJUST);

    proto = pr_session_get_protocol(0);
    metric = get_conn_metric(session.pool, proto);
    statsd_metric_gauge(statsd, metric, -1, STATSD_METRIC_GAUGE_FL_ADJUST);

    statsd_statsd_close(statsd);
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
      pr_session_disconnect(&statsd_module, PR_SESS_DISCONNECT_BAD_CONFIG,
        NULL);
    }
  }
}

static void statsd_sess_reinit_ev(const void *event_data, void *user_data) {
  int res;

  /* A HOST command changed the main_server pointer; reinitialize ourselves. */

  pr_event_unregister(&statsd_module, "core.exit", statsd_exit_ev);
  pr_event_unregister(&statsd_module, "core.session-reinit",
    statsd_sess_reinit_ev);

  /* Reset internal state. */
  statsd_engine = STATSD_DEFAULT_ENGINE;
  statsd_exclude_filter = NULL;
#ifdef PR_USE_REGEX
  statsd_exclude_pre = NULL;
#endif /* PR_USE_REGEX */
  statsd_sampling = STATSD_DEFAULT_SAMPLING;

  if (statsd != NULL) {
    statsd_statsd_close(statsd);
    statsd = NULL;
  }

  res = statsd_sess_init();
  if (res < 0) {
    pr_session_disconnect(&statsd_module, PR_SESS_DISCONNECT_SESSION_INIT_FAILED,
      NULL);
  }
}

static void statsd_shutdown_ev(const void *event_data, void *user_data) {
  if (statsd != NULL) {
    statsd_statsd_close(statsd);
    statsd = NULL;
  }
}

static void statsd_ssh2_sftp_sess_opened_ev(const void *event_data,
    void *user_data) {
  pool *tmp_pool;
  char *proto_metric;

  if (should_sample(statsd_sampling) == FALSE) {
    return;
  }

  tmp_pool = make_sub_pool(session.pool);
  proto_metric = get_conn_metric(tmp_pool, "sftp");
  statsd_metric_counter(statsd, proto_metric, 1);
  statsd_metric_gauge(statsd, proto_metric, 1, STATSD_METRIC_GAUGE_FL_ADJUST);
  statsd_statsd_flush(statsd);
  destroy_pool(tmp_pool);
}

static void statsd_ssh2_scp_sess_opened_ev(const void *event_data,
    void *user_data) {
  pool *tmp_pool;
  char *proto_metric;

  if (should_sample(statsd_sampling) == FALSE) {
    return;
  }

  tmp_pool = make_sub_pool(session.pool);
  proto_metric = get_conn_metric(tmp_pool, "scp");
  statsd_metric_counter(statsd, proto_metric, 1);
  statsd_metric_gauge(statsd, proto_metric, 1, STATSD_METRIC_GAUGE_FL_ADJUST);
  statsd_statsd_flush(statsd);
  destroy_pool(tmp_pool);
}

static void incr_timeout_metric(const char *name) {
  pool *tmp_pool;
  char *metric;

  /* Unlike other common metrics, for now the timeout counters are NOT subject
   * to the sampling frequency.
   */

  tmp_pool = make_sub_pool(session.pool);
  metric = get_timeout_metric(tmp_pool, name);
  statsd_metric_counter(statsd, metric, 1);
  statsd_statsd_flush(statsd);
  destroy_pool(tmp_pool);
}

static void statsd_timeout_idle_ev(const void *event_data, void *user_data) {
  incr_timeout_metric("TimeoutIdle");
}

static void statsd_timeout_login_ev(const void *event_data, void *user_data) {
  incr_timeout_metric("TimeoutLogin");
}

static void statsd_timeout_noxfer_ev(const void *event_data, void *user_data) {
  incr_timeout_metric("TimeoutNoTransfer");
}

static void statsd_timeout_session_ev(const void *event_data, void *user_data) {
  incr_timeout_metric("TimeoutSession");
}

static void statsd_timeout_stalled_ev(const void *event_data, void *user_data) {
  incr_timeout_metric("TimeoutStalled");
}

static void incr_tls_handshake_error_metric(const char *name) {
  pool *tmp_pool;
  char *metric;

  tmp_pool = make_sub_pool(session.pool);

  /* Unlike other common metrics, for now the TLS handshake counters are NOT
   * subject to the sampling frequency.
   */

  metric = get_tls_metric(tmp_pool, name);
  statsd_metric_counter(statsd, metric, 1);
  statsd_statsd_flush(statsd);
  destroy_pool(tmp_pool);
}

static void statsd_tls_ctrl_handshake_error_ev(const void *event_data,
    void *user_data) {
  incr_tls_handshake_error_metric("handshake.ctrl.error");
}

static void statsd_tls_data_handshake_error_ev(const void *event_data,
    void *user_data) {
  incr_tls_handshake_error_metric("handshake.data.error");
}

/* Initialization functions
 */

static int statsd_sess_init(void) {
  config_rec *c;
  char *host, *metric, *prefix = NULL, *suffix = NULL;
  int port, use_tcp = FALSE;
  const pr_netaddr_t *addr;

  pr_event_register(&statsd_module, "core.session-reinit", statsd_sess_reinit_ev,
    NULL);

  c = find_config(main_server->conf, CONF_PARAM, "StatsdEngine", FALSE);
  if (c != NULL) {
    statsd_engine = *((int *) c->argv[0]);
  }

  if (statsd_engine == FALSE) {
    return 0;
  }

  c = find_config(main_server->conf, CONF_PARAM, "StatsdServer", FALSE);
  if (c == NULL) {
    pr_log_debug(DEBUG10, MOD_STATSD_VERSION
      ": missing required StatsdServer directive, disabling module");
    statsd_engine = FALSE;
    return 0;
  }

  host = c->argv[0];
  addr = pr_netaddr_get_addr(session.pool, host, NULL);
  if (addr == NULL) {
    pr_log_pri(PR_LOG_NOTICE, MOD_STATSD_VERSION
      ": error resolving '%s' to IP address: %s", host, strerror(errno));
    statsd_engine = FALSE;
    return 0;
  }

  port = *((int *) c->argv[1]);
  pr_netaddr_set_port2((pr_netaddr_t *) addr, port);

  use_tcp = *((int *) c->argv[2]);
  prefix = c->argv[3];
  suffix = c->argv[4];

  statsd = statsd_statsd_open(session.pool, addr, use_tcp, statsd_sampling,
    prefix, suffix);
  if (statsd == NULL) {
    pr_log_pri(PR_LOG_NOTICE, MOD_STATSD_VERSION
      ": error opening statsd connection to %s%s:%d: %s",
      use_tcp ? "tcp://" : "udp://", host, port, strerror(errno));
    statsd_engine = FALSE;
    return 0;
  }

#ifdef HAVE_SRANDOM
  srandom((unsigned int) (time(NULL) ^ getpid()));
#else
  srand((unsigned int) (time(NULL) ^ getpid()));
#endif /* HAVE_SRANDOM */

  c = find_config(main_server->conf, CONF_PARAM, "StatsdExcludeFilter", FALSE);
  if (c != NULL &&
      c->argc == 2) {
    statsd_exclude_filter = c->argv[0];
    statsd_exclude_pre = c->argv[1];
  }

  c = find_config(main_server->conf, CONF_PARAM, "StatsdSampling", FALSE);
  if (c != NULL) {
    statsd_sampling = *((float *) c->argv[0]);
  }

  metric = get_conn_metric(session.pool, NULL);
  statsd_metric_gauge(statsd, metric, 1, STATSD_METRIC_GAUGE_FL_ADJUST);
  statsd_statsd_flush(statsd);

  pr_event_register(&statsd_module, "core.exit", statsd_exit_ev, NULL);

  pr_event_register(&statsd_module, "core.timeout-idle",
    statsd_timeout_idle_ev, NULL);
  pr_event_register(&statsd_module, "core.timeout-login",
    statsd_timeout_login_ev, NULL);
  pr_event_register(&statsd_module, "core.timeout-no-transfer",
    statsd_timeout_noxfer_ev, NULL);
  pr_event_register(&statsd_module, "core.timeout-session",
    statsd_timeout_session_ev, NULL);
  pr_event_register(&statsd_module, "core.timeout-stalled",
    statsd_timeout_stalled_ev, NULL);

  if (pr_module_exists("mod_sftp.c") == TRUE) {
    pr_event_register(&statsd_module, "mod_sftp.sftp.session-opened",
      statsd_ssh2_sftp_sess_opened_ev, NULL);
    pr_event_register(&statsd_module, "mod_sftp.scp.session-opened",
      statsd_ssh2_scp_sess_opened_ev, NULL);
  }

  if (pr_module_exists("mod_tls.c") == TRUE) {
    pr_event_register(&statsd_module, "mod_tls.ctrl-handshake-failed",
      statsd_tls_ctrl_handshake_error_ev, NULL);
    pr_event_register(&statsd_module, "mod_tls.data-handshake-failed",
      statsd_tls_data_handshake_error_ev, NULL);
  }

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
  { "StatsdEngine",		set_statsdengine,		NULL },
  { "StatsdExcludeFilter",	set_statsdexcludefilter,	NULL },
  { "StatsdSampling",		set_statsdsampling,		NULL },
  { "StatsdServer",		set_statsdserver,		NULL },
  { NULL }
};

static cmdtable statsd_cmdtab[] = {
  { LOG_CMD,		C_ANY,	G_NONE,	statsd_log_any,		FALSE,	FALSE },
  { LOG_CMD_ERR,	C_ANY,	G_NONE,	statsd_log_any_err,	FALSE,	FALSE },
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
