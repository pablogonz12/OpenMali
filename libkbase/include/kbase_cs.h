/* SPDX-License-Identifier: MIT */
/*
 * libkbase: Mali CSF (Valhall v10/v11, arch 10-11) command-stream instruction encoders.
 *
 * Every instruction is one 64-bit word; bits 63:56 are the opcode. Field positions
 * are taken from the public Mesa genxml description (src/panfrost/genxml/v10.xml,
 * structs "CS NOP", "CS MOVE48", "CS WAIT", "CS STORE_MULTIPLE", "CS SYNC_ADD64", ...,
 * MIT) and checked against how Mesa's cs_builder.h sequences them. The encoders
 * below are our own code. Layouts used (bit ranges inclusive):
 *   NOP       op 0  | 55:48 metadata type, 47:0 payload
 *   MOVE48    op 1  | 55:48 dst reg, 47:0 imm            (writes dst, dst+1 as 64 bit)
 *   MOVE32    op 2  | 55:48 dst reg, 31:0 imm
 *   WAIT      op 3  | 31:16 scoreboard-slot mask
 *   ADD_IMM64 op 17 | 55:48 dst, 47:40 src, 31:0 signed imm
 *   STORE_MULTIPLE op 21 | 55:48 first data reg, 47:40 address reg pair, 31:16 reg mask, 15:0 signed byte offset
 *   SYNC_SET32 op 38 / SYNC_ADD64 op 51 / SYNC_SET64 op 52 |
 *       51:48 signal slot, 47:40 address reg pair, 39:32 data reg (pair for 64), 31:16 wait mask, 2:1 scope, 0 error-propagate
 * Scoreboard slot 0 tracks load/store (a WAIT on mask 0x1 makes earlier STOREs visible).
 * A SYNC op with wait mask 0 executes synchronously (in order, without waiting for prior asynchronous work).
 */
#ifndef KBASE_CS_H
#define KBASE_CS_H

#include <stdbool.h>
#include <stdint.h>

#define KB_CS_OP_NOP        0
#define KB_CS_OP_MOVE48     1
#define KB_CS_OP_MOVE32     2
#define KB_CS_OP_WAIT       3
#define KB_CS_OP_RUN_FRAGMENT 7
#define KB_CS_OP_FINISH_FRAGMENT 11
#define KB_CS_OP_ADD_IMM64  17
#define KB_CS_OP_STORE_MULTIPLE 21
#define KB_CS_OP_SYNC_SET32 38
#define KB_CS_OP_SYNC_ADD64 51
#define KB_CS_OP_SYNC_SET64 52

#define KB_CS_SCOPE_SYSTEM 0
#define KB_CS_SCOPE_CSG    2
#define KB_CS_SLOT_LS      0   /* scoreboard slot used by load/store */

static inline uint64_t kb_cs_raw(unsigned op, uint64_t body) { return ((uint64_t)op << 56) | (body & 0x00ffffffffffffffull); }

static inline uint64_t kb_cs_nop(void) { return 0; }
static inline uint64_t kb_cs_move48(unsigned dst, uint64_t imm) { return kb_cs_raw(KB_CS_OP_MOVE48, ((uint64_t)dst << 48) | (imm & 0xffffffffffffull)); }
static inline uint64_t kb_cs_move32(unsigned dst, uint32_t imm) { return kb_cs_raw(KB_CS_OP_MOVE32, ((uint64_t)dst << 48) | imm); }
static inline uint64_t kb_cs_wait(unsigned slot_mask) { return kb_cs_raw(KB_CS_OP_WAIT, (uint64_t)(slot_mask & 0xffff) << 16); }
static inline uint64_t kb_cs_add_imm64(unsigned dst, unsigned src, int32_t imm)
{ return kb_cs_raw(KB_CS_OP_ADD_IMM64, ((uint64_t)dst << 48) | ((uint64_t)src << 40) | (uint32_t)imm); }
static inline uint64_t kb_cs_store_multiple(unsigned first_reg, unsigned addr_reg, unsigned reg_mask, int16_t byte_off)
{ return kb_cs_raw(KB_CS_OP_STORE_MULTIPLE, ((uint64_t)first_reg << 48) | ((uint64_t)addr_reg << 40) | ((uint64_t)(reg_mask & 0xffff) << 16) | (uint16_t)byte_off); }

