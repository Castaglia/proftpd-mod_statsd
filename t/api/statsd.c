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

static pool *p = NULL;

static void set_up(void) {
  if (p == NULL) {
    p = make_sub_pool(NULL);
  }

  statsd_statsd_init();

  if (getenv("TEST_VERBOSE") != NULL) {
    pr_trace_set_levels("statsd.statsd", 1, 20);
  }
}

static void tear_down(void) {
  if (getenv("TEST_VERBOSE") != NULL) {
    pr_trace_set_levels("statsd.statsd", 0, 0);
  }

  statsd_statsd_free();

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

START_TEST (statsd_close_test) {
  int res;

  mark_point();
  res = statsd_statsd_close(NULL);
  ck_assert_msg(res < 0, "Failed to handle null statsd");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);
}
END_TEST

START_TEST (statsd_open_test) {
  const pr_netaddr_t *addr;
  struct statsd *statsd;

  mark_point();
  statsd = statsd_statsd_open(NULL, NULL, FALSE, 0.0, NULL, NULL);
  ck_assert_msg(statsd == NULL, "Failed to handle null pool");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  statsd = statsd_statsd_open(p, NULL, FALSE, -1.0, NULL, NULL);
  ck_assert_msg(statsd == NULL, "Failed to handle null addr");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  addr = statsd_addr(STATSD_DEFAULT_PORT);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, -1.0, NULL, NULL);
  ck_assert_msg(statsd == NULL, "Failed to handle invalid sampling");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  (void) statsd_statsd_close(statsd);

  mark_point();
  statsd = statsd_statsd_open(p, addr, TRUE, 1.0, NULL, NULL);

  /* If statsd IS running, but is not configued for TCP, the "Connection
   * refused" error is expected.
   */
  if (statsd != NULL &&
      errno != ECONNREFUSED) {
    ck_abort_msg("Failed to open TCP statsd connection: %s", strerror(errno));

  } else {
    (void) statsd_statsd_close(statsd);
  }
}
END_TEST

START_TEST (statsd_get_namespacing_test) {
  int res;
  const char *prefix, *suffix;
  const pr_netaddr_t *addr;
  struct statsd *statsd;

  mark_point();
  res = statsd_statsd_get_namespacing(NULL, NULL, NULL);
  ck_assert_msg(res < 0, "Failed to handle null statsd");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  addr = statsd_addr(STATSD_DEFAULT_PORT);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  mark_point();
  res = statsd_statsd_get_namespacing(statsd, NULL, NULL);
  ck_assert_msg(res < 0, "Failed to handle null prefix AND suffix");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  prefix = suffix = NULL;

  mark_point();
  res = statsd_statsd_get_namespacing(statsd, &prefix, &suffix);
  ck_assert_msg(res == 0, "Failed to get namespacing: %s", strerror(errno));
  ck_assert_msg(prefix == NULL, "Got prefix %s unexpectedly", prefix);
  ck_assert_msg(suffix == NULL, "Got suffix %s unexpectedly", suffix);

  (void) statsd_statsd_close(statsd);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, "foo", "bar");
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  prefix = suffix = NULL;

  mark_point();
  res = statsd_statsd_get_namespacing(statsd, &prefix, &suffix);
  ck_assert_msg(res == 0, "Failed to get namespacing: %s", strerror(errno));
  ck_assert_msg(prefix != NULL, "Expected prefix, got null");
  ck_assert_msg(strcmp(prefix, "foo") == 0, "Expected 'foo', got '%s'", prefix);
  ck_assert_msg(suffix != NULL, "Expected suffix, got null");
  ck_assert_msg(strcmp(suffix, "bar") == 0, "Expected 'bar', got '%s'", suffix);

  prefix = suffix = NULL;

  mark_point();
  res = statsd_statsd_get_namespacing(statsd, &prefix, NULL);
  ck_assert_msg(res == 0, "Failed to get namespacing: %s", strerror(errno));
  ck_assert_msg(prefix != NULL, "Expected prefix, got null");
  ck_assert_msg(strcmp(prefix, "foo") == 0, "Expected 'foo', got '%s'", prefix);

  prefix = suffix = NULL;

  mark_point();
  res = statsd_statsd_get_namespacing(statsd, NULL, &suffix);
  ck_assert_msg(res == 0, "Failed to get namespacing: %s", strerror(errno));
  ck_assert_msg(suffix != NULL, "Expected suffix, got null");
  ck_assert_msg(strcmp(suffix, "bar") == 0, "Expected 'bar', got '%s'", suffix);

  (void) statsd_statsd_close(statsd);
}
END_TEST

