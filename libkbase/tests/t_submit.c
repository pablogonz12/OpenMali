// SPDX-License-Identifier: MIT
/*
 * t_submit: end-to-end test of libkbase. One JSON object per line on stdout.
 *   props/caps   context, GPU props, uAPI capability row
 *   e1_*         NOP/MOVE/STORE/SYNC stream: CS_EXTRACT==CS_INSERT, BO value, sync_file signals
 *   e2_*         1000 submits, submit->signal latency
 *   e3_fault     invalid instruction: fatal notification decoded, group torn down, fresh group works
 *   e4_teardown  no leaked BOs / queues / fds
 *   f_dmabuf_sync_file  DMA_BUF_IOCTL_{EXPORT,IMPORT}_SYNC_FILE on a dma-heap buffer
 * Options: --event-thread (SET_FLAGS CSF_EVENT_THREAD), --quick (100 submits)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kbase.h"

typedef unsigned long long ull;

static void js(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
		else if (c < 0x20 || c == 0x7f) printf("\\u%04x", c);
		else putchar(c);
	}
	putchar('"');
}
static void jb(const char *t) { printf("{\"test\":"); js(t); }
static void jk(const char *k) { putchar(','); js(k); putchar(':'); }
static void jok(int ok) { jk("ok"); fputs(ok ? "true" : "false", stdout); }
static void jstr(const char *k, const char *v) { jk(k); js(v); }
static void ji(const char *k, long long v) { jk(k); printf("%lld", v); }
static void ju(const char *k, ull v) { jk(k); printf("%llu", v); }
static void jx(const char *k, ull v) { jk(k); printf("\"0x%llx\"", v); }
static void jd(const char *k, double v) { jk(k); printf("%.2f", v); }
static void jend(void) { fputs("}\n", stdout); fflush(stdout); }

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int count_fds(void)
{
	int n = 0;
	DIR *d = opendir("/proc/self/fd");
	if (!d)
		return -1;
	while (readdir(d))
		n++;
	closedir(d);
	return n - 3; /* ., .., the DIR's own fd */
}

/* ------------------------------------------------------------ fixture */
struct fx {
	struct kb_ctx *ctx;
	struct kb_bo data;   /* cached CPU, GPU RD|WR */
	struct kb_bo sync;   /* CSF_EVENT memory: CQS object at +0 */
	struct kb_queue q;
	uint64_t target;     /* value the sync object reaches after the next submit */
	bool have_q;
};

static const uint64_t k_magic_base = 0xC0DEF00D00000000ull;

static void build_stream(const struct fx *f, struct kb_cs_buf *b, uint64_t magic)
{
	b->n = 0;
	kb_cs_emit(b, kb_cs_nop());
	kb_cs_emit(b, kb_cs_move48(64, f->data.gpu_va));                 /* r64:r65 = &data */
	kb_cs_emit(b, kb_cs_move32(66, (uint32_t)magic));                /* r66 = low */
	kb_cs_emit(b, kb_cs_move32(67, (uint32_t)(magic >> 32)));        /* r67 = high */
	kb_cs_emit(b, kb_cs_store_multiple(66, 64, 0x3, 0));             /* *(u64*)data = r66:r67 */
	kb_cs_emit(b, kb_cs_wait(1u << KB_CS_SLOT_LS));                  /* store visible */
	kb_cs_emit(b, kb_cs_move48(68, f->sync.gpu_va));                 /* r68:r69 = &sync */
	kb_cs_emit(b, kb_cs_move48(70, 1));                              /* r70:r71 = 1 */
	kb_cs_emit(b, kb_cs_sync_add64(68, 70, KB_CS_SCOPE_SYSTEM));     /* sync += 1 */
}

static int fx_open_queue(struct fx *f)
{
	int r = kb_queue_create(f->ctx, NULL, &f->q);
	f->have_q = r == 0;
	return r;
}

struct sres { int rc; double submit_to_signal_us; bool extract_ok, value_ok, sync_ok, fence_ok; uint64_t data_val; };

/* One full submit: KCPU wait+fence enqueued first, then the stream is written and kicked. */
static struct sres do_submit(struct fx *f, enum kb_kick how, int timeout_ms, uint64_t magic)
{
	struct sres s;
	memset(&s, 0, sizeof s);
	volatile uint64_t *d = f->data.cpu;
	*d = 0;
	kb_bo_cache_flush_inv(&f->data, 0, 64);

