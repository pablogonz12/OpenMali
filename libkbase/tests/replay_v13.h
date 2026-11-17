// SPDX-License-Identifier: MIT
/* t_triangle_v13 --replay DIR: load a PanVK capture (tools-replay/mkreplay.py) into our BOs, relocate every pointer, submit the captured
 * vertex + fragment rings through libkbase queues and count the pixels.  Included by t_triangle_v13.c after shaders_v13.h / loop.h.
 * Swaps: --swap-shaders (our binaries in PanVK's SPDs), --swap-spd (+ our SPD words), --swap-heap-desc, --no-sync (skip MEM_SYNC),
 *        --plain-flags (allocate with our library's flags 0x17/0x0f instead of the captured ones), --exec-flags HEX. */
#ifndef REPLAY_V13_H
#define REPLAY_V13_H

struct rp_reg { uint64_t old_va, len, new_va, flags, gpu_off; char file[96]; int exec, zero; uint8_t *cpu; };
struct rp_map { uint64_t old_va, len, new_va; };
struct rp_opts { bool swap_shaders, swap_spd, nosync, plain, swap_heap_desc, only_last; uint64_t exec_flags; unsigned timeout_ms; };

static struct rp_map rp_m[64];
static int rp_nm;

static uint64_t rp_reloc(uint64_t x)
{
	uint64_t p = x & 0xffffffffffffull;
	for (int i = 0; i < rp_nm; i++)
		if (p >= rp_m[i].old_va && p < rp_m[i].old_va + rp_m[i].len) return x - rp_m[i].old_va + rp_m[i].new_va;
	return x;
}
static bool rp_in(uint64_t x)
{
	uint64_t p = x & 0xffffffffffffull;
	for (int i = 0; i < rp_nm; i++)
		if (p >= rp_m[i].old_va && p < rp_m[i].old_va + rp_m[i].len) return true;
	return false;
}

/* qword relocation (data blobs and MOVE48 immediates); returns the number of qwords changed */
static unsigned rp_reloc_qwords(uint64_t *q, size_t n)
{
	unsigned c = 0;
	for (size_t i = 0; i < n; i++) {
		if (!q[i]) continue;
		uint64_t y = rp_reloc(q[i]);
		if (y != q[i]) { q[i] = y; c++; }
	}
	return c;
}
/* MOVE32 pairs (even reg lo, next reg hi) that assemble a pointer: FAU/other 64-bit regs set by two MOVE32 */
static unsigned rp_reloc_move32(uint64_t *q, size_t n)
{
	unsigned c = 0;
	long lo_idx[256];
	for (int i = 0; i < 256; i++) lo_idx[i] = -1;
	for (size_t i = 0; i < n; i++) {
		unsigned op = (unsigned)(q[i] >> 56), reg = (unsigned)((q[i] >> 48) & 255);
		if (op != 2) { if (op != 3 && op != 1) { /* keep pairs across other ops */ } continue; }
		if (!(reg & 1)) { lo_idx[reg] = (long)i; continue; }
		long li = lo_idx[reg - 1];
		if (li < 0) continue;
		uint64_t lo = q[li] & 0xffffffffull, hi = q[i] & 0xffffffffull;
		uint64_t v = (hi << 32) | lo;
		if (!rp_in(v)) continue;
		uint64_t nv = rp_reloc(v);
		q[li] = (q[li] & ~0xffffffffull) | (nv & 0xffffffffull);
		q[i] = (q[i] & ~0xffffffffull) | (nv >> 32);
		c++;
	}
	return c;
}

static uint8_t *rp_file(const char *dir, const char *fn, size_t *len)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", dir, fn);
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc((size_t)n + 8);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
	fclose(f);
	*len = (size_t)n;
	return b;
}

struct rp_mem_sync { uint64_t handle, user_addr, size; uint8_t type, pad[7]; };
#define RP_IOCTL_MEM_SYNC _IOW(KBASE_IOCTL_TYPE, 15, struct rp_mem_sync)

