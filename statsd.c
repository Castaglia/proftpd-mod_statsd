/*
 * ProFTPD: mod_statsd Statsd API
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, the respective copyright holders give permission
 * to link this program with OpenSSL, and distribute the resulting
 * executable, without including the source code for OpenSSL in the source
 * distribution.
 */

#include "statsd.h"

struct statsd {
  pool *pool;

  const pr_netaddr_t *addr;
  int fd;

  /* Pending metrics */
  pool *metrics_pool;
  char *metrics_buf;
  size_t metrics_buflen;
};

static int statsd_proto_udp = IPPROTO_UDP;

static const char *trace_channel = "statsd.statsd";

struct statsd *statsd_statsd_open(pool *p, const pr_netaddr_t *addr) {
  int family, fd, xerrno;
  pool *sub_pool;
  struct statsd *statsd;

  if (p == NULL ||
      addr == NULL) {
    errno = EINVAL;
    return NULL;
  }

  family = pr_netaddr_get_family(addr);
  fd = socket(family, SOCK_DGRAM, statsd_proto_udp);
  xerrno = errno;

  if (fd < 0) {
    pr_trace_msg(trace_channel, 1, "error opening %s UDP socket: %s",
      family == AF_INET ? "IPv4" : "IPv6", strerror(xerrno));
    errno = xerrno;
    return NULL;
  }

  sub_pool = make_sub_pool(p);
  pr_pool_tag(sub_pool, "Statsd Client Pool");

  statsd = pcalloc(sub_pool, sizeof(struct statsd));
  statsd->pool = sub_pool;
  statsd->addr = addr;
  statsd->fd = fd;

  return statsd;
}

int statsd_statsd_close(struct statsd *statsd) {
  if (statsd == NULL) {
    errno = EINVAL;
    return -1;
  }

  (void) close(statsd->fd);
  destroy_pool(statsd->pool);

  return 0;
}

pool *statsd_statsd_get_pool(struct statsd *statsd) {
  if (statsd == NULL) {
    errno = EINVAL;
    return NULL;
  }

  return statsd->pool;
}

int statsd_statsd_set_fd(struct statsd *statsd, int fd) {
  if (statsd == NULL) {
    errno = EINVAL;
    return -1;
  }

  (void) close(statsd->fd);
  statsd->fd = fd;

  return 0;
}

static void send_metrics(struct statsd *statsd, const void *buf, size_t len) {
  if (statsd->addr != NULL) {
    int res, xerrno;

    while (TRUE) {
      res = sendto(statsd->fd, buf, len, 0, pr_netaddr_get_sockaddr(statsd->addr),
        pr_netaddr_get_sockaddr_len(statsd->addr));
      xerrno = errno;

      if (res < 0) {
        if (xerrno == EINTR) {
          pr_signals_handle();
          continue;
        }

        pr_trace_msg(trace_channel, 5,
          "error sending %lu bytes of metrics data to %s:%d: %s",
          (unsigned long) len, pr_netaddr_get_ipstr(statsd->addr),
          ntohs(pr_netaddr_get_port(statsd->addr)), strerror(xerrno));
        errno = xerrno;

      } else {
        /* XXX Should we watch for short writes? */
        pr_trace_msg(trace_channel, 19,
          "sent %d bytes of metrics data (of %lu bytes pending) to %s:%d", res,
          (unsigned long) len, pr_netaddr_get_ipstr(statsd->addr),
          ntohs(pr_netaddr_get_port(statsd->addr)));
      }

      break;
    }
  }
}

static void clear_metrics(struct statsd *statsd) {
  if (statsd->metrics_pool != NULL) {
    destroy_pool(statsd->metrics_pool);
  }
  statsd->metrics_pool = NULL;
  statsd->metrics_buf = NULL;
  statsd->metrics_buflen = 0;
}

int statsd_statsd_write(struct statsd *statsd, const char *metric,
    size_t metric_len, int flags) {

  if (statsd == NULL ||
      metric == NULL ||
      metric_len == 0) {
    errno = EINVAL;
    return -1;
  }

  /* Would this metric put us over the max packet size?  If so, flush the
   * metrics now.
   */
  if (statsd->metrics_buf != NULL) {
    if ((statsd->metrics_buflen + metric_len + 1) > STATSD_MAX_PACKET_SIZE) {
      send_metrics(statsd, statsd->metrics_buf, statsd->metrics_buflen);
      clear_metrics(statsd);
    }
  }

  if (statsd->metrics_buf != NULL) {
    statsd->metrics_buf = pstrcat(statsd->metrics_pool, statsd->metrics_buf,
      "\n", metric, NULL);
    statsd->metrics_buflen += (metric_len + 1);

  } else {
    statsd->metrics_pool = make_sub_pool(statsd->pool);
    pr_pool_tag(statsd->metrics_pool, "Statsd buffered metrics pool");

    statsd->metrics_buf = pstrndup(statsd->metrics_pool, metric, metric_len);
    statsd->metrics_buflen = metric_len;
  }

  if (flags & STATSD_STATSD_FL_SEND_NOW) {
    send_metrics(statsd, statsd->metrics_buf, statsd->metrics_buflen);
    clear_metrics(statsd);
  }

  return 0;
}

int statsd_statsd_flush(struct statsd *statsd) {
  if (statsd == NULL) {
    errno = EINVAL;
    return -1;
  }

  send_metrics(statsd, statsd->metrics_buf, statsd->metrics_buflen);
  clear_metrics(statsd);
  return 0;
}

int statsd_statsd_init(void) {
  struct protoent *pre = NULL;

#ifdef HAVE_SETPROTOENT
  setprotoent(FALSE);
#endif /* SETPROTOENT */

  pre = getprotobyname("udp");
  if (pre != NULL) {
    statsd_proto_udp = pre->p_proto;
  }

#ifdef HAVE_ENDPROTOENT
  endprotoent();
#endif /* HAVE_ENDPROTOENT */

  return 0;
}

int statsd_statsd_free(void) {
  return 0;
}
