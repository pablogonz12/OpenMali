// SPDX-License-Identifier: MIT
/* Shared --loop harness for t_triangle / t_uniform / t_depth / t_texture.
 *   --loop N            repeat the test body N times in one process (default 1: output unchanged)
 *   --loop-reset-ctx    also recreate context/queue/heap/BOs for every iteration
 *   --loop-max-fail K   stop after K failing iterations (default 3; a faulted queue makes every later one a timeout)
 *   --dump-on-fail      accepted; failing iterations always dump (existing behaviour) and their captured output is replayed
 * Passing iterations' output is captured and discarded, failing ones are replayed, then one "loop_summary" line is printed.
 * "us" per iteration = sum of submit->signal of all draws/scenes in the iteration. */
#ifndef T_LOOP_H
#define T_LOOP_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

static struct {
	int n, reset, max_fail, active, depth, iter, pass, fail, first_fail;
	double us_acc, us_min, us_max, us_sum;
	int us_n, saved_fd, cap_fd;
} g_loop = { 1, 0, 3, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, -1, -1 };

static void loop_add_us(double us) { g_loop.us_acc += us; }

static void loop_iter_begin(void)
{
	if (!g_loop.active) return;
	if (g_loop.depth++) return;
	g_loop.us_acc = 0;
	fflush(stdout);
#ifdef SYS_memfd_create
	g_loop.cap_fd = (int)syscall(SYS_memfd_create, "loopcap", 0);
#endif
	if (g_loop.cap_fd >= 0) {
		g_loop.saved_fd = dup(1);
		dup2(g_loop.cap_fd, 1);
	}
}

static void loop_iter_end(int ok)
{
	if (!g_loop.active) return;
	if (--g_loop.depth) return;
	fflush(stdout);
	if (g_loop.cap_fd >= 0) {
		dup2(g_loop.saved_fd, 1);
		close(g_loop.saved_fd);
		if (!ok) {
			char buf[4096];
			ssize_t k;
			lseek(g_loop.cap_fd, 0, SEEK_SET);
			printf("{\"test\":\"loop_iteration_output\",\"iter\":%d}\n", g_loop.iter);
			fflush(stdout);
			while ((k = read(g_loop.cap_fd, buf, sizeof buf)) > 0) { if (write(1, buf, (size_t)k) < 0) break; }
		}
		close(g_loop.cap_fd);
		g_loop.cap_fd = -1;
	}
	if (ok) g_loop.pass++;
	else { g_loop.fail++; if (g_loop.first_fail < 0) g_loop.first_fail = g_loop.iter; }
	if (g_loop.us_acc > 0) {
		if (!g_loop.us_n || g_loop.us_acc < g_loop.us_min) g_loop.us_min = g_loop.us_acc;
		if (g_loop.us_acc > g_loop.us_max) g_loop.us_max = g_loop.us_acc;
		g_loop.us_sum += g_loop.us_acc; g_loop.us_n++;
	}
	g_loop.iter++;
}

static int loop_stop(void) { return g_loop.active && g_loop.fail >= g_loop.max_fail; }

static void loop_summary(const char *name)
{
	printf("{\"test\":\"loop_summary\",\"name\":\"%s\",\"requested\":%d,\"ran\":%d,\"reset_ctx\":%s,\"pass\":%d,\"fail\":%d,"
	       "\"first_fail_iter\":%d,\"us_min\":%.1f,\"us_mean\":%.1f,\"us_max\":%.1f}\n", name, g_loop.n, g_loop.iter,
	       g_loop.reset ? "true" : "false", g_loop.pass, g_loop.fail, g_loop.first_fail, g_loop.us_min,
	       g_loop.us_n ? g_loop.us_sum / g_loop.us_n : 0.0, g_loop.us_max);
	fflush(stdout);
}

/* run(argc, argv, inner): performs setup, `inner` bracketed body iterations, teardown. */
static int loop_main(const char *name, int argc, char **argv, int (*run)(int, char **, int))
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--loop") && i + 1 < argc) g_loop.n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--loop-reset-ctx")) g_loop.reset = 1;
		else if (!strcmp(argv[i], "--loop-max-fail") && i + 1 < argc) g_loop.max_fail = atoi(argv[++i]);
	}
	if (g_loop.n <= 1 && !g_loop.reset) return run(argc, argv, 1);
	if (g_loop.n < 1) g_loop.n = 1;
	g_loop.active = 1;
	int rc = 0;
	if (g_loop.reset) {
		for (int i = 0; i < g_loop.n && !loop_stop(); i++) {
			loop_iter_begin();
			int r = run(argc, argv, 1);
			loop_iter_end(r == 0);
			if (r) rc = 1;
		}
	} else {
		rc = run(argc, argv, g_loop.n);
	}
	loop_summary(name);
	return (rc || g_loop.fail) ? 1 : 0;
}
#endif
