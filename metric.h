/*
 * ProFTPD - mod_statsd Metric API
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

#ifndef MOD_STATSD_METRIC_H
#define MOD_STATSD_METRIC_H

#include "mod_statsd.h"
#include "statsd.h"

int statsd_metric_counter(struct statsd *statsd, const char *name, int64_t incr,
  int flags);
int statsd_metric_timer(struct statsd *statsd, const char *name, uint64_t ms,
  int flags);
int statsd_metric_gauge(struct statsd *statsd, const char *name, int64_t val,
  int flags);

/* Use this flag, for a gauge, for adjusting the existing gauge value, rather
 * that setting it.
 */
#define STATSD_METRIC_FL_GAUGE_ADJUST		0x0001

/* Usage this flag to indicate that the metric is NOT subject to the sampling
 * frequency.
 */
#define STATSD_METRIC_FL_IGNORE_SAMPLING	0x0002

#endif /* MOD_STATSD_METRIC_H */
