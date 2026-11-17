/* SPDX-License-Identifier: MIT. Offline: the arch-10 differences of libkbase's packers (genxml v10.xml vs v11.xml):
 * no SET_STATE_IMM32 (SET_SB_ENTRY instead, op 23, endpoint 3:0 / other 7:4), ZSD bit 21 is not "separated dependency tracking". */
#include <stdio.h>
#include "kbase_cs.h"
#include "kbase_draw.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	uint32_t z[KB_ZSD_SIZE / 4];

	kb_v10 = 0;
	CHECK(kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, 3) == 0x1c00000000000003ull + (0ull << 32), "v11 SET_STATE_IMM32 endpoint 3: %llx", (unsigned long long)kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, 3));
	kb_draw_zsd_off(z);
	CHECK(z[4] & (1u << 21), "v11 ZSD bit 21 set");

	kb_v10 = 1;
	CHECK(kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, 3) == (0x17ull << 56 | 3 | ((uint64_t)KB_CS_SLOT_LS << 4)), "v10 SET_SB_ENTRY endpoint 3: %llx", (unsigned long long)kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, 3));
	CHECK(kb_cs_set_state_imm32(KB_CS_SB_SEL_OTHER, 0) == 0, "v10 SB_SEL_OTHER is a NOP");
	CHECK(kb_cs_set_state_imm32(KB_CS_SB_SEL_DEFERRED, 0) == 0, "v10 SB_SEL_DEFERRED is a NOP");
	kb_draw_zsd_off(z);
	CHECK(!(z[4] & (1u << 21)) && (z[4] & (1u << 22)), "v10 ZSD bit 21 clear, bit 22 kept");
	kb_draw_zsd_depth(z, KB_FUNC_ALWAYS, 1);
	CHECK(!(z[4] & (1u << 21)) && (z[4] & (1u << 27)), "v10 ZSD depth write, bit 21 clear");

	printf(fails ? "t_v10_cs: %d FAILED\n" : "t_v10_cs: ok\n", fails);
	return fails != 0;
}
