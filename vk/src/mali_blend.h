/* SPDX-License-Identifier: MIT */
/* Minimal input for the fixed-function blend equation packer (see mali_blend.c). No driver or Mesa headers, so
 * tests/t_blend_eq.c can link the packer on the host and check the encoding against the Vulkan formula. */
#ifndef MALI_BLEND_H
#define MALI_BLEND_H

#include <stdbool.h>
#include <stdint.h>

struct mali_blend_eq_in {
   bool en;                        /* blendEnable */
   uint8_t src_c, dst_c, src_a, dst_a;   /* VkBlendFactor */
   uint8_t op_c, op_a;             /* VkBlendOp: ADD 0, SUBTRACT 1, REVERSE_SUBTRACT 2 */
};

/* Packs word 1 bits 0..23 of the blend descriptor; *cm gets the constant-channel mask (bit 0..2 colour, bit 3 alpha).
 * 0 = packed, -1 = not expressible in fixed function (the caller then compiles a blend shader). */
int mali_blend_eq(const struct mali_blend_eq_in *f, uint32_t *eq, unsigned *const_mask);

#endif