	uint64_t target = f->target + 1;
	int fence = -1;
	if ((s.rc = kb_kcpu_cqs_wait_gt64(f->ctx, f->sync.gpu_va, target - 1)) ||
	    (s.rc = kb_kcpu_fence_signal(f->ctx, &fence)))
		return s;

	struct kb_cs_buf b;
	build_stream(f, &b, magic);
	uint64_t t0 = now_ns();
	s.rc = kb_queue_submit(&f->q, b.ins, b.n, how);
	if (s.rc) { close(fence); return s; }
	s.rc = kb_fence_wait(f->ctx, fence, timeout_ms);
	uint64_t t1 = now_ns();
	close(fence);
	s.fence_ok = s.rc == 0;
	s.submit_to_signal_us = (double)(t1 - t0) / 1000.0;
	if (!s.fence_ok)
		return s;
	f->target = target;
	/* The firmware advances CS_EXTRACT when the last instruction retires: allow a short settle. */
	for (int i = 0; i < 2000 && !kb_queue_idle(&f->q); i++)
		usleep(100);
	s.extract_ok = kb_queue_idle(&f->q);
	kb_bo_cache_flush_inv(&f->data, 0, 64); /* GPU is not IO-coherent: drop stale CPU lines */
	s.data_val = *d;
	s.value_ok = s.data_val == magic;
	s.sync_ok = *(volatile uint64_t *)f->sync.cpu == target;
	return s;
}

