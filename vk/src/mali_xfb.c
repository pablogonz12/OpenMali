/* SPDX-License-Identifier: MIT */
#include "mali_xfb.h"

#include <string.h>

static uint32_t
vpp(unsigned t)
{
   switch (t) {
   case MALI_XFB_POINT_LIST: return 1;
   case MALI_XFB_LINE_LIST: case MALI_XFB_LINE_STRIP: return 2;
   case MALI_XFB_TRIANGLE_LIST: case MALI_XFB_TRIANGLE_STRIP: case MALI_XFB_TRIANGLE_FAN: return 3;
   default: return 0;
   }
}

static uint64_t
prims_in(unsigned t, uint32_t n)
{
   switch (t) {
   case MALI_XFB_POINT_LIST: return n;
   case MALI_XFB_LINE_LIST: return n / 2;
   case MALI_XFB_LINE_STRIP: return n >= 2 ? n - 1 : 0;
   case MALI_XFB_TRIANGLE_LIST: return n / 3;
   case MALI_XFB_TRIANGLE_STRIP: case MALI_XFB_TRIANGLE_FAN: return n >= 3 ? n - 2 : 0;
   default: return 0;
   }
}

uint32_t
mali_xfb_input_vertex(unsigned t, uint32_t p, uint32_t c)
{
   switch (t) {
   case MALI_XFB_POINT_LIST: return p;
   case MALI_XFB_LINE_LIST: return p * 2 + c;
   case MALI_XFB_LINE_STRIP: return p + c;
   case MALI_XFB_TRIANGLE_LIST: return p * 3 + c;
   case MALI_XFB_TRIANGLE_STRIP: return (p & 1) ? p + (c == 0 ? 1 : c == 1 ? 0 : 2) : p + c;
   case MALI_XFB_TRIANGLE_FAN: return c == 0 ? 0 : p + c;
   default: return 0;
   }
}

bool
mali_xfb_plan(unsigned t, uint32_t n, uint32_t instances, bool restart, const struct mali_xfb_buffer *buf, unsigned nbuf,
              struct mali_xfb_plan *o)
{
   uint32_t v = vpp(t);
   if (!v || nbuf == 0 || nbuf > MALI_XFB_MAX_BUFFERS || restart)
      return false;
   bool any = false;
   for (unsigned i = 0; i < nbuf; i++) {
      if (buf[i].stride & 3)
         return false;
      any |= buf[i].stride != 0;
   }
   if (!any)
      return false;
   struct mali_xfb_plan p;
   memset(&p, 0, sizeof p);
   p.verts_per_prim = v;
   uint64_t per_inst = prims_in(t, n);
   p.prims_needed = per_inst * instances;
   p.out_per_instance = (uint32_t)(per_inst * v);
   uint64_t fit = p.prims_needed;
   for (unsigned i = 0; i < nbuf; i++) {
      if (!buf[i].stride)
         continue;
      uint64_t room = buf[i].size > buf[i].start ? buf[i].size - buf[i].start : 0;
      uint64_t per_prim = (uint64_t)buf[i].stride * v;
      if (room / per_prim < fit)
         fit = room / per_prim;
   }
   p.prims_written = fit;
   p.out_vertices = fit * v;
   for (unsigned i = 0; i < nbuf; i++)
      if (buf[i].stride)
         p.counter[i] = buf[i].start + p.out_vertices * buf[i].stride;
   *o = p;
   return true;
}

uint64_t
mali_xfb_run_cpu(unsigned t, uint32_t n, uint32_t instances, const struct mali_xfb_plan *plan, uint32_t stride,
                 uint8_t *base, mali_xfb_cpu_vs vs, void *user)
{
   (void)n; (void)instances;
   uint32_t per_inst = plan->out_per_instance;
   uint64_t ran = 0;
   float rec[64];
   for (uint64_t k = 0; k < plan->out_vertices; k++) {
      uint32_t inst = (uint32_t)(k / per_inst), r = (uint32_t)(k % per_inst);
      uint32_t vid = mali_xfb_input_vertex(t, r / plan->verts_per_prim, r % plan->verts_per_prim);
      vs(user, vid, inst, rec);
      memcpy(base + k * stride, rec, stride);
      ran++;
   }
   return ran;
}
