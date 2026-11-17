/* SPDX-License-Identifier: MIT */
/*
 * kbase_cs_v13: CSF instruction encoders and IDVS staging registers for arch 13 (Mali-G925). Additive: includes kbase_cs.h and
 * defines only new names (KB13_*). Source: Mesa genxml/v13.xml ("CS RUN_IDVS2" L825, "CS Opcode" L544, IDVS SR L1451) and
 * cs_builder.h cs_run_idvs2 (L1559). RUN_IDVS (6) no longer exists on v13 (first draw CS_INVALID_INSTRUCTION on the Poco).
 * See V13-NEXT.md.
 */
#ifndef KBASE_CS_V13_H
#define KBASE_CS_V13_H

#include "kbase_cs.h"

#define KB13_CS_OP_RUN_IDVS2 12
#define KB13_CS_OP_ARITH_IMM64 17   /* v11 ADD_IMM64 became a sub-opcode of 17 (bits 39:32); kb_cs_add_imm64 lacks it: do not use on v13 */

/* IDVS shading mode (genxml "IDVS Shading Mode"): Early = 0 */
#define KB13_IDVS_MODE_EARLY 0u

/* RUN_IDVS2: flags_override [31:0], progress_increment 32, malloc_enable 33, draw_id_enable 34, vertex_shading_mode [36:35],
 * draw_id [47:40], stage [51:48] (Internal 0). */
static inline uint64_t kb13_cs_run_idvs2(uint32_t flags_override, unsigned mode)
{
	return kb_cs_raw(KB13_CS_OP_RUN_IDVS2, (uint64_t)flags_override | (1ull << 33) | ((uint64_t)(mode & 3) << 35));
}

/* IDVS staging registers, v13 (genxml v13.xml L1451). Pairs are 64-bit (use M32 twice for pointers with the FAU count in 63:56). */
#define KB13_IDVS_VERTEX_SRT 0
#define KB13_IDVS_FRAGMENT_SRT 2
#define KB13_IDVS_VERTEX_FAU 8
#define KB13_IDVS_FRAGMENT_FAU 10
#define KB13_IDVS_VERTEX_SPD 16
#define KB13_IDVS_FRAGMENT_SPD 18
#define KB13_IDVS_VERTEX_TSD 24
#define KB13_IDVS_FRAGMENT_TSD 26
#define KB13_IDVS_GLOBAL_ATTR_OFFSET 32
#define KB13_IDVS_INDEX_COUNT 33
#define KB13_IDVS_INSTANCE_COUNT 34
#define KB13_IDVS_INDEX_OFFSET 35
#define KB13_IDVS_VERTEX_OFFSET 36
#define KB13_IDVS_INSTANCE_OFFSET 37
#define KB13_IDVS_TILER_CTX 40
#define KB13_IDVS_SCISSOR 42
#define KB13_IDVS_VIEWPORT_HIGH 44   /* min x/y, max x/y u16 (words 0,1 of "Viewport") */
#define KB13_IDVS_VIEWPORT_LOW 46    /* min/max depth floats (words 2,3) */
#define KB13_IDVS_INDEX_BUFFER 48
#define KB13_IDVS_INDEX_BUFFER_SIZE 50
#define KB13_IDVS_VARY_SIZE 51
#define KB13_IDVS_ZSD 52
#define KB13_IDVS_BLEND_DESC 54
#define KB13_IDVS_TILER_FLAGS 56
#define KB13_IDVS_DCD0 57
#define KB13_IDVS_DCD1 58
#define KB13_IDVS_DCD2 59
#define KB13_IDVS_LINE_WIDTH 60
#define KB13_IDVS_TILER_FLAGS2 61
#define KB13_IDVS_OQ 64              /* 64-bit (r64:65): never use as scratch in a draw CS */

/* Scoreboard / ALU encoders (v13.xml "CS NEXT_SB_ENTRY" L911, "CS SET_STATE" L918, "CS LOGIC_OP32" L860; cs_builder.h cs_next_sb_entry,
 * cs_set_state, cs_bit_set32/cs_bit_clear32). Verified word-for-word against the PanVK Poco capture:
 * NEXT_SB_ENTRY r66 endpoint index = 0x1a42000100000000, LOGIC_OP32 r67=r67|(1<<r66) = 0x1343434200000017, SET_STATE wait r67 = 0x1b00430900000000. */
#define KB13_CS_OP_LOGIC_OP32 19
#define KB13_CS_OP_NEXT_SB_ENTRY 26
#define KB13_CS_OP_SET_STATE 27
#define KB13_LOGIC_AND_A_NB 2        /* bit clear: dst = a & ~b */
#define KB13_LOGIC_OR 7              /* bit set:   dst = a | b  */
#define KB13_SB_TYPE_ENDPOINT 1
#define KB13_SET_STATE_SB_SEL_ENDPOINT 0
#define KB13_SET_STATE_SB_SEL_OTHER 1
#define KB13_SET_STATE_SB_SEL_DEFERRED 2
#define KB13_SET_STATE_SB_MASK_STREAM 8
#define KB13_SET_STATE_SB_MASK_WAIT 9

/* NEXT_SB_ENTRY: dst = next scoreboard slot (format 0 = index, 1 = mask) of the given scoreboard type. */
static inline uint64_t kb13_cs_next_sb_entry(unsigned dst, unsigned sb_type, unsigned format)
{
	return kb_cs_raw(KB13_CS_OP_NEXT_SB_ENTRY, ((uint64_t)dst << 48) | ((uint64_t)(format & 1) << 36) | ((uint64_t)(sb_type & 0xf) << 32));
}
/* SET_STATE from a register: state = KB13_SET_STATE_*, src = 32-bit register. */
static inline uint64_t kb13_cs_set_state(unsigned state, unsigned src)
{
	return kb_cs_raw(KB13_CS_OP_SET_STATE, ((uint64_t)src << 40) | ((uint64_t)(state & 0xff) << 32));
}
/* LOGIC_OP32 with the "Index" flag (bit 4): source 1 is a bit INDEX (1 << r[src1]); index=0 uses the register value directly. */
static inline uint64_t kb13_cs_logic_op32(unsigned dst, unsigned src0, unsigned src1, unsigned mode, int index)
{
	return kb_cs_raw(KB13_CS_OP_LOGIC_OP32, ((uint64_t)dst << 48) | ((uint64_t)src0 << 40) | ((uint64_t)src1 << 32) | (mode & 0xfu) | (index ? 0x10u : 0u));
}
/* cs_bit_set32 / cs_bit_clear32 (index form) */
static inline uint64_t kb13_cs_bit_set32(unsigned dst, unsigned src, unsigned idx_reg) { return kb13_cs_logic_op32(dst, src, idx_reg, KB13_LOGIC_OR, 1); }
static inline uint64_t kb13_cs_bit_clear32(unsigned dst, unsigned src, unsigned idx_reg) { return kb13_cs_logic_op32(dst, src, idx_reg, KB13_LOGIC_AND_A_NB, 1); }

/* Subqueue-context offsets PanVK's CS reads/writes (struct panvk_cs_subqueue_context): render.tiler_heap +0x30, render.geom_buf +0x38,
 * td_count +0x70, last_td_fullscreen_tiler_flags +0x74. We keep a private 256 B "sqc" BO for the same layout (pointer in r122:123). */
#define KB13_SQC_TILER_HEAP 0x30
#define KB13_SQC_GEOM_BUF 0x38
#define KB13_SQC_TD_COUNT 0x70
#define KB13_SQC_REG 122

#endif