static inline uint64_t kb_cs_sync(unsigned op, unsigned addr_reg, unsigned data_reg, unsigned scope, unsigned wait_mask, unsigned signal_slot)
{
	return kb_cs_raw(op, ((uint64_t)(signal_slot & 0xf) << 48) | ((uint64_t)addr_reg << 40) | ((uint64_t)data_reg << 32) |
			     ((uint64_t)(wait_mask & 0xffff) << 16) | ((uint64_t)(scope & 3) << 1));
}
static inline uint64_t kb_cs_sync_set32(unsigned addr_reg, unsigned data_reg, unsigned scope)
{ return kb_cs_sync(KB_CS_OP_SYNC_SET32, addr_reg, data_reg, scope, 0, 0); }
static inline uint64_t kb_cs_sync_add64(unsigned addr_reg, unsigned data_reg, unsigned scope)
{ return kb_cs_sync(KB_CS_OP_SYNC_ADD64, addr_reg, data_reg, scope, 0, 0); }
static inline uint64_t kb_cs_sync_set64(unsigned addr_reg, unsigned data_reg, unsigned scope)
{ return kb_cs_sync(KB_CS_OP_SYNC_SET64, addr_reg, data_reg, scope, 0, 0); }

/* Fragment (v10/v11; Mesa genxml "CS RUN_FRAGMENT", "CS FINISH_FRAGMENT", "CS SET_STATE_IMM32"):
 *   RUN_FRAGMENT op 7: bit 0 enable TEM, 7:4 tile order (0 = Z order), bit 32 progress increment.
 *   FINISH_FRAGMENT op 11: bit 0 incr fragment completed, 31:16 wait mask, 39:32 last / 47:40 first heap chunk reg, 51:48 signal slot.
 *   SET_STATE_IMM32 op 28: 31:0 value, 39:32 state (0 = scoreboard select for endpoint, 1 = other, 2 = deferred).
 * Staging registers for RUN_FRAGMENT: r40:r41 FBD pointer, r42 bbox min, r43 bbox max. */
#define KB_CS_OP_SET_STATE_IMM32 28
#define KB_CS_SB_SEL_ENDPOINT 0
#define KB_CS_SB_SEL_OTHER    1
#define KB_CS_SB_SEL_DEFERRED 2
/* v10 (Mali-G610/G710/G510/G310, Mesa genxml v10.xml): no SET_STATE_IMM32. Scoreboards are selected with SET_SB_ENTRY (op 23:
 * 3:0 endpoint entry, 7:4 other entry, 8 entries in total), and endpoint/other cannot be set apart: "other" is pinned to the
 * load/store slot (cs_builder.h cs_select_endpoint_sb). kb_v10 is set once per process by the driver when the GPU is arch 10.
 * On v10 SB_SEL_OTHER / SB_SEL_DEFERRED become NOPs (only diagnostic MALI_CV knobs use them). */
#define KB_CS_OP_SET_SB_ENTRY 23
extern int kb_v10;
static inline uint64_t kb_cs_set_state_imm32(unsigned state, uint32_t value)
{
	if (kb_v10) {
		if (state != KB_CS_SB_SEL_ENDPOINT) return kb_cs_raw(0, 0);   /* NOP */
		return kb_cs_raw(KB_CS_OP_SET_SB_ENTRY, (value & 0xf) | ((uint64_t)KB_CS_SLOT_LS << 4));
	}
	return kb_cs_raw(KB_CS_OP_SET_STATE_IMM32, ((uint64_t)(state & 0xff) << 32) | value);
}
static inline uint64_t kb_cs_run_fragment(unsigned tile_order)
{ return kb_cs_raw(KB_CS_OP_RUN_FRAGMENT, (uint64_t)(tile_order & 0xf) << 4); }
static inline uint64_t kb_cs_finish_fragment(unsigned wait_mask, unsigned signal_slot)
{ return kb_cs_raw(KB_CS_OP_FINISH_FRAGMENT, ((uint64_t)(signal_slot & 0xf) << 48) | ((uint64_t)(wait_mask & 0xffff) << 16)); }

/* REQ_RESOURCE op 34 (genxml "CS REQ_RESOURCE"): acquire/release endpoint resources; bit 0 compute, 1 fragment, 2 tiler, 3 IDVS, 4 RT.
 * RUN_FRAGMENT does not issue until the fragment resource has been requested (observed: CS_EXTRACT stalls at it). */
