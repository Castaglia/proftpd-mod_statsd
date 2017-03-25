/*
 * ProFTPD - mod_statsd Statsd API
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

#ifndef MOD_STATSD_STATSD_H
#define MOD_STATSD_STATSD_H

#include "mod_statsd.h"

struct statsd;

/* Per the excellent documentation on multi-metric packets here:
 *
 *  https://github.com/etsy/statsd/blob/master/docs/metric_types.md#multi-metric-packets
 *
 * We'll use a maximum UDP packet size of 512 bytes, for interoperability.
 */
#define STATSD_MAX_UDP_PACKET_SIZE		512

/* The max length of a single metric is the same as the max packet size. */
#define STATSD_MAX_METRIC_SIZE			STATSD_MAX_UDP_PACKET_SIZE

struct statsd *statsd_statsd_open(pool *p, const pr_netaddr_t *addr,
  int use_tcp, float sampling);
int statsd_statsd_close(struct statsd *statsd);

int statsd_statsd_write(struct statsd *statsd, const char *metric,
  size_t metric_len, int flags);
#define STATSD_STATSD_FL_SEND_NOW	0x0001

/* Flush any buffered pending metrics */
int statsd_statsd_flush(struct statsd *statsd);

/* Returns a reference to pool used for the statsd client. */
pool *statsd_statsd_get_pool(struct statsd *statsd);

/* Returns the sampling percentage for the statsd client. */
float statsd_statsd_get_sampling(struct statsd *statsd);

/* This is for testing purposes. */
int statsd_statsd_set_fd(struct statsd *statsd, int fd);

int statsd_statsd_init(void);
int statsd_statsd_free(void);

#endif /* MOD_STATSD_STATSD_H */