START_TEST (statsd_get_pool_test) {
  pool *res;
  const pr_netaddr_t *addr;
  struct statsd *statsd;

  mark_point();
  res = statsd_statsd_get_pool(NULL);
  ck_assert_msg(res == NULL, "Failed to handle null statsd");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  addr = statsd_addr(STATSD_DEFAULT_PORT);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  mark_point();
  res = statsd_statsd_get_pool(statsd);
  ck_assert_msg(res != NULL, "Failed to get pool: %s", strerror(errno));

  (void) statsd_statsd_close(statsd);
}
END_TEST

START_TEST (statsd_get_sampling_test) {
  float res;
  const pr_netaddr_t *addr;
  struct statsd *statsd;

  mark_point();
  res = statsd_statsd_get_sampling(NULL);
  ck_assert_msg(res < 0.0, "Failed to handle null statsd");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  addr = statsd_addr(STATSD_DEFAULT_PORT);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  mark_point();
  res = statsd_statsd_get_sampling(statsd);
  ck_assert_msg(res >= 1.0, "Failed to get sampling: %s", strerror(errno));

  (void) statsd_statsd_close(statsd);
}
END_TEST

START_TEST (statsd_set_fd_test) {
  int res;
  const pr_netaddr_t *addr;
  struct statsd *statsd;

  mark_point();
  res = statsd_statsd_set_fd(NULL, -1);
  ck_assert_msg(res < 0, "Failed to handle null statsd");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  addr = statsd_addr(STATSD_DEFAULT_PORT);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  mark_point();
  res = statsd_statsd_set_fd(statsd, -1);
  ck_assert_msg(res == 0, "Failed to set fd: %s", strerror(errno));

  (void) statsd_statsd_close(statsd);
}
END_TEST

START_TEST (statsd_write_test) {
  int res;
  const pr_netaddr_t *addr;
  struct statsd *statsd;

  mark_point();
  res = statsd_statsd_write(NULL, NULL, 0, 0);
  ck_assert_msg(res < 0, "Failed to handle null statsd");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  addr = statsd_addr(STATSD_DEFAULT_PORT);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  mark_point();
  res = statsd_statsd_write(statsd, NULL, 0, 0);
  ck_assert_msg(res < 0, "Failed to handle null metric");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = statsd_statsd_write(statsd, "foo", 0, 0);
  ck_assert_msg(res < 0, "Failed to handle zero length metric");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = statsd_statsd_write(statsd, "foo", 3, 0);
  ck_assert_msg(res == 0, "Failed to send metric: %s", strerror(errno));

  mark_point();
  res = statsd_statsd_write(statsd, "bar", 3, STATSD_STATSD_FL_SEND_NOW);
  ck_assert_msg(res == 0, "Failed to send metric now: %s", strerror(errno));

  (void) statsd_statsd_close(statsd);

  /* Now test sending metrics to a bad port. */
  addr = statsd_addr(45778);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  mark_point();
  res = statsd_statsd_write(statsd, "bar", 3, STATSD_STATSD_FL_SEND_NOW);
  ck_assert_msg(res == 0, "Failed to send metric now: %s", strerror(errno));

  (void) statsd_statsd_close(statsd);
}
END_TEST

START_TEST (statsd_flush_test) {
  int res;
  const pr_netaddr_t *addr;
  struct statsd *statsd;

  mark_point();
  res = statsd_statsd_flush(NULL);
  ck_assert_msg(res < 0, "Failed to handle null statsd");
  ck_assert_msg(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  addr = statsd_addr(STATSD_DEFAULT_PORT);

  mark_point();
  statsd = statsd_statsd_open(p, addr, FALSE, 1.0, NULL, NULL);
  ck_assert_msg(statsd != NULL, "Failed to open statsd connection: %s",
    strerror(errno));

  mark_point();
  res = statsd_statsd_flush(statsd);
  ck_assert_msg(res == 0, "Failed to flush metrics: %s", strerror(errno));

  mark_point();
  res = statsd_statsd_write(statsd, "foo", 3, 0);
  ck_assert_msg(res == 0, "Failed to send metric: %s", strerror(errno));

  mark_point();
  res = statsd_statsd_flush(statsd);
  ck_assert_msg(res == 0, "Failed to flush metrics: %s", strerror(errno));

  (void) statsd_statsd_close(statsd);
}
END_TEST

Suite *tests_get_statsd_suite(void) {
  Suite *suite;
  TCase *testcase;

  suite = suite_create("statsd");
  testcase = tcase_create("base");

  tcase_add_checked_fixture(testcase, set_up, tear_down);

  tcase_add_test(testcase, statsd_close_test);
  tcase_add_test(testcase, statsd_open_test);
  tcase_add_test(testcase, statsd_get_namespacing_test);
  tcase_add_test(testcase, statsd_get_pool_test);
  tcase_add_test(testcase, statsd_get_sampling_test);
  tcase_add_test(testcase, statsd_set_fd_test);
  tcase_add_test(testcase, statsd_write_test);
  tcase_add_test(testcase, statsd_flush_test);

  suite_add_tcase(suite, testcase);
  return suite;
}