#define KB_CS_OP_REQ_RESOURCE 34
#define KB_CS_RES_COMPUTE 1u
#define KB_CS_RES_FRAGMENT 2u
#define KB_CS_RES_TILER 4u
static inline uint64_t kb_cs_req_res(unsigned mask) { return kb_cs_raw(KB_CS_OP_REQ_RESOURCE, mask & 0x1f); }

/* FLUSH_CACHE2 op 36 (genxml "CS FLUSH_CACHE2"): 3:0 L2 mode, 7:4 LSC mode (0 none, 1 clean, 3 clean+invalidate), 11:8 other (2 = invalidate),
 * 31:16 wait mask, 47:40 register holding the latest flush id, 51:48 signal slot.
 * ALWAYS ASYNCHRONOUS (Mesa cs_instr_is_asynchronous): wait mask 0 only means "issue now", completion is reported on the signal
 * slot (here 0 = KB_CS_SLOT_LS). A flush that later instructions depend on MUST be followed by kb_cs_wait(1 << KB_CS_SLOT_LS),
 * or use kb_cs_emit_flush_wait(). An unwaited clean+invalidate races the next RUN_* (see INTERMITTENT.md). */
#define KB_CS_OP_FLUSH_CACHE2 36
static inline uint64_t kb_cs_flush_cache2(unsigned l2, unsigned lsc, unsigned other, unsigned flush_id_reg)
{ return kb_cs_raw(KB_CS_OP_FLUSH_CACHE2, ((uint64_t)flush_id_reg << 40) | ((other & 0xf) << 8) | ((lsc & 0xf) << 4) | (l2 & 0xf)); }

/* Geometry (v11; genxml "CS RUN_IDVS", "CS FINISH_TILING", "CS HEAP_SET", "CS HEAP_OPERATION", "CS LOAD_MULTIPLE").
 *   RUN_IDVS op 6: 31:0 flags override, bit 33 malloc enable, bit 38 fragment SRT select (REQUIRED: PanVK sets it, r4 = fragment resource table), 35..39 varying/fragment SRT/FAU/TSD select (0 = first set), 47:40 draw id.
 *   FINISH_TILING op 9.  HEAP_SET op 48: 47:40 register pair holding the heap context VA.
 *   HEAP_OPERATION op 49: 31:16 wait mask (0 = synchronous), 34:32 operation (0 vertex/tiler started, 1 vertex/tiler completed).
 *   LOAD_MULTIPLE op 20: 15:0 byte offset, 31:16 register mask, 47:40 address register pair, 55:48 first destination register.
 *   FINISH_FRAGMENT with heap chunks: bit 0 increment fragment completed, 39:32 last chunk reg pair, 47:40 first chunk reg pair.
 * IDVS staging registers: SRT 0/2/4/6, FAU 8/10/12/14, SPD 16/18/20/22, TSD 24..30, then 32.. as in KB_IDVS_* below. */