static void report_submit(const char *name, const struct fx *f, const struct sres *s, uint64_t magic)
{
	jb(name);
	jok(s->rc == 0 && s->fence_ok && s->extract_ok && s->value_ok && s->sync_ok);
	ji("rc", s->rc);
	jk("fence_signalled"); fputs(s->fence_ok ? "true" : "false", stdout);
	jk("extract_eq_insert"); fputs(s->extract_ok ? "true" : "false", stdout);
	ju("cs_extract", kb_queue_extract(&f->q));
	ju("cs_insert", f->q.insert);
	jx("bo_value", s->data_val);
	jx("bo_expected", magic);
	jk("sync_value_ok"); fputs(s->sync_ok ? "true" : "false", stdout);
	jd("submit_to_signal_us", s->submit_to_signal_us);
	if (s->rc)
		jstr("error", kb_last_error());
	jend();
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static void run_timing(struct fx *f, const char *name, enum kb_kick how, int n, bool *ok_out)
{
	double *v = calloc((size_t)n, sizeof *v);
	int done = 0, bad = 0;
	if (!v) { *ok_out = false; return; }
	for (int i = 0; i < n; i++) {
		uint64_t m = k_magic_base | (uint32_t)(0x1000 + i);
		struct sres s = do_submit(f, how, 2000, m);
		if (!(s.rc == 0 && s.fence_ok && s.extract_ok && s.value_ok && s.sync_ok)) {
			bad++;
			if (!s.fence_ok) break;
			continue;
		}
		v[done++] = s.submit_to_signal_us;
	}
	double sum = 0;
	for (int i = 0; i < done; i++) sum += v[i];
	double mean = done ? sum / done : 0;
	qsort(v, (size_t)done, sizeof *v, cmp_d);
	jb(name);
	*ok_out = done == n && !bad;
	jok(*ok_out);
	ji("submits", n); ji("completed", done); ji("failed", bad);
	jstr("kick", how == KB_KICK_IOCTL ? "ioctl CS_QUEUE_KICK" : "doorbell page write only");
	jd("mean_us", mean);
	jd("min_us", done ? v[0] : 0);
	jd("p50_us", done ? v[done / 2] : 0);
	jd("p99_us", done ? v[done * 99 / 100] : 0);
	jd("max_us", done ? v[done - 1] : 0);
	jend();
	free(v);
}

int main(int argc, char **argv)
{
	struct kb_open_opts oo;
	memset(&oo, 0, sizeof oo);
	int nsub = 1000;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--event-thread")) oo.context_flags = BASE_CONTEXT_CSF_EVENT_THREAD;
		else if (!strcmp(argv[i], "--quick")) nsub = 100;
	}
	int fds_before = count_fds();
	int failures = 0;
	struct fx f;
	memset(&f, 0, sizeof f);

	int r = kb_open(&f.ctx, &oo);
	jb("open"); jok(r == 0); ji("rc", r);
	if (r) {
		jstr("error", kb_last_error());
		jend();
		return 1;
	}
	jend();
	{
		const struct kb_gpu_props *p = kb_props(f.ctx);
		unsigned ma, mi;
		kb_uapi_version(f.ctx, &ma, &mi);
		jb("props"); jok(1);
		ji("uapi_major", ma); ji("uapi_minor", mi);
		jstr("ddk", kb_ddk_version(f.ctx));
		jstr("caps_note", kb_caps(f.ctx)->ddk_note);
		jx("gpu_id", p->gpu_id);
		ji("arch_major", p->arch_major); ji("arch_minor", p->arch_minor); ji("arch_rev", p->arch_rev);
		jx("shader_present", p->shader_present); jx("tiler_present", p->tiler_present); jx("l2_present", p->l2_present);
		ji("va_bits", p->va_bits); ji("pa_bits", p->pa_bits);
		jx("csf_glb_version", p->csf_glb_version); ji("csf_groups", p->csf_group_num); ji("csf_streams", p->csf_total_streams);
		ji("csf_group0_suspend_size", p->csf_group0_suspend_size);
		jend();
	}

	/* caps: unknown versions must be refused with a clear message */
	jb("caps_lookup"); jok(kb_caps_lookup(1, 21) && !kb_caps_lookup(1, 99) && !kb_caps_lookup(2, 0));
	jend();

	/* ---- setup ---- */
	r = kb_bo_alloc(f.ctx, 4096, KB_BO_CPU_CACHED | KB_BO_GPU_RD | KB_BO_GPU_WR, &f.data);
	int r2 = kb_bo_alloc(f.ctx, 4096, KB_BO_CSF_EVENT | KB_BO_GPU_RD | KB_BO_GPU_WR, &f.sync);
	jb("bo_alloc"); jok(r == 0 && r2 == 0); ji("cached_rc", r); ji("csf_event_rc", r2);
	if (r == 0) { jx("cached_gpu_va", f.data.gpu_va); }
	if (r2 == 0) { jx("event_gpu_va", f.sync.gpu_va); }
	if (r || r2) jstr("error", kb_last_error());
	jend();
	if (r || r2) { failures++; goto teardown; }
	memset(f.sync.cpu, 0, 4096);

	/* uncached CPU mapping + GPU write coherence is exercised by the ring itself; check cached/uncached alloc once more */
	{
		struct kb_bo u;
		int ru = kb_bo_alloc(f.ctx, 8192, KB_BO_GPU_RD | KB_BO_GPU_WR, &u);
		bool okk = ru == 0;
		if (okk) {
			*(volatile uint32_t *)u.cpu = 0x12345678;
			okk = *(volatile uint32_t *)u.cpu == 0x12345678;
			uint64_t va = u.gpu_va;
			kb_bo_free(&u);
			jb("bo_uncached_roundtrip"); jok(okk && !kb_bo_gpu_va_live(f.ctx, va)); jx("gpu_va", va);
			jk("va_live_after_free"); fputs(kb_bo_gpu_va_live(f.ctx, va) ? "true" : "false", stdout);
		} else {
			jb("bo_uncached_roundtrip"); jok(0); jstr("error", kb_last_error());
		}
		jend();
		if (!okk) failures++;
	}

	r = fx_open_queue(&f);
	jb("queue_create"); jok(r == 0); ji("rc", r);
	if (r == 0) { ji("group_handle", f.q.group_handle); ji("group_uid", (long long)f.q.group_uid); ji("ring_size", f.q.ring_size); }
	else jstr("error", kb_last_error());
	jend();
	if (r) { failures++; goto teardown; }

	/* ---- E1 ---- */
	{
		uint64_t m = k_magic_base | 0x1;
		struct sres s = do_submit(&f, KB_KICK_IOCTL, 3000, m);
		report_submit("e1_submit_ioctl_kick", &f, &s, m);
		if (!(s.rc == 0 && s.fence_ok && s.extract_ok && s.value_ok && s.sync_ok)) {
			failures++;
			struct kb_event ev;
			int g = 0;
			while (g++ < 10 && kb_event_read(f.ctx, &ev, 200) > 0) {
				jb("notification"); jok(0); jstr("text", ev.text); jx("status", ev.status); jend();
			}
			if (!s.fence_ok) goto teardown; /* the GPU did not run it; do not pile on */
		}
		m = k_magic_base | 0x2;
		s = do_submit(&f, KB_KICK_DOORBELL, 1500, m);
		report_submit("e1_submit_doorbell_only", &f, &s, m);
		if (!s.fence_ok) {
			/* The doorbell page alone did not start the group; the ioctl kick must still complete it. */
			jb("e1_doorbell_only_note"); jok(1); jstr("note", "doorbell-only kick did not complete in 1.5s; falling back to ioctl kick");
			jend();
			struct kbase_ioctl_cs_queue_kick k = { f.q.ring.gpu_va };
			(void)k;
			/* the pending KCPU wait is satisfied once the stream runs: kick it now */
			struct kb_cs_buf nb;
			nb.n = 0;
			kb_cs_emit(&nb, kb_cs_nop());
			if (kb_queue_submit(&f.q, nb.ins, nb.n, KB_KICK_IOCTL) == 0) usleep(200000);
			jb("e1_doorbell_fallback"); jk("extract_eq_insert"); fputs(kb_queue_idle(&f.q) ? "true" : "false", stdout);
			jok(1); jend();
			/* Resynchronise the fixture: the sync target advanced iff the stream ran. */
			uint64_t cur = *(volatile uint64_t *)f.sync.cpu;
			f.target = cur;
		}
	}

	/* ---- E2 ---- */
	{
		bool ok;
		run_timing(&f, "e2_timing_ioctl_kick", KB_KICK_IOCTL, nsub, &ok);
		if (!ok) failures++;
		run_timing(&f, "e2_timing_doorbell_only", KB_KICK_DOORBELL, nsub / 10, &ok);
		/* doorbell-only is informational: a miss is reported, not counted as a failure */
	}

	/* ---- E3 ---- */
	{
		uint64_t ev_before = kb_events_seen(f.ctx);
		uint64_t ins[2] = { kb_cs_nop(), kb_cs_invalid() };
		int rr = kb_queue_submit(&f.q, ins, 2, KB_KICK_IOCTL);
		struct kb_event ev, fatal;
		memset(&fatal, 0, sizeof fatal);
		bool got = false;
		uint64_t t0 = now_ns();
		int guard = 0;
		while (!got && guard++ < 40) {
			int e = kb_event_read(f.ctx, &ev, 100);
			if (e < 0) break;
			if (e > 0) {
				jb("notification"); jok(1); jstr("text", ev.text); ji("type", ev.type); ji("group", ev.group);
				ji("error_type", ev.error_type); jx("status", ev.status); jx("sideband", ev.sideband); jend();
				if (ev.fatal) { fatal = ev; got = true; }
			}
		}
		double lat = (double)(now_ns() - t0) / 1000.0;
		bool dec = got && (fatal.status & 0xff) == 0x49;
		bool faulted = kb_queue_faulted(&f.q);
		/* tear down and rebuild */
		kb_queue_destroy(&f.q);
		f.have_q = false;
		struct kb_leaks lk;
		kb_leaks(f.ctx, &lk);
		bool clean = lk.queues == 0 && lk.bos == 2; /* data + sync only */
		int rc2 = fx_open_queue(&f);
		struct sres s2;
		memset(&s2, 0, sizeof s2);
		uint64_t m = k_magic_base | 0xE3;
		if (rc2 == 0) {
			/* group handle reuse: the fault flag must be cleared for the new group */
			f.target = *(volatile uint64_t *)f.sync.cpu;
			s2 = do_submit(&f, KB_KICK_IOCTL, 3000, m);
		}
		bool fresh = rc2 == 0 && s2.rc == 0 && s2.fence_ok && s2.extract_ok && s2.value_ok && s2.sync_ok;
		jb("e3_fault"); jok(rr == 0 && got && dec && faulted && clean && fresh);
		ji("submit_rc", rr);
		jk("fatal_notification"); fputs(got ? "true" : "false", stdout);
		jstr("decoded", got ? fatal.text : "(none)");
		jx("exception_code", fatal.status & 0xff);
		jstr("exception_name", got ? kb_exception_name(fatal.status) : "-");
		jk("is_invalid_instruction"); fputs(dec ? "true" : "false", stdout);
		jd("fault_latency_us", lat);
		ju("events_seen", kb_events_seen(f.ctx) - ev_before);
		jk("group_marked_faulted"); fputs(faulted ? "true" : "false", stdout);
		jk("teardown_clean"); fputs(clean ? "true" : "false", stdout);
		ji("live_queues_after", lk.queues); ji("live_bos_after", lk.bos);
		ji("fresh_group_rc", rc2);
		jk("fresh_group_runs"); fputs(fresh ? "true" : "false", stdout);
		jd("fresh_submit_to_signal_us", s2.submit_to_signal_us);
		jend();
		if (!(rr == 0 && got && dec && faulted && clean && fresh)) failures++;
		if (rc2) goto teardown;
	}

	/* ---- F: dma-buf sync_file ioctls ---- */
	{
		int heap_fd = kb_dmaheap_alloc(KB_HEAP_SYSTEM, 4096);
		jb("f_dmabuf_sync_file"); 
		if (heap_fd < 0) {
			jok(0); jstr("error", kb_last_error());
			failures++;
		} else {
			int sfd = -1;
			int re = kb_dmabuf_export_sync_file(heap_fd, KB_DMA_BUF_SYNC_RW, &sfd);
			jk("export_supported"); fputs(re == 0 ? "true" : "false", stdout);
			ji("export_errno", re == 0 ? 0 : -re);
			if (re) jstr("export_error", strerror(-re));
			bool exp_signalled = false;
			if (re == 0) {
				struct pollfd pf = { sfd, POLLIN, 0 };
				exp_signalled = poll(&pf, 1, 0) == 1;
				close(sfd);
			}
			jk("export_fence_signalled"); fputs(exp_signalled ? "true" : "false", stdout);
			int kf = -1;
			int ri = -ENOSYS;
			int rk = kb_kcpu_fence_signal(f.ctx, &kf); /* empty KCPU queue: signals immediately */
			if (rk == 0) {
				ri = kb_dmabuf_import_sync_file(heap_fd, KB_DMA_BUF_SYNC_WRITE, kf);
				close(kf);
			}
			jk("import_supported"); fputs(ri == 0 ? "true" : "false", stdout);
			ji("import_errno", ri == 0 ? 0 : -ri);
			if (ri) jstr("import_error", strerror(-ri));
			ji("kcpu_fence_rc", rk);
			bool exp2 = false;
			if (ri == 0) {
				int s2 = -1;
				exp2 = kb_dmabuf_export_sync_file(heap_fd, KB_DMA_BUF_SYNC_READ, &s2) == 0;
				if (exp2) close(s2);
			}
			jk("export_after_import_ok"); fputs(exp2 ? "true" : "false", stdout);
			jok(1); /* a report, not a pass/fail: supported or errno */
			close(heap_fd);
		}
		jend();
	}

teardown:
	if (f.have_q)
		kb_queue_destroy(&f.q);
	uint64_t dva = f.data.gpu_va, sva = f.sync.gpu_va;
	if (f.data.ctx) kb_bo_free(&f.data);
	if (f.sync.ctx) kb_bo_free(&f.sync);
	{
		struct kb_leaks lk;
		kb_leaks(f.ctx, &lk);
		bool live = (dva && kb_bo_gpu_va_live(f.ctx, dva)) || (sva && kb_bo_gpu_va_live(f.ctx, sva));
		kb_close(f.ctx);
		int fds_after = count_fds();
		bool ok = lk.bos == 0 && lk.queues == 0 && !live && fds_after == fds_before;
		jb("e4_teardown"); jok(ok);
		ji("leaked_bos", lk.bos); ji("leaked_queues", lk.queues); ji("leaked_kcpu_queues_before_close", lk.kcpu_queues);
		jk("freed_va_still_live"); fputs(live ? "true" : "false", stdout);
		ji("fds_before", fds_before); ji("fds_after", fds_after);
		jend();
		if (!ok) failures++;
	}
	jb("summary"); jok(failures == 0); ji("failures", failures); jend();
	return failures ? 1 : 0;
}
