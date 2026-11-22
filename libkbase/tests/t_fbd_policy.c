/* Offline (no phone): the FBD policy bits that decide whether the tiler writes back tiles a frame never drew in.
 *   FBD word 12 bit 29 Empty Tile Write Enable and bit 28 Empty Tile Read Enable: CRC/ETE state, which we do not have
 *   (v13.xml:2320-2321, same at 12:29 on v10.xml:1734 and v11.xml:1992; PanVK enables them only with CRC,
 *   panfrost/lib/pan_desc.c:1296-1310).
 *   RT word 1 bit 31 Clean Tile Write Enable: only for a CLEAR loadOp (PanVK pan_fb.c:412-437, pan_desc.c:1094-1119);
 *   ZS/S word 0 bit 31 likewise (:523-526).
 *   FBD words 9..10 Bound Min/Max: the render area rounded to tiles, not the attachment (pan_desc.c:1501-1502).
 * Each case prints one line so the check output shows what changed. */
#include <stdio.h>
#include <string.h>
#include "kbase_fb.h"

static int fails;
static uint32_t W(const uint8_t *out, size_t off, unsigned i) { uint32_t w; memcpy(&w, out + off + 4 * i, 4); return w; }

static void
expect(const char *what, unsigned got, unsigned want)
{
   printf("  %-52s %u (want %u) %s\n", what, got, want, got == want ? "ok" : "FAIL");
   if (got != want) fails++;
}

int
main(void)
{
   struct kb_fb_params p;
   uint8_t out[512];
   memset(&p, 0, sizeof p);
   p.width = 64; p.height = 64;
   printf("t_fbd_policy: default (no bound box, no load-op flags)\n");
   expect("kb_fb_pack", kb_fb_pack(out, &p) == 0, 1);
   expect("FBD w12 Empty Tile Write Enable (12:29)", (W(out, 0, 12) >> 29) & 1, 0);
   expect("FBD w12 Empty Tile Read Enable (12:28)",  (W(out, 0, 12) >> 28) & 1, 0);
   expect("FBD w9 Bound Min X",  W(out, 0, 9) & 0xffff, 0);
   expect("FBD w10 Bound Max X", W(out, 0, 10) & 0xffff, 63);
   size_t rtoff = KB_FB_FBD_SIZE;
   expect("RT w1 Clean Tile Write Enable (1:31, default on)", (W(out, rtoff, 1) >> 31) & 1, 1);

   printf("t_fbd_policy: loadOp LOAD (no_clean_tile_write)\n");
   p.no_clean_tile_write = 1;
   p.bound_set = 1; p.bound_x0 = 32; p.bound_y0 = 32; p.bound_x1 = 63; p.bound_y1 = 63;
   expect("kb_fb_pack", kb_fb_pack(out, &p) == 0, 1);
   expect("RT w1 Clean Tile Write Enable", (W(out, rtoff, 1) >> 31) & 1, 0);
   expect("FBD w9 Bound Min X/Y", ((W(out, 0, 9) & 0xffff) << 16) | (W(out, 0, 9) >> 16), 0x00200020);
   expect("FBD w10 Bound Max X/Y", ((W(out, 0, 10) & 0xffff) << 16) | (W(out, 0, 10) >> 16), 0x003f003f);
   expect("FBD w12 Empty Tile Write Enable stays off", (W(out, 0, 12) >> 29) & 1, 0);

   printf("t_fbd_policy: depth/stencil LOAD\n");
   memset(&p, 0, sizeof p);
   p.width = 64; p.height = 64;
   p.zs_format = KB_ZS_D32F; p.zs_gpu_va = 0x1000; p.zs_row_stride = 256;
   p.has_stencil = 1; p.s_gpu_va = 0x2000; p.s_row_stride = 64;
   p.zs_no_clean_tile_write = 1; p.s_no_clean_tile_write = 1;
   expect("kb_fb_pack", kb_fb_pack(out, &p) == 0, 1);
   expect("ZS w0 Clean Tile Write Enable (0:31)", (W(out, KB_FB_FBD_SIZE, 0) >> 31) & 1, 0);
   expect("S w0 Clean Tile Write Enable (0:31, shared bit)", (W(out, KB_FB_FBD_SIZE + KB_FB_ZSEXT_SIZE, 0) >> 31) & 1, 0);

   printf("t_fbd_policy: %s\n", fails ? "FAILED" : "OK");
   return fails ? 1 : 0;
}

/* Stub: kb_fb_pack warns about v11-only fields through the shared arch-warning hook; this test does not care. */


/* Stub: kb_fb_pack warns about v11-only fields through the shared hook; this test does not care. */
void kb_arch_warn_v11_only(unsigned arch, const char *who) { (void)arch; (void)who; }
