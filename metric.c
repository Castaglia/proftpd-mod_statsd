/*
 * ProFTPD: mod_statsd Metric API
 * Copyright (c) 2017-2022 TJ Saunders
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

#include "metric.h"

/* Don't allow timings longer than 1 year. */
#define STATSD_MAX_TIME_MS	31536000000UL

static const char *trace_channel = "statsd.metric";

/* Watch out for any characters which might interfere with the statsd format. */
static char *sanitize_name(pool *p, const char *name) {
  char *cleaned_name, *ptr;
  int adjusted_name = FALSE;

  cleaned_name = pstrdup(p, name);
  for (ptr = cleaned_name; *ptr; ptr++) {
    if (*ptr == ':' ||
        *ptr == '|' ||
        *ptr == '@') {
      *ptr = '_';
      adjusted_name = TRUE;
    }
  }

  if (adjusted_name == TRUE) {
    pr_trace_msg(trace_channel, 12, "sanitized metric name '%s' into '%s'",
      name, cleaned_name);
  }

  return cleaned_name;
}

static int write_metric(struct statsd *statsd, const char *metric_type,
    const char *name, const char *val_prefix, int64_t val, float sampling) {
  int res, xerrno;
  pool *p, *tmp_pool;
  const char *prefix = NULL, *suffix = NULL;
  char *metric;
  size_t metric_len;

  statsd_statsd_get_namespacing(statsd, &prefix, &suffix);
  p = statsd_statsd_get_pool(statsd);
  tmp_pool = make_sub_pool(p);

  metric_len = STATSD_MAX_METRIC_SIZE;
  metric = pcalloc(tmp_pool, metric_len);

  if (sampling >= 1.0) {
    res = snprintf(metric, metric_len-1, "%s%s%s:%s%lld|%s",
      prefix != NULL ? prefix : "", sanitize_name(tmp_pool, name),
      suffix != NULL ? suffix : "", val_prefix, (long long) val, metric_type);

  } else {
    res = snprintf(metric, metric_len-1, "%s%s%s:%s%lld|%s|@%.2f",
      prefix != NULL ? prefix : "", sanitize_name(tmp_pool, name),
      suffix != NULL ? suffix : "", val_prefix, (long long) val, metric_type,
      sampling);
  }

  res = statsd_statsd_write(statsd, metric, res, 0);
  xerrno = errno;

  destroy_pool(tmp_pool);

  errno = xerrno;
  return res;
}

int statsd_metric_counter(struct statsd *statsd, const char *name,
    int64_t incr, int flags) {
  float sampling;

  if (statsd == NULL ||
      name == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (flags & STATSD_METRIC_FL_IGNORE_SAMPLING) {
    sampling = 1.0;

  } else {
    sampling = statsd_statsd_get_sampling(statsd);
  }

  return write_metric(statsd, "c", name, "", incr, sampling);
}

int statsd_metric_timer(struct statsd *statsd, const char *name, uint64_t ms,
    int flags) {
  float sampling;

  if (statsd == NULL ||
      name == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (ms > STATSD_MAX_TIME_MS) {
    pr_trace_msg(trace_channel, 19, "truncating time %lu ms to max %lu ms",
      (unsigned long) ms, (unsigned long) STATSD_MAX_TIME_MS);
    ms = STATSD_MAX_TIME_MS;
  }

  if (flags & STATSD_METRIC_FL_IGNORE_SAMPLING) {
    sampling = 1.0;

  } else {
    sampling = statsd_statsd_get_sampling(statsd);
  }

  return write_metric(statsd, "ms", name, "", ms, sampling);
}

int statsd_metric_gauge(struct statsd *statsd, const char *name, int64_t val,
    int flags) {
  char *val_prefix;

  if (statsd == NULL ||
      name == NULL) {
    errno = EINVAL;
    return -1;
  }

  val_prefix = "";

  if (flags & STATSD_METRIC_FL_GAUGE_ADJUST) {
    if (val > 0) {
      val_prefix = "+";

    } else if (val < 0) {
      val_prefix = "-";
    }

  } else {
    /* If we are NOT adjusting an existing gauge value, then a negative
     * gauge value makes no sense.
     */
    if (val < 0) {
      val = 0;
    }
  }

  /* Unlike counters and timers, gauges are NOT subject to sampling frequency;
   * the statsd protocol does not allow for this, and rightly so.
   */
  return write_metric(statsd, "g", name, val_prefix, val, 1.0);
}
