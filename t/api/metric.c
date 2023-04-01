/*
 * ProFTPD - mod_statsd testsuite
 * Copyright (c) 2017-2022 TJ Saunders <tj@castaglia.org>
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

/* Alias tests. */

#include "tests.h"
#include "statsd.h"
#include "metric.h"

static pool *p = NULL;

static void set_up(void) {
  if (p == NULL) {
    p = make_sub_pool(NULL);
  }

  if (getenv("TEST_VERBOSE") != NULL) {
    pr_trace_set_levels("statsd.metric", 1, 20);
  }
}

static void tear_down(void) {
  if (getenv("TEST_VERBOSE") != NULL) {
    pr_trace_set_levels("statsd.metric", 0, 0);
  }

  if (p) {
    destroy_pool(p);
    p = NULL;
  }
}

static const pr_netaddr_t *statsd_addr(unsigned int port) {
  const pr_netaddr_t *addr;

  addr = pr_netaddr_get_addr(p, "127.0.0.1", NULL);
  ck_assert_msg(addr != NULL, "Failed to resolve 127.0.0.1: %s", strerror(errno));
  pr_netaddr_set_port2((pr_netaddr_t *) addr, port);

  return addr;
}

START_TEST (metric_counter_test) {
  int res;
  const pr_netaddr_t *addr;
  struct statsd *statsd;

  mark_point();
  res = statsd_metric_counter(NULL, NULL, 0, 0);
  ck_assert_msg(res < 0, "Failed to handle null statsd");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  addr = statsd_addr(STATSD_DEFAULT_PORT);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  mark_point();
  res = statsd_metric_counter(statsd, NULL, 0, 0);
  ck_assert_msg(res < 0, "Failed to handle null name");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = statsd_metric_counter(statsd, "foo", 0, 0);
  ck_assert_msg(res == 0, "Failed to set counter: %s", strerror(errno));

  mark_point();
  res = statsd_metric_counter(statsd, "foo", 0, STATSD_METRIC_FL_IGNORE_SAMPLING);
  ck_assert_msg(res == 0, "Failed to set counter: %s", strerror(errno));

  mark_point();
  res = statsd_statsd_flush(statsd);
  ck_assert_msg(res == 0, "Failed to flush metrics: %s", strerror(errno));

  (void) statsd_statsd_close(statsd);
}
END_TEST

START_TEST (metric_timer_test) {
  int res;
  const pr_netaddr_t *addr;
  struct statsd *statsd;
  uint64_t ms;

  mark_point();
  res = statsd_metric_timer(NULL, NULL, 0, 0);
  ck_assert_msg(res < 0, "Failed to handle null statsd");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  addr = statsd_addr(STATSD_DEFAULT_PORT);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  mark_point();
  res = statsd_metric_timer(statsd, NULL, 0, 0);
  ck_assert_msg(res < 0, "Failed to handle null name");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  ms = 1;

  mark_point();
  res = statsd_metric_timer(statsd, "foo", ms, 0);
  ck_assert_msg(res == 0, "Failed to set timer: %s", strerror(errno));

  /* Deliberately use a very large timer, to test the truncation. */
  ms = 315360000000UL;

  mark_point();
  res = statsd_metric_timer(statsd, "bar", ms, 0);
  ck_assert_msg(res == 0, "Failed to set timer: %s", strerror(errno));

  mark_point();
  res = statsd_metric_timer(statsd, "bar", ms, STATSD_METRIC_FL_IGNORE_SAMPLING);
  ck_assert_msg(res == 0, "Failed to set timer: %s", strerror(errno));

  mark_point();
  res = statsd_statsd_flush(statsd);
  ck_assert_msg(res == 0, "Failed to flush metrics: %s", strerror(errno));

  (void) statsd_statsd_close(statsd);
}
END_TEST

START_TEST (metric_gauge_test) {
  int res;
  const pr_netaddr_t *addr;
  struct statsd *statsd;

  mark_point();
  res = statsd_metric_gauge(NULL, NULL, 0, 0);
  ck_assert_msg(res < 0, "Failed to handle null statsd");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  addr = statsd_addr(STATSD_DEFAULT_PORT);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  mark_point();
  res = statsd_metric_gauge(statsd, NULL, 0, 0);
  ck_assert_msg(res < 0, "Failed to handle null name");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = statsd_metric_gauge(statsd, "foo", 1, 0);
  ck_assert_msg(res == 0, "Failed to set gauge: %s", strerror(errno));

  mark_point();
  res = statsd_metric_gauge(statsd, "foo", 1, STATSD_METRIC_FL_GAUGE_ADJUST);
  ck_assert_msg(res == 0, "Failed to set gauge: %s", strerror(errno));

  mark_point();
  res = statsd_metric_gauge(statsd, "foo", -1, STATSD_METRIC_FL_GAUGE_ADJUST);
  ck_assert_msg(res == 0, "Failed to set gauge: %s", strerror(errno));

  mark_point();
  res = statsd_statsd_flush(statsd);
  ck_assert_msg(res == 0, "Failed to flush metrics: %s", strerror(errno));

  (void) statsd_statsd_close(statsd);
}
END_TEST

Suite *tests_get_metric_suite(void) {
  Suite *suite;
  TCase *testcase;

  suite = suite_create("metric");
  testcase = tcase_create("base");

  tcase_add_checked_fixture(testcase, set_up, tear_down);

  tcase_add_test(testcase, metric_counter_test);
  tcase_add_test(testcase, metric_timer_test);
  tcase_add_test(testcase, metric_gauge_test);

  suite_add_tcase(suite, testcase);
  return suite;
}