static int rp_run(const char *dir, const struct rp_opts *ro)
{
	struct kb_ctx *ctx;
	struct rp_reg regs[40];
	int nreg = 0;
	char line[512];
	char rname[2][16]; int rn = 0; unsigned rnq[2] = { 0, 0 }; char rfile[2][96];
	uint64_t cs_va[16], cs_len[16]; int ncs = 0;
	uint64_t rt_va = 0, rt_len = 0, sync_va = 0, clear = 0x80402010, spd_va[2] = { 0, 0 }, sh_va[2] = { 0, 0 };
	uint64_t hctx_old = 0, hctx_len = 0, hch_old = 0, hch_len = 0;
	char path[600];
	snprintf(path, sizeof path, "%s/manifest.txt", dir);
	FILE *mf = fopen(path, "r");
	if (!mf) { jb("replay"); jok(0); jstr("error", "no manifest"); jend(); return 1; }
	while (fgets(line, sizeof line, mf)) {
		char k[32], a[96]; unsigned long long v1, v2, v3, v4;
		if (sscanf(line, "%31s", k) != 1) continue;
		if (!strcmp(k, "region") && sscanf(line, "%*s %llx %llx %llx %95s", &v1, &v2, &v3, a) == 4 && nreg < 40) {
			struct rp_reg *r = &regs[nreg++]; memset(r, 0, sizeof *r); r->old_va = v1; r->len = v2; r->flags = v3; snprintf(r->file, sizeof r->file, "%s", a);
		} else if (!strcmp(k, "exec") && sscanf(line, "%*s %llx %llx %llx %95s %llx", &v1, &v2, &v3, a, &v4) == 5 && nreg < 40) {
			struct rp_reg *r = &regs[nreg++]; memset(r, 0, sizeof *r); r->old_va = v1; r->len = v2; r->flags = v3; r->exec = 1; r->gpu_off = v4; snprintf(r->file, sizeof r->file, "%s", a);
		} else if (!strcmp(k, "zeroregion") && sscanf(line, "%*s %llx %llx %llx", &v1, &v2, &v3) == 3 && nreg < 40) {
			struct rp_reg *r = &regs[nreg++]; memset(r, 0, sizeof *r); r->old_va = v1; r->len = v2; r->flags = v3; r->zero = 1;
		} else if (!strcmp(k, "ring") && rn < 2 && sscanf(line, "%*s %15s %u %95s", rname[rn], &rnq[rn], rfile[rn]) == 3) rn++;
		else if (!strcmp(k, "cs") && ncs < 16 && sscanf(line, "%*s %llx %llu", &v1, &v2) == 2) { cs_va[ncs] = v1; cs_len[ncs++] = v2; }
		else if (!strcmp(k, "heapctx") && sscanf(line, "%*s %llx %llx", &v1, &v2) == 2) { hctx_old = v1; hctx_len = v2; }
		else if (!strcmp(k, "heapchunk") && sscanf(line, "%*s %llx %llx", &v1, &v2) == 2) { hch_old = v1; hch_len = v2; }
		else if (!strcmp(k, "rt") && sscanf(line, "%*s %llx %llu", &v1, &v2) == 2) { rt_va = v1; rt_len = v2; }
		else if (!strcmp(k, "sync") && sscanf(line, "%*s %llx", &v1) == 1) sync_va = v1;
		else if (!strcmp(k, "clear") && sscanf(line, "%*s %llx", &v1) == 1) clear = v1;
		else if (!strcmp(k, "spd") && sscanf(line, "%*s %31s %llx", a, &v1) == 2) spd_va[a[0] == 'f'] = v1;
		else if (!strcmp(k, "shader") && sscanf(line, "%*s %31s %llx", a, &v1) == 2) sh_va[a[0] == 'f'] = v1;
	}
	fclose(mf);

	int r = kb_open(&ctx, NULL);
	jb("replay_open"); jok(r == 0); ji("rc", r); ji("regions", nreg); jbool("swap_shaders", ro->swap_shaders); jbool("swap_spd", ro->swap_spd);
	jbool("plain_flags", ro->plain); jbool("nosync", ro->nosync); jend();
	if (r) return 1;
	int fd = kb_fd(ctx);
	/* PanVK order: queue groups first (vertex group 0, fragment group 1), then the tiler heap, then the memory */
	struct kb_queue vq, fq;
	memset(&vq, 0, sizeof vq); memset(&fq, 0, sizeof fq);
	r = kb_queue_create(ctx, NULL, &vq);
	int r2 = kb_queue_create(ctx, NULL, &fq);
	struct kb_tiler_heap heap;
	memset(&heap, 0, sizeof heap);
	int r3 = kb_tiler_heap_create(ctx, 0x100000, 10, 400, &heap);
	jb("replay_setup"); jok(!r && !r2 && !r3); ji("vq", r); ji("fq", r2); ji("heap", r3); jx("heap_ctx_va", heap.ctx_va); jx("heap_first_chunk", heap.first_chunk_va);
	if (r || r2 || r3) { jstr("error", kb_last_error()); jend(); return 1; }
	jend();

	/* allocate every captured BO with the captured flags (or ours with --plain-flags) */
	for (int i = 0; i < nreg; i++) {
		struct rp_reg *g = &regs[i];
		union kbase_ioctl_mem_alloc_ex req;
		memset(&req, 0, sizeof req);
		uint64_t fl = g->flags;
		if (g->exec && ro->exec_flags) fl = ro->exec_flags;
		if (ro->plain) fl = g->exec ? 0x17 : (0xf | 0x1000ull * 0 | 0x20000ull * 0);
		req.in.va_pages = req.in.commit_pages = g->len / 4096;
		req.in.flags = fl;
		if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC_EX, &req)) { jb("replay_alloc"); jok(0); jx("va", g->old_va); jx("flags", fl); ji("errno", errno); jend(); return 1; }
		uint64_t handle = req.out.gpu_va;
		void *p = mmap(NULL, g->len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)handle);
		if (p == MAP_FAILED) { jb("replay_mmap"); jok(0); jx("va", g->old_va); ji("errno", errno); jend(); return 1; }
		g->cpu = p;
		g->new_va = (req.out.flags & BASE_MEM_SAME_VA) ? (uint64_t)(uintptr_t)p : handle;
		rp_m[rp_nm].old_va = g->exec ? g->gpu_off : g->old_va;
		rp_m[rp_nm].len = g->len; rp_m[rp_nm].new_va = g->new_va; rp_nm++;
		jb("replay_bo"); jx("old_va", rp_m[rp_nm - 1].old_va); jx("new_va", g->new_va); jx("flags_req", fl); jx("flags_out", req.out.flags); ji("len", (long long)g->len); jbool("exec", g->exec); jend();
	}
	if (hctx_len) { rp_m[rp_nm].old_va = hctx_old; rp_m[rp_nm].len = hctx_len; rp_m[rp_nm].new_va = heap.ctx_va; rp_nm++; }
	if (hch_len) { rp_m[rp_nm].old_va = hch_old; rp_m[rp_nm].len = hch_len; rp_m[rp_nm].new_va = heap.first_chunk_va; rp_nm++; }

	/* contents + relocation */
	unsigned nrel = 0, nrel32 = 0;
	struct rp_reg *rtreg = NULL, *syncreg = NULL, *execreg = NULL;
	for (int i = 0; i < nreg; i++) {
		struct rp_reg *g = &regs[i];
		if (!g->zero) {
			size_t n = 0;
			uint8_t *b = rp_file(dir, g->file, &n);
			if (!b || n != g->len) { jb("replay_file"); jok(0); jstr("file", g->file); jend(); return 1; }
			memcpy(g->cpu, b, n);
			free(b);
			if (!g->exec) nrel += rp_reloc_qwords((uint64_t *)g->cpu, g->len / 8);
		} else memset(g->cpu, 0, g->len);
		if (!g->exec && g->old_va == rt_va) rtreg = g;
		if (g->old_va == sync_va) syncreg = g;
		if (g->exec) execreg = g;
	}
	for (int c = 0; c < ncs; c++)
		for (int i = 0; i < nreg; i++) {
			struct rp_reg *g = &regs[i];
			if (!g->exec && cs_va[c] >= g->old_va && cs_va[c] + cs_len[c] <= g->old_va + g->len)
				nrel32 += rp_reloc_move32((uint64_t *)(g->cpu + (cs_va[c] - g->old_va)), cs_len[c] / 8);
		}
	if (syncreg) memset(syncreg->cpu, 0, 4096);   /* sync objects start from zero: the rings replay all three submissions */
	if (rtreg) memset(rtreg->cpu, 0, rt_len);

	/* shader swaps: place our binaries in the free tail of the exec BO and repoint the PanVK SPDs */
	if ((ro->swap_shaders || ro->swap_spd) && execreg) {
		uint64_t vs_new = execreg->new_va + 0x1000, fs_new = execreg->new_va + 0x1200;
		memcpy(execreg->cpu + 0x1000, vs_bin, VS_SIZE);
		memcpy(execreg->cpu + 0x1200, fs_const_bin, FS_CONST_SIZE);
		if (getenv("RP_BREAK")) memset(execreg->cpu + 0x1200, 0, FS_CONST_SIZE);   /* sanity: prove the swapped FS is what runs */
		for (int s = 0; s < 2; s++) {
			uint64_t spd_old = spd_va[s];
			for (int i = 0; i < nreg; i++) {
				struct rp_reg *g = &regs[i];
				if (g->exec || spd_old < g->old_va || spd_old + 32 > g->old_va + g->len) continue;
				uint32_t *d = (uint32_t *)(g->cpu + (spd_old - g->old_va));
				uint64_t va = s ? fs_new : vs_new;
				if (ro->swap_spd) {
					if (s) kb_v13_spd(d, 2, va, FS_CONST_WORK_REG_COUNT, FS_CONST_PRELOAD, 0);
					else kb_v13_spd(d, 3, va, VS_WORK_REG_COUNT, VS_PRELOAD, 0);
				} else { d[2] = (uint32_t)va; d[3] = (uint32_t)(va >> 32); }
				jb("replay_spd"); jstr("stage", s ? "fs" : "vs"); jx("spd_new_va", g->new_va + (spd_old - g->old_va)); jx("shader_va", va);
				for (int w = 0; w < 4; w++) { char kk[8]; snprintf(kk, sizeof kk, "w%d", w); jx(kk, d[w]); }
				jend();
			}
		}
	}
	(void)sh_va;
	if (ro->swap_shaders && execreg) {
		/* our binaries need our FAU layout (imm at word 8, 13/11 words), our varying size (32 B: no secondary shader) and tiler flags */
		struct rp_reg *vb = NULL; uint64_t *chunk = NULL; uint64_t vfau_old = 0, ffau_old = 0;
		for (int c = 0; c < ncs; c++) if (cs_len[c] == 792)
			for (int i = 0; i < nreg; i++)
				if (!regs[i].exec && cs_va[c] >= regs[i].old_va && cs_va[c] + cs_len[c] <= regs[i].old_va + regs[i].len) chunk = (uint64_t *)(regs[i].cpu + (cs_va[c] - regs[i].old_va));
		for (int i = 0; i < nreg; i++) if (regs[i].old_va == 0x5fffe19000ull) vb = &regs[i];
		if (chunk && vb) {
			uint32_t *vf = (uint32_t *)(vb->cpu + 0x200), *ff = (uint32_t *)(vb->cpu + 0x300);
			uint64_t vf_va = vb->new_va + 0x200, ff_va = vb->new_va + 0x300;
			uint64_t pv = 0, pf = 0;
			for (int i = 0; i < 99; i++) {   /* PanVK FAU pointers (already relocated by the MOVE32 pass) */
				unsigned op = (unsigned)(chunk[i] >> 56), rg = (unsigned)((chunk[i] >> 48) & 255);
				if (op != 2) continue;
				if (rg == 8) pv = (pv & ~0xffffffffull) | (chunk[i] & 0xffffffffull);
				if (rg == 9) pv = (pv & 0xffffffffull) | ((chunk[i] & 0xffffffffull) << 32);
				if (rg == 10) pf = (pf & ~0xffffffffull) | (chunk[i] & 0xffffffffull);
				if (rg == 11) pf = (pf & 0xffffffffull) | ((chunk[i] & 0xffffffffull) << 32);
			}
			vfau_old = pv & 0xffffffffffffull; ffau_old = pf & 0xffffffffffffull;
			const uint32_t *pvw = NULL, *pfw = NULL;
			for (int i = 0; i < nreg; i++) {
				if (regs[i].exec) continue;
				if (vfau_old >= regs[i].new_va && vfau_old + 40 <= regs[i].new_va + regs[i].len) pvw = (uint32_t *)(regs[i].cpu + (vfau_old - regs[i].new_va));
				if (ffau_old >= regs[i].new_va && ffau_old + 40 <= regs[i].new_va + regs[i].len) pfw = (uint32_t *)(regs[i].cpu + (ffau_old - regs[i].new_va));
			}
			if (pvw && pfw) {
				memset(vf, 0, 64); memset(ff, 0, 64);
				memcpy(vf, pvw, 24); memcpy(ff, pfw, 32);                 /* PanVK viewport words 0..5 / FS blend words 0..7 */
				for (unsigned i = 0; i < VS_NIMM; i++) vf[vs_imm[i][0]] = vs_imm[i][1];
				for (unsigned i = 0; i < FS_CONST_NIMM; i++) ff[fs_const_imm[i][0]] = fs_const_imm[i][1];
				uint64_t nv = kb_draw_fau_ptr(vf_va, VS_FAU_COUNT), nf = kb_draw_fau_ptr(ff_va, FS_CONST_FAU_COUNT);
				for (int i = 0; i < 99; i++) {
					unsigned op = (unsigned)(chunk[i] >> 56), rg = (unsigned)((chunk[i] >> 48) & 255);
					if (op != 2) continue;
					uint64_t *imm = &chunk[i];
					if (rg == 8) *imm = (*imm & ~0xffffffffull) | (nv & 0xffffffffull);
					if (rg == 9) *imm = (*imm & ~0xffffffffull) | (nv >> 32);
					if (rg == 10) *imm = (*imm & ~0xffffffffull) | (nf & 0xffffffffull);
					if (rg == 11) *imm = (*imm & ~0xffffffffull) | (nf >> 32);
					if (rg == KB13_IDVS_VARY_SIZE) *imm = (*imm & ~0xffffffffull) | VS_GENERIC_SIZE_B;
					if (rg == KB13_IDVS_TILER_FLAGS) *imm = (*imm & ~0xffffffffull) | kb_v13_prim_flags_triangles();
				}
				jb("replay_swapfau"); jok(1); jx("vfau_new", nv); jx("ffau_new", nf); jx("panvk_vfau", vfau_old); jx("panvk_ffau", ffau_old);
				jx("tiler_flags", kb_v13_prim_flags_triangles()); jend();
			} else { jb("replay_swapfau"); jok(0); jx("panvk_vfau", vfau_old); jx("panvk_ffau", ffau_old); jend(); }
		}
	}

	/* CPU->device sync of the written BOs (PanVK: MEM_SYNC after every CPU write to cached memory) */
	if (!ro->nosync)
		for (int i = 0; i < nreg; i++) {
			struct rp_mem_sync ms = { rp_m[i].new_va, (uint64_t)(uintptr_t)regs[i].cpu, regs[i].len, 1, { 0 } };
			if (ioctl(fd, RP_IOCTL_MEM_SYNC, &ms)) { jb("replay_sync"); jok(0); jx("va", regs[i].old_va); ji("errno", errno); jend(); }
		}

	/* rings */
	uint64_t *rings[2];
	for (int i = 0; i < rn; i++) {
		size_t n = 0;
		rings[i] = (uint64_t *)rp_file(dir, rfile[i], &n);
		if (!rings[i]) { jb("replay_ring"); jok(0); jstr("file", rfile[i]); jend(); return 1; }
		nrel += rp_reloc_qwords(rings[i], rnq[i]);
		nrel32 += rp_reloc_move32(rings[i], rnq[i]);
	}
	jb("replay_reloc"); jok(1); ji("qwords", nrel); ji("move32_pairs", nrel32); ji("maps", rp_nm); jend();

	uint64_t sync0 = syncreg ? ((volatile uint64_t *)syncreg->cpu)[0] : 0;
	(void)sync0;
	uint64_t t0 = now_ns();
	int rc_v = kb_queue_submit(&vq, rings[0], rnq[0], KB_KICK_IOCTL);
	int rc_f = kb_queue_submit(&fq, rings[1], rnq[1], KB_KICK_IOCTL);
	jb("replay_submit"); jok(!rc_v && !rc_f); ji("rc_v", rc_v); ji("rc_f", rc_f); jend();
	unsigned covered = 0, waited = 0, afbc_distinct = 0;
	volatile uint64_t *sy = syncreg ? (volatile uint64_t *)syncreg->cpu : NULL;
	while (sy && waited < ro->timeout_ms) {
		if (sy[8] >= 3) break;   /* fragment queue's counter (+0x40) after its three submissions */
		struct kb_event ev;
		while (kb_event_read(ctx, &ev, 0) > 0) {
			jb("notification"); jok(0); jstr("text", ev.text); ji("group", ev.group); jx("status", ev.status); jstr("exception", kb_exception_name(ev.status)); jbool("fatal", ev.fatal); jend();
		}
		usleep(1000); waited++;
	}
	uint64_t t1 = now_ns();
	struct kb_event ev;
	while (kb_event_read(ctx, &ev, 100) > 0) {
		jb("notification"); jok(0); jstr("text", ev.text); ji("group", ev.group); jx("status", ev.status); jstr("exception", kb_exception_name(ev.status)); jbool("fatal", ev.fatal); jend();
	}
	unsigned cleared = 0, other = 0; uint32_t first_other = 0;
	if (rtreg) {
		uint32_t *px = (uint32_t *)rtreg->cpu;
		for (size_t i = 0; i < rt_len / 4; i++) {
			if (px[i] == (uint32_t)clear) cleared++;
			else { other++; if (!first_other && px[i]) first_other = px[i]; }
		}
		covered = 0;
		for (size_t i = 0; i < rt_len / 4; i++) if (px[i] != (uint32_t)clear && px[i] != 0) covered++;
	}
	if (rtreg) {   /* the PanVK RT is AFBC: 16-byte headers first.  Histogram of distinct headers = solid blocks per colour; partial blocks have unique ones */
		uint32_t *w = (uint32_t *)rtreg->cpu;
		struct { uint32_t h[4]; unsigned n; } hh[16]; int nh = 0; unsigned nz = 0, hdr_blocks = 0, body_nz_pages = 0;
		for (unsigned b = 0; b < 512; b++) {
			uint32_t *h = w + b * 4;
			if (!(h[0] | h[1] | h[2] | h[3])) continue;
			nz++;
			int k;
			for (k = 0; k < nh; k++) if (!memcmp(hh[k].h, h, 16)) { hh[k].n++; break; }
			if (k == nh && nh < 16) { memcpy(hh[nh].h, h, 16); hh[nh].n = 1; nh++; }
		}
		hdr_blocks = nz; afbc_distinct = (unsigned)nh;
		for (unsigned pg = 1; pg < rt_len / 4096; pg++) { uint32_t *p = w + pg * 1024; for (int k = 0; k < 1024; k++) if (p[k]) { body_nz_pages++; break; } }
		jb("replay_afbc"); ji("nonzero_headers", hdr_blocks); ji("body_nonzero_pages", body_nz_pages); ji("distinct", nh);
		for (int k = 0; k < nh; k++) { char kk[16]; snprintf(kk, sizeof kk, "h%d", k); jk(kk); printf("[\"%08x\",\"%08x\",\"%08x\",\"%08x\",%u]", hh[k].h[0], hh[k].h[1], hh[k].h[2], hh[k].h[3], hh[k].n); }
		jend();
	}
	jb("replay_result"); jok(afbc_distinct >= 3); ji("afbc_distinct", afbc_distinct); ji("covered", covered); ji("cleared_words", cleared); ji("other_words", other); jx("first_other", first_other);
	jx("sync_vertex", sy ? sy[0] : 0); jx("sync_frag", sy ? sy[8] : 0); ji("us", (long long)((t1 - t0) / 1000)); ji("waited_ms", waited);
	jbool("vq_faulted", kb_queue_faulted(&vq)); jbool("fq_faulted", kb_queue_faulted(&fq));
	jx("vq_extract", kb_queue_extract(&vq)); jx("vq_insert", vq.insert); jx("fq_extract", kb_queue_extract(&fq)); jx("fq_insert", fq.insert); jend();
	for (int i = 0; i < nreg; i++) {   /* which BOs did the GPU write?  (compare with the relocated image we uploaded) */
		struct rp_reg *g = &regs[i];
		if (g->exec) continue;
		unsigned nz = 0, nonclear = 0; uint32_t *w = (uint32_t *)g->cpu;
		for (size_t k = 0; k < g->len / 4; k++) { if (w[k]) nz++; if (w[k] == (uint32_t)clear) nonclear++; }
		if (nonclear) { jb("replay_region"); jx("old_va", g->old_va); jx("new_va", g->new_va); ji("nonzero_words", nz); ji("clear_words", nonclear); jend(); }
	}
	if (execreg) {
		jb("replay_exec_head"); for (int w = 0; w < 8; w++) { char kk[8]; snprintf(kk, sizeof kk, "w%d", w); jx(kk, ((uint32_t *)execreg->cpu)[w]); } jend();
	}
	int ok = afbc_distinct >= 3;
	kb_queue_destroy(&vq); kb_queue_destroy(&fq);
	kb_tiler_heap_destroy(&heap);
	kb_close(ctx);
	return ok ? 0 : 1;
}
#endif
