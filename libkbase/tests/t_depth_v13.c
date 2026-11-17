// SPDX-License-Identifier: MIT
/*
 * t_depth_v13: depth-tested scene on Mali-G925 (arch 13, RUN_IDVS2), built on libkbase. Two overlapping triangles, the NEAR one (z 0.25, red) is drawn
 * FIRST and the FAR one (z 0.75, green) second, so only a working depth test (compare LESS, depth write on) leaves red in the overlap.
 *   depth_less_d32    depth buffer D32F (ZS write format D32), clear depth 1.0
 *   depth_less_d24    same with D24X8
 *   depth_clear_value clear depth 0.5, D32F: the far triangle (0.75 < 0.5 is false) must vanish entirely, near stays; depth buffer = 0.5 there
 * Pass = colour image analytic (see t_uniform.c) AND the written-back depth buffer matches the analytic depth (D24: low or high 24 bits,
 * the better decode is reported as zs_decode_mode). --func N overrides the compare function (7 = Always: the far triangle then wins the overlap;
 * the analytic expectation follows --func, so this also passes if the hardware honours it). --zsnostore skips depth write-back and its check.
 * Options: --dump --only N --timeout MS --hmask HEX --pk N --zs N --slot N --chunk BYTES --func N --zsnostore. See rung2_v13.h.
 */
#include "rung2_v13.h"

#define NEAR_T { { 0.8f, 0.8f, -0.15f, 0.0f }, 0.25f, { 1.0f, 0.0f, 0.0f, 1.0f } }
#define FAR_T  { { 0.8f, 0.8f, 0.15f, 0.0f }, 0.75f, { 0.0f, 1.0f, 0.0f, 1.0f } }

static const struct scene k_scenes[] = {
	{ "depth_less_d32", 2, { NEAR_T, FAR_T }, 1, KB_FUNC_LESS, KB_ZS_D32F, 1.0f },
	{ "depth_less_d24", 2, { NEAR_T, FAR_T }, 1, KB_FUNC_LESS, KB_ZS_D24X8, 1.0f },
	{ "depth_clear_value", 2, { NEAR_T, FAR_T }, 1, KB_FUNC_LESS, KB_ZS_D32F, 0.5f },
};

int main(int argc, char **argv)
{
	return r2_run("t_depth_v13", argc, argv, k_scenes, sizeof k_scenes / sizeof k_scenes[0], true);
}
