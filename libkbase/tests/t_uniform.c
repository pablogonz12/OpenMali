// SPDX-License-Identifier: MIT
/*
 * t_uniform: push constants / uniforms (FAU) on Mali v11 (CSF), built on libkbase. Shaders uni.vert/uni.frag (tests/shaders_v11.h).
 *   uniform_scale_offset_colour  one triangle: the VS scales/offsets the vertex buffer triangle with push constants, the FS colour
 *                                comes from a push constant. Triangle (128,64) (192,64) (160,128) px, colour 0xff3366cc (R=204 G=102 B=51).
 *   uniform_two_draws            two disjoint triangles, same shaders, two FAU tables (per-draw uniforms): left blue-ish, right orange.
 * Pass = every pixel farther than 1 px from an edge matches the analytic colour (+-1 per channel) and the covered area is within 2 %
 * of the exact pixel-centre count. Options: --dump --only N --timeout MS --hmask HEX --pk N --zs N --slot N --chunk BYTES. See rung2.h.
 */
#include "rung2.h"

static const struct scene k_scenes[] = {
	{ "uniform_scale_offset_colour", 1, { { { 0.5f, 0.5f, 0.25f, -0.25f }, 0.5f, { 0.8f, 0.4f, 0.2f, 1.0f } } }, 0, KB_FUNC_ALWAYS, KB_ZS_NONE, 0.0f },
	{ "uniform_two_draws", 2, {
		{ { 0.5f, 0.5f, -0.5f, 0.0f }, 0.5f, { 0.2f, 0.6f, 1.0f, 1.0f } },
		{ { 0.5f, 0.5f, 0.5f, 0.0f }, 0.5f, { 1.0f, 0.5f, 0.0f, 1.0f } } }, 0, KB_FUNC_ALWAYS, KB_ZS_NONE, 0.0f },
};

int main(int argc, char **argv)
{
	return r2_run("t_uniform", argc, argv, k_scenes, sizeof k_scenes / sizeof k_scenes[0], false);
}