#define KB_CS_OP_RUN_IDVS 6
#define KB_CS_OP_FINISH_TILING 9
#define KB_CS_OP_LOAD_MULTIPLE 20
#define KB_CS_OP_HEAP_SET 48
#define KB_CS_OP_HEAP_OPERATION 49
#define KB_CS_RES_IDVS 8u
#define KB_CS_HEAP_VT_STARTED 0u
#define KB_CS_HEAP_VT_COMPLETED 1u
#define KB_IDVS_VERTEX_SRT 0
#define KB_IDVS_FRAGMENT_SRT 4
#define KB_IDVS_VERTEX_FAU 8
#define KB_IDVS_FRAGMENT_FAU 12
#define KB_IDVS_VERTEX_POS_SPD 16
#define KB_IDVS_VERTEX_VARY_SPD 18
#define KB_IDVS_FRAGMENT_SPD 20
#define KB_IDVS_TSD 24                /* TSD_0: Local Storage descriptor (panvk update_tls) */
#define KB_IDVS_GLOBAL_ATTR_OFFSET 32
#define KB_IDVS_INDEX_COUNT 33
#define KB_IDVS_INSTANCE_COUNT 34
#define KB_IDVS_INDEX_OFFSET 35
#define KB_IDVS_VERTEX_OFFSET 36
#define KB_IDVS_INSTANCE_OFFSET 37
#define KB_IDVS_DCD2 38
#define KB_IDVS_TILER_CTX 40
#define KB_IDVS_SCISSOR 42
#define KB_IDVS_LOW_DEPTH_CLAMP 44
#define KB_IDVS_HIGH_DEPTH_CLAMP 45
#define KB_IDVS_OQ 46
#define KB_IDVS_VARY_SIZE 48
#define KB_IDVS_BLEND_DESC 50
#define KB_IDVS_ZSD 52
#define KB_IDVS_TILER_FLAGS 56
#define KB_IDVS_DCD0 57
#define KB_IDVS_DCD1 58
#define KB_IDVS_PRIMITIVE_SIZE 60
#define KB_IDVS_TILER_FLAGS2 61
static inline uint64_t kb_cs_run_idvs(void) { return kb_cs_raw(KB_CS_OP_RUN_IDVS, (1ull << 33) | (1ull << 38)); }
static inline uint64_t kb_cs_finish_tiling(void) { return kb_cs_raw(KB_CS_OP_FINISH_TILING, 0); }
static inline uint64_t kb_cs_heap_set(unsigned addr_reg) { return kb_cs_raw(KB_CS_OP_HEAP_SET, (uint64_t)addr_reg << 40); }
static inline uint64_t kb_cs_heap_operation(unsigned op) { return kb_cs_raw(KB_CS_OP_HEAP_OPERATION, (uint64_t)(op & 7) << 32); }
static inline uint64_t kb_cs_load_multiple(unsigned first_reg, unsigned addr_reg, unsigned reg_mask, int16_t byte_off)
{ return kb_cs_raw(KB_CS_OP_LOAD_MULTIPLE, ((uint64_t)first_reg << 48) | ((uint64_t)addr_reg << 40) | ((uint64_t)(reg_mask & 0xffff) << 16) | (uint16_t)byte_off); }
static inline uint64_t kb_cs_finish_fragment_heap(bool incr, unsigned first_chunk_reg, unsigned last_chunk_reg)
{ return kb_cs_raw(KB_CS_OP_FINISH_FRAGMENT, (incr ? 1u : 0u) | ((uint64_t)last_chunk_reg << 32) | ((uint64_t)first_chunk_reg << 40)); }
/* The same, deferred: waits the scoreboard slots in wait_mask and signals signal_slot (bits 51:48). A FINISH_FRAGMENT with
 * wait mask 0 is a synchronous instruction and the packer drops its signal slot (Mesa cs_apply_async, genxml/cs_builder.h:866),
 * so nothing can be ordered after the chunk release it performs. PanVK chains successive FINISH_FRAGMENTs through an
 * alternating scoreboard slot exactly for this reason: "each iteration signals the next one, so the next FINISH_FRAGMENT
 * cannot start before the previous one completed (required to guarantee that used heap chunks won't be released
 * prematurely)" (panfrost/vulkan/csf/panvk_vX_cmd_draw.c:4633-4650, cs_iter_sb_update). */
static inline uint64_t kb_cs_finish_fragment_heap_signal(bool incr, unsigned first_chunk_reg, unsigned last_chunk_reg,
                                                        unsigned signal_slot, unsigned wait_mask)
{
	return kb_cs_raw(KB_CS_OP_FINISH_FRAGMENT, (incr ? 1u : 0u) | ((uint64_t)(signal_slot & 0xf) << 48) |
			 ((uint64_t)(wait_mask & 0xffff) << 16) | ((uint64_t)last_chunk_reg << 32) | ((uint64_t)first_chunk_reg << 40));
}

/* A deliberately undefined opcode (opcodes above 53 are unassigned in v10/v11); the CSF firmware reports CS_INVALID_INSTRUCTION. */
static inline uint64_t kb_cs_invalid(void) { return kb_cs_raw(0xfe, 0x1234); }

/* Tiny fixed-capacity builder. */
struct kb_cs_buf { uint64_t ins[256]; unsigned n; };
static inline void kb_cs_emit(struct kb_cs_buf *b, uint64_t i) { if (b->n < 256) b->ins[b->n++] = i; }
/* FLUSH_CACHE2 + WAIT on its signal slot: the flush has completed before the next instruction runs (PanVK: cs_flush_caches + cs_wait_slot). */
static inline void kb_cs_emit_flush_wait(struct kb_cs_buf *b, unsigned l2, unsigned lsc, unsigned other, unsigned flush_id_reg)
{
	kb_cs_emit(b, kb_cs_flush_cache2(l2, lsc, other, flush_id_reg));
	kb_cs_emit(b, kb_cs_wait(1u << KB_CS_SLOT_LS));
}

#endif
