/* SPDX-License-Identifier: MIT */
/* M7 offline tests: set layouts, pools, writes, templates, descriptor bytes vs libkbase (hardware-proven), and mali_compile with a
 * pipeline layout. Usage: t_desc [shader_dir] [-v]. No GPU, no loader. */
#include "../src/mali_descriptor.h"
#include "../src/mali_pipeline_layout.h"
#include "../src/mali_compile.h"
#include "../../libkbase/include/kbase_draw.h"

#include "vk_alloc.h"
#include "vk_object.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail, g_verbose;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *g_dir = "driver/vk/shader";

/* ---- fake resources: handles are pointers to these ---- */
struct fview { unsigned w, h; uint64_t plane_va; };
static struct fview g_view = { 8, 8, 0x1000 };
static const uint64_t k_texels_va = 0x7f0000100000ull;
static uint64_t g_buf_va = 0x7f0000200040ull, g_buf_size = 4096;

static int r_view(VkImageView v, VkDescriptorType t, struct mali_hw_desc *o)
{
   const struct fview *f = (const void *)(uintptr_t)v;
   mali_pack_texture_2d_rgba8(11, o, f->w, f->h, f->plane_va);
   return 0;
}
static int r_smp(VkSampler s, struct mali_hw_desc *o) { mali_pack_sampler(11, o, 1, 1, 9); return 0; }
static int r_buf(VkBuffer b, uint64_t *va, uint64_t *size) { *va = g_buf_va; *size = g_buf_size; return 0; }
static int r_bview(VkBufferView v, struct mali_hw_desc *o) { memset(o, 0xab, sizeof *o); return 0; }
static const struct mali_desc_res_ops k_res = { r_view, r_smp, r_buf, r_bview };

static struct vk_device g_dev;

static struct mali_dsl *
mk_layout(unsigned arch, uint32_t n, const VkDescriptorSetLayoutBinding *b)
{
   VkDescriptorSetLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b };
   struct mali_dsl *d = NULL;
   VkResult r = mali_dsl_create(&g_dev, arch, &ci, &d);
   CHECK(r == VK_SUCCESS, "layout create %d", r);
   return d;
}

static VkDescriptorSet
mk_set(unsigned arch, struct mali_dsl *l, VkDescriptorPool *poolout)
{
   VkDescriptorPoolSize ps[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 },
                                 { VK_DESCRIPTOR_TYPE_SAMPLER, 4 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 } };
   VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 4, .pPoolSizes = ps,
                                     .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT };
   VkResult r = mali_create_descriptor_pool(&g_dev, arch, &pi, NULL, poolout);
   CHECK(r == VK_SUCCESS, "pool create %d", r);
   VkDescriptorSetLayout hl = mali_dsl_to_handle(l);
   VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = *poolout, .descriptorSetCount = 1, .pSetLayouts = &hl };
   VkDescriptorSet s = VK_NULL_HANDLE;
   r = mali_allocate_descriptor_sets(&g_dev, &ai, &s);
   CHECK(r == VK_SUCCESS, "alloc set %d", r);
   return s;
}

/* known-good bytes from libkbase's t_texture: user set 0 = [0] texture, [1] sampler; plane separately; buffer */
static void
test_bytes(unsigned arch)
{
   printf("descriptor bytes vs libkbase, v%u\n", arch);
   VkDescriptorSetLayoutBinding b[] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
      { .binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_ALL },
   };
   struct mali_dsl *l = mk_layout(arch, 3, b);
   CHECK(l->nbindings == 5 && l->slot_count == 3 && l->dyn_count == 2, "layout shape %u %u %u", l->nbindings, l->slot_count, l->dyn_count);
   CHECK(l->b[0].slot == 0 && l->b[1].slot == 1 && l->b[1].stride == 2, "slots");
   CHECK(l->b[2].type == MALI_DT_NONE && l->b[4].dyn_idx == 0 && l->b[4].stride == 0, "holes and dyn");
   VkDescriptorPool pool;
   VkDescriptorSet hs = mk_set(arch, l, &pool);
   struct mali_dset *s = mali_dset_from_handle(hs);
   CHECK(s && s->bytes == 96, "set bytes");

   VkDescriptorBufferInfo bi = { (VkBuffer)(uintptr_t)1, 0x40, 256 };
   VkDescriptorImageInfo ii = { (VkSampler)(uintptr_t)2, (VkImageView)(uintptr_t)&g_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
   VkDescriptorBufferInfo dbi[2] = { { (VkBuffer)(uintptr_t)1, 0, VK_WHOLE_SIZE }, { (VkBuffer)(uintptr_t)1, 16, 64 } };
   VkWriteDescriptorSet w[] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = hs, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = hs, .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = hs, .dstBinding = 4, .descriptorCount = 2,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = dbi },
   };
   mali_update_descriptor_sets(&g_dev, 3, w, 0, NULL);

   /* oracle: exactly what t_texture builds (kb_draw_*) */
   uint32_t uset[16], buf[8];
   kb_draw_texture_2d_rgba8(uset, 8, 8, g_view.plane_va);
   kb_draw_sampler_nearest_clamp(uset + KB_DESC_SET_STRIDE / 4);
   CHECK(!memcmp(s->cpu + 32, uset, 64), "texture+sampler bytes differ from libkbase");
   kb_draw_buffer(buf, g_buf_va + 0x40, 256);
   CHECK(!memcmp(s->cpu, buf, 32), "UBO buffer descriptor differs from libkbase");
   CHECK(s->dyn[0].va == g_buf_va && s->dyn[0].size == g_buf_size, "dyn0 whole size");
   CHECK(s->dyn[1].va == g_buf_va + 16 && s->dyn[1].size == 64, "dyn1");
   struct mali_hw_desc d;
   mali_dyn_buf_pack(arch, &d, &s->dyn[1], 32);
   kb_draw_buffer(buf, g_buf_va + 16 + 32, 64);
   CHECK(!memcmp(&d, buf, 32), "dynamic buffer + offset differs from libkbase");
   /* plane descriptor too */
   uint32_t pl[8];
   kb_draw_plane_linear(pl, k_texels_va, 8, 8, kb_tex_row_stride_rgba8(8));
   mali_pack_plane_linear(arch, &d, k_texels_va, 8, 8, kb_tex_row_stride_rgba8(8));
   CHECK(!memcmp(&d, pl, 32), "plane bytes");
   if (g_verbose) { for (int i = 0; i < 16; i++) printf("%08x%c", ((uint32_t *)s->cpu)[8 + i], i % 8 == 7 ? '\n' : ' '); }

   /* copy: binding 1 -> a second set */
   VkDescriptorPool pool2;
   VkDescriptorSet hs2 = mk_set(arch, l, &pool2);
   VkCopyDescriptorSet cp = { .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = hs, .srcBinding = 1, .dstSet = hs2, .dstBinding = 1, .descriptorCount = 1 };
   mali_update_descriptor_sets(&g_dev, 0, NULL, 1, &cp);
   CHECK(!memcmp(mali_dset_from_handle(hs2)->cpu + 32, uset, 64), "copy descriptor set");

   /* template: same data via the update-template path */
   struct vk_descriptor_update_template *tp = calloc(1, sizeof *tp + 2 * sizeof(struct vk_descriptor_template_entry));
   tp->entry_count = 2;
   tp->entries[0] = (struct vk_descriptor_template_entry){ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, 0, 1, 0, 0 };
   tp->entries[1] = (struct vk_descriptor_template_entry){ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, 0, 1, offsetof(struct { VkDescriptorImageInfo i; VkDescriptorBufferInfo b; }, b), 0 };
   struct { VkDescriptorImageInfo i; VkDescriptorBufferInfo b; } data = { ii, bi };
   VkDescriptorPool pool3;
   VkDescriptorSet hs3 = mk_set(arch, l, &pool3);
   mali_dset_update_template(mali_dset_from_handle(hs3), tp, &data);
   CHECK(!memcmp(mali_dset_from_handle(hs3)->cpu + 32, uset, 64), "template texture bytes");
   kb_draw_buffer(buf, g_buf_va + 0x40, 256);
   CHECK(!memcmp(mali_dset_from_handle(hs3)->cpu, buf, 32), "template UBO bytes");

   mali_destroy_descriptor_pool(&g_dev, pool, NULL);
   mali_destroy_descriptor_pool(&g_dev, pool2, NULL);
   mali_destroy_descriptor_pool(&g_dev, pool3, NULL);
   free(tp);
   vk_descriptor_set_layout_unref(&g_dev, &l->vk);
}

static void
test_pool(void)
{
   printf("pool: first fit, free, reuse, exhaustion\n");
   VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 2, .stageFlags = 0x1f };
   struct mali_dsl *l = mk_layout(11, 1, &b);
   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 5 };   /* 5 descriptors = 2 sets of 2 + 1 spare */
   VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8, .poolSizeCount = 1, .pPoolSizes = &ps,
                                     .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT };
   VkDescriptorPool pool;
   CHECK(mali_create_descriptor_pool(&g_dev, 11, &pi, NULL, &pool) == VK_SUCCESS, "pool");
   VkDescriptorSetLayout hl[3] = { mali_dsl_to_handle(l), mali_dsl_to_handle(l), mali_dsl_to_handle(l) };
   VkDescriptorSet s[3];
   VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 2, .pSetLayouts = hl };
   CHECK(mali_allocate_descriptor_sets(&g_dev, &ai, s) == VK_SUCCESS, "alloc 2");
   ai.descriptorSetCount = 1;
   CHECK(mali_allocate_descriptor_sets(&g_dev, &ai, &s[2]) == VK_ERROR_OUT_OF_POOL_MEMORY, "alloc 3rd must fail");
   CHECK(s[2] == VK_NULL_HANDLE, "failed alloc returns null handle");
   struct mali_dset *a = mali_dset_from_handle(s[0]), *c = mali_dset_from_handle(s[1]);
   CHECK(c->va == a->va + 64 && (a->va % 32) == 0, "contiguous, 32-aligned");
   uint64_t freed_va = a->va;
   mali_free_descriptor_sets(&g_dev, pool, 1, &s[0]);
   CHECK(mali_allocate_descriptor_sets(&g_dev, &ai, &s[2]) == VK_SUCCESS && mali_dset_from_handle(s[2])->va == freed_va, "reuses the freed hole");
   mali_reset_descriptor_pool(&g_dev, pool);
   CHECK(mali_allocate_descriptor_sets(&g_dev, &ai, &s[0]) == VK_SUCCESS, "alloc after reset");
   mali_destroy_descriptor_pool(&g_dev, pool, NULL);
   vk_descriptor_set_layout_unref(&g_dev, &l->vk);
}


/* ---- pool / set lifetime, write spill, copy, dynamic, inline, template stride, immutable samplers (fake backend) ---- */
static VkDescriptorPool
mk_pool(unsigned arch, uint32_t max_sets, VkDescriptorType t, uint32_t count, VkDescriptorPoolCreateFlags fl)
{
   VkDescriptorPoolSize ps = { t, count };
   VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = max_sets, .poolSizeCount = 1, .pPoolSizes = &ps, .flags = fl };
   VkDescriptorPool p = VK_NULL_HANDLE;
   CHECK(mali_create_descriptor_pool(&g_dev, arch, &pi, NULL, &p) == VK_SUCCESS, "pool create");
   return p;
}

static VkResult
alloc_n(VkDescriptorPool pool, uint32_t n, struct mali_dsl *l, VkDescriptorSet *out)
{
   VkDescriptorSetLayout hl[8];
   for (uint32_t i = 0; i < n; i++) hl[i] = mali_dsl_to_handle(l);
   VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = n, .pSetLayouts = hl };
   return mali_allocate_descriptor_sets(&g_dev, &ai, out);
}

static void
test_pool_limits(unsigned arch)
{
   printf("pool limits, v%u\n", arch);
   VkDescriptorSetLayoutBinding b1 = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_ALL };
   struct mali_dsl *l = mk_layout(arch, 1, &b1);   /* 2 slots = 64 bytes */
   VkDescriptorSet s[4] = {0};

   /* maxSets limit with plenty of bytes */
   VkDescriptorPool p = mk_pool(arch, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
   CHECK(alloc_n(p, 2, l, s) == VK_SUCCESS && s[0] && s[1], "2 sets within maxSets");
   VkDescriptorSet x = VK_NULL_HANDLE;
   CHECK(alloc_n(p, 1, l, &x) == VK_ERROR_OUT_OF_POOL_MEMORY && x == VK_NULL_HANDLE, "maxSets exhausted");
   mali_free_descriptor_sets(&g_dev, p, 1, &s[0]);
   CHECK(mali_dpool_from_handle(p)->nsets == 1, "nsets after free");
   CHECK(alloc_n(p, 1, l, &x) == VK_SUCCESS, "slot frees up after free");
   mali_destroy_descriptor_pool(&g_dev, p, NULL);

   /* byte capacity limit with a high maxSets: 3 UBO descriptors = 96 bytes, a set needs 64 */
   p = mk_pool(arch, 100, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
   CHECK(mali_dpool_from_handle(p)->cap == 96, "pool capacity bytes %u", mali_dpool_from_handle(p)->cap);
   CHECK(alloc_n(p, 1, l, &s[0]) == VK_SUCCESS, "first fits");
   CHECK(alloc_n(p, 1, l, &s[1]) == VK_ERROR_OUT_OF_POOL_MEMORY && s[1] == VK_NULL_HANDLE, "second does not fit in bytes");
   /* all or nothing: a two-set request that only half fits leaves the pool untouched */
   mali_free_descriptor_sets(&g_dev, p, 1, &s[0]);
   VkDescriptorSet two[2] = { (VkDescriptorSet)(uintptr_t)1, (VkDescriptorSet)(uintptr_t)1 };
   CHECK(alloc_n(p, 2, l, two) == VK_ERROR_OUT_OF_POOL_MEMORY, "half-fitting request fails");
   CHECK(two[0] == VK_NULL_HANDLE && two[1] == VK_NULL_HANDLE, "failed request returns only null handles");
   CHECK(mali_dpool_from_handle(p)->nsets == 0 && mali_dpool_from_handle(p)->sets == NULL, "failed request rolled back (nsets %u)", mali_dpool_from_handle(p)->nsets);
   CHECK(alloc_n(p, 1, l, &s[0]) == VK_SUCCESS, "pool still usable after rollback");

   /* double free and freeing into the wrong pool must not corrupt the live list or the count */
   VkDescriptorPool q = mk_pool(arch, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
   VkDescriptorSet qs = VK_NULL_HANDLE;
   CHECK(alloc_n(q, 1, l, &qs) == VK_SUCCESS, "alloc in q");
   mali_free_descriptor_sets(&g_dev, p, 1, &qs);   /* wrong pool */
   CHECK(mali_dpool_from_handle(q)->nsets == 1 && mali_dpool_from_handle(p)->nsets == 1, "foreign free is a no-op");
   VkDescriptorSet nul = VK_NULL_HANDLE;
   mali_free_descriptor_sets(&g_dev, q, 1, &nul);   /* VK_NULL_HANDLE is ignored */
   CHECK(mali_dpool_from_handle(q)->nsets == 1, "null free ignored");
   struct mali_dset *keep = mali_dset_from_handle(qs);
   mali_free_descriptor_sets(&g_dev, q, 1, &qs);
   CHECK(mali_dpool_from_handle(q)->nsets == 0 && mali_dpool_from_handle(q)->sets == NULL, "free ok");
   (void)keep;   /* freed memory: not touched again; a second free would be UB by spec, only the foreign/null cases are tested */

   /* reset clears all sets and zeroes pool memory; destroy with live sets must not leak or crash */
   struct mali_dset *live = mali_dset_from_handle(s[0]);
   memset(live->cpu, 0x5a, live->bytes);
   uint8_t *mem = mali_dpool_from_handle(p)->cpu;
   CHECK(mali_reset_descriptor_pool(&g_dev, p) == VK_SUCCESS && mali_dpool_from_handle(p)->nsets == 0 && mali_dpool_from_handle(p)->sets == NULL, "reset");
   int dirty = 0;
   for (uint32_t i = 0; i < 96; i++) dirty |= mem[i];
   CHECK(!dirty, "reset zeroes pool memory");
   CHECK(alloc_n(p, 1, l, &s[0]) == VK_SUCCESS && mali_dset_from_handle(s[0])->off == 0, "alloc restarts at offset 0 after reset");
   mali_destroy_descriptor_pool(&g_dev, p, NULL);   /* one live set */
   CHECK(alloc_n(q, 1, l, &qs) == VK_SUCCESS, "q alive");
   mali_destroy_descriptor_pool(&g_dev, q, NULL);   /* one live set */
   mali_destroy_descriptor_pool(&g_dev, VK_NULL_HANDLE, NULL);

   /* creation errors */
   VkDescriptorPoolSize bad = { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 };
   VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &bad };
   VkDescriptorPool np = VK_NULL_HANDLE;
   CHECK(mali_create_descriptor_pool(&g_dev, arch, &pi, NULL, &np) == VK_ERROR_FEATURE_NOT_PRESENT, "unsupported pool type");
   VkDescriptorPoolSize huge = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0x10000000u };
   pi.pPoolSizes = &huge;
   CHECK(mali_create_descriptor_pool(&g_dev, arch, &pi, NULL, &np) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "pool over 1 GiB rejected");
   vk_descriptor_set_layout_unref(&g_dev, &l->vk);
}

static void
test_write_copy(unsigned arch)
{
   printf("write spill, copy, dynamic, null, v%u\n", arch);
   VkDescriptorSetLayoutBinding b[] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_ALL },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_ALL },
      { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .descriptorCount = 3, .stageFlags = VK_SHADER_STAGE_ALL },
      { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL },
   };
   struct mali_dsl *l = mk_layout(arch, 4, b);
   CHECK(l->slot_count == 5 && l->dyn_count == 3, "layout %u slots %u dyn", l->slot_count, l->dyn_count);
   VkDescriptorPool pool = mk_pool(arch, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64, 0);
   VkDescriptorSet hs[2];
   CHECK(alloc_n(pool, 2, l, hs) == VK_SUCCESS, "alloc");
   struct mali_dset *a = mali_dset_from_handle(hs[0]), *c = mali_dset_from_handle(hs[1]);
   CHECK(a->dyn && c->dyn, "dyn arrays allocated");

   /* a write of 3 descriptors at binding 0 element 1 spills into binding 1 (spec: following bindings) */
   VkDescriptorBufferInfo bi[3] = { { (VkBuffer)1, 0x10, 64 }, { (VkBuffer)1, 0x20, 128 }, { (VkBuffer)1, 0x30, VK_WHOLE_SIZE } };
   VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = hs[0], .dstBinding = 0, .dstArrayElement = 1, .descriptorCount = 3,
                              .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = bi };
   mali_update_descriptor_sets(&g_dev, 1, &w, 0, NULL);
   struct mali_hw_desc e[3], z;
   mali_pack_buffer(arch, &e[0], g_buf_va + 0x10, 64);
   mali_pack_buffer(arch, &e[1], g_buf_va + 0x20, 128);
   mali_pack_buffer(arch, &e[2], g_buf_va + 0x30, g_buf_size - 0x30);
   memset(&z, 0, sizeof z);
   CHECK(!memcmp(a->cpu, &z, 32), "element 0 untouched");
   CHECK(!memcmp(a->cpu + 32, &e[0], 32), "b0[1] written");
   CHECK(!memcmp(a->cpu + 64, &e[1], 32), "spill into b1[0]");
   CHECK(!memcmp(a->cpu + 96, &e[2], 32), "spill into b1[1], WHOLE_SIZE = buffer size - offset");

   /* a write past the last binding is dropped and does not touch the neighbouring slot (b3) nor the next set */
   w.dstBinding = 3; w.dstArrayElement = 0; w.descriptorCount = 3;
   uint8_t before[32];
   memcpy(before, c->cpu, 32);
   mali_update_descriptor_sets(&g_dev, 1, &w, 0, NULL);
   mali_pack_buffer(arch, &e[0], g_buf_va + 0x10, 64);
   CHECK(!memcmp(a->cpu + 128, &e[0], 32), "b3[0] written, rest dropped");
   CHECK(!memcmp(c->cpu, before, 32), "overflowing write did not run into the next set");

   /* dynamic buffers: base va and range stored, offset applied at bind time; spill inside the dynamic binding */
   VkDescriptorBufferInfo db[3] = { { (VkBuffer)1, 0x100, 16 }, { (VkBuffer)1, 0x200, VK_WHOLE_SIZE }, { VK_NULL_HANDLE, 0, 0 } };
   VkWriteDescriptorSet dw = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = hs[0], .dstBinding = 2, .descriptorCount = 3,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = db };
   mali_update_descriptor_sets(&g_dev, 1, &dw, 0, NULL);
   CHECK(a->dyn[0].va == g_buf_va + 0x100 && a->dyn[0].size == 16, "dyn[0]");
   CHECK(a->dyn[1].va == g_buf_va + 0x200 && a->dyn[1].size == g_buf_size - 0x200, "dyn[1] whole size");
   CHECK(a->dyn[2].va == 0 && a->dyn[2].size == 0, "dyn[2] null buffer");
   struct mali_hw_desc d, ref;
   mali_dyn_buf_pack(arch, &d, &a->dyn[0], 0x40);
   mali_pack_buffer(arch, &ref, g_buf_va + 0x140, 16);
   CHECK(!memcmp(&d, &ref, 32), "dyn offset added to va, range kept");
   memset(&d, 0xcc, sizeof d);
   mali_dyn_buf_pack(arch, &d, &a->dyn[2], 0x40);
   CHECK(!memcmp(&d, &z, 32), "null dynamic buffer stays a null descriptor whatever the offset");

   /* null buffer in a non-dynamic binding writes a null descriptor over old data */
   VkDescriptorBufferInfo nb = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
   VkWriteDescriptorSet nw = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = hs[0], .dstBinding = 0, .dstArrayElement = 1, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &nb };
   mali_update_descriptor_sets(&g_dev, 1, &nw, 0, NULL);
   CHECK(!memcmp(a->cpu + 32, &z, 32), "null UBO overwrites old descriptor");

   /* copy across bindings and sets: 2 elements from a.b1[0..1] to c.b0[0..1] (spill source is NOT followed by copies: count limited) */
   VkCopyDescriptorSet cp = { .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = hs[0], .srcBinding = 1, .dstSet = hs[1], .dstBinding = 0, .descriptorCount = 2 };
   mali_update_descriptor_sets(&g_dev, 0, NULL, 1, &cp);
   mali_pack_buffer(arch, &e[0], g_buf_va + 0x20, 128);
   mali_pack_buffer(arch, &e[1], g_buf_va + 0x30, g_buf_size - 0x30);
   CHECK(!memcmp(c->cpu, &e[0], 32) && !memcmp(c->cpu + 32, &e[1], 32), "array copy of 2 elements");
   /* copy with element offsets; the second element continues into the next consecutive binding (dst elem 1 has room for 1) */
   memset(c->cpu, 0, 64);
   cp.srcArrayElement = 0; cp.dstArrayElement = 1; cp.descriptorCount = 2;
   mali_update_descriptor_sets(&g_dev, 0, NULL, 1, &cp);
   CHECK(!memcmp(c->cpu, &z, 32) && !memcmp(c->cpu + 32, &e[0], 32), "copy honours dstArrayElement");
   CHECK(!memcmp(c->cpu + 64, &e[1], 32), "copy continues into the next consecutive binding");
   /* dynamic copy */
   VkCopyDescriptorSet dc = { .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = hs[0], .srcBinding = 2, .srcArrayElement = 1, .dstSet = hs[1],
                              .dstBinding = 2, .dstArrayElement = 0, .descriptorCount = 2 };
   mali_update_descriptor_sets(&g_dev, 0, NULL, 1, &dc);
   CHECK(c->dyn[0].va == a->dyn[1].va && c->dyn[0].size == a->dyn[1].size && c->dyn[1].va == 0, "dynamic descriptors copied");
   CHECK(c->dyn[2].va == 0, "dynamic copy stayed inside its range");
   /* copy between a dynamic and a slot binding is ignored (no memory is mixed up) */
   memcpy(before, c->cpu, 32);
   VkCopyDescriptorSet mc = { .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = hs[0], .srcBinding = 2, .dstSet = hs[1], .dstBinding = 0, .descriptorCount = 1 };
   mali_update_descriptor_sets(&g_dev, 0, NULL, 1, &mc);
   CHECK(!memcmp(c->cpu, before, 32), "dynamic -> slot copy ignored");

   /* copy over unused bindings: src has b0,b2 (gap at b1), dst has b0,b3 (gap at b1,b2); a 2-element copy must skip both gaps */
   VkDescriptorSetLayoutBinding gs[] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL },
      { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL },
   };
   VkDescriptorSetLayoutBinding gd[] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL },
      { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL },
   };
   struct mali_dsl *gsl = mk_layout(arch, 2, gs), *gdl = mk_layout(arch, 2, gd);
   VkDescriptorPool gp = mk_pool(arch, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8, 0);
   VkDescriptorSet gss, gds;
   CHECK(alloc_n(gp, 1, gsl, &gss) == VK_SUCCESS && alloc_n(gp, 1, gdl, &gds) == VK_SUCCESS, "gap sets alloc");
   struct mali_dset *gsd = mali_dset_from_handle(gss), *gdd = mali_dset_from_handle(gds);
   VkDescriptorBufferInfo g0 = { (VkBuffer)1, 0x10, 64 }, g2 = { (VkBuffer)1, 0x20, 128 };
   VkWriteDescriptorSet gw0 = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = gss, .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &g0 };
   VkWriteDescriptorSet gw2 = gw0;
   gw2.dstBinding = 2; gw2.pBufferInfo = &g2;
   VkWriteDescriptorSet gws[2] = { gw0, gw2 };
   mali_update_descriptor_sets(&g_dev, 2, gws, 0, NULL);
   VkCopyDescriptorSet gc = { .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = gss, .srcBinding = 0, .dstSet = gds, .dstBinding = 0, .descriptorCount = 2 };
   mali_update_descriptor_sets(&g_dev, 0, NULL, 1, &gc);
   CHECK(!memcmp(gdd->cpu, gsd->cpu, 32) && memcmp(gdd->cpu, &z, 32), "gap copy: first descriptor");
   CHECK(!memcmp(gdd->cpu + 32, gsd->cpu + 32, 32) && memcmp(gdd->cpu + 32, &z, 32), "gap copy: continues past unused bindings on both sides");
   mali_destroy_descriptor_pool(&g_dev, gp, NULL);
   vk_descriptor_set_layout_unref(&g_dev, &gsl->vk);
   vk_descriptor_set_layout_unref(&g_dev, &gdl->vk);


   mali_destroy_descriptor_pool(&g_dev, pool, NULL);
   vk_descriptor_set_layout_unref(&g_dev, &l->vk);
}

static void
test_inline_template_imm(unsigned arch)
{
   printf("inline uniform, template stride, immutable samplers, v%u\n", arch);
   VkDescriptorSetLayoutBinding bad[] = { { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, .descriptorCount = 6, .stageFlags = VK_SHADER_STAGE_ALL } };
   VkDescriptorSetLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = bad };
   struct mali_dsl *dd = NULL;
   CHECK(mali_dsl_create(&g_dev, arch, &ci, &dd) == VK_ERROR_FEATURE_NOT_PRESENT, "inline size not a multiple of 4 rejected");
   bad[0].descriptorCount = 260;
   CHECK(mali_dsl_create(&g_dev, arch, &ci, &dd) == VK_ERROR_FEATURE_NOT_PRESENT, "inline size over 256 rejected");

   VkSampler imm[1] = { (VkSampler)(uintptr_t)7 };
   VkDescriptorSetLayoutBinding b[] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, .descriptorCount = 40, .stageFlags = VK_SHADER_STAGE_ALL },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .pImmutableSamplers = imm },
      { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 3, .stageFlags = VK_SHADER_STAGE_ALL },
   };
   struct mali_dsl *l = mk_layout(arch, 3, b);
   CHECK(l->b[0].inline_bytes == 40 && l->b[0].stride == 3 && l->b[1].slot == 3 && l->b[2].slot == 5, "inline occupies 1 header + 2 data slots (%u %u %u)", l->b[0].stride, l->b[1].slot, l->b[2].slot);
   VkDescriptorPoolSize ps[] = { { VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 80 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8 } };
   VkDescriptorPoolInlineUniformBlockCreateInfo pib = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO, .maxInlineUniformBlockBindings = 2 };
   VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .pNext = &pib, .maxSets = 2, .poolSizeCount = 3, .pPoolSizes = ps };
   VkDescriptorPool pool;
   CHECK(mali_create_descriptor_pool(&g_dev, arch, &pi, NULL, &pool) == VK_SUCCESS, "pool with inline size");
   VkDescriptorSet hs[2];
   CHECK(alloc_n(pool, 2, l, hs) == VK_SUCCESS, "alloc");
   struct mali_dset *s = mali_dset_from_handle(hs[0]), *t = mali_dset_from_handle(hs[1]);
   CHECK(s->bytes == 8 * 32, "set bytes %u", s->bytes);

   /* alloc writes the inline header (buffer descriptor -> data slots) and the immutable sampler without any update */
   struct mali_hw_desc h, ref;
   mali_pack_buffer(arch, &ref, s->va + 32, 40);
   memcpy(&h, s->cpu, 32);
   CHECK(!memcmp(&h, &ref, 32), "inline header points at slot+1 with the byte size");
   mali_pack_sampler(11, &ref, 1, 1, 9);   /* fake r_smp output */
   CHECK(!memcmp(s->cpu + 3 * 32 + 32, &ref, 32), "immutable sampler written at alloc (second slot of the combined element)");

   /* inline writes: byte offsets, bounds */
   uint8_t data[64];
   for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i + 1);
   VkWriteDescriptorSetInlineUniformBlock ib = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK, .dataSize = 8, .pData = data };
   VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &ib, .dstSet = hs[0], .dstBinding = 0, .dstArrayElement = 4, .descriptorCount = 8,
                              .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK };
   mali_update_descriptor_sets(&g_dev, 1, &w, 0, NULL);
   CHECK(!memcmp(s->cpu + 32 + 4, data, 8) && s->cpu[32 + 3] == 0 && s->cpu[32 + 12] == 0, "inline write at byte offset 4");
   w.dstArrayElement = 36; w.descriptorCount = 8; ib.dataSize = 8;   /* 36 + 8 > 40: dropped entirely */
   mali_update_descriptor_sets(&g_dev, 1, &w, 0, NULL);
   CHECK(s->cpu[32 + 36] == 0 && s->cpu[32 + 40] == 0, "out-of-range inline write dropped, no bytes spilled into the next binding");
   w.dstArrayElement = 0; w.descriptorCount = 40; ib.dataSize = 40;
   mali_update_descriptor_sets(&g_dev, 1, &w, 0, NULL);
   CHECK(!memcmp(s->cpu + 32, data, 40) && s->cpu[32 + 40] == 0, "full-size inline write stops at the block end");

   /* inline copy: byte offsets and counts */
   VkCopyDescriptorSet cp = { .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = hs[0], .srcBinding = 0, .srcArrayElement = 8, .dstSet = hs[1], .dstBinding = 0,
                              .dstArrayElement = 4, .descriptorCount = 12 };
   mali_update_descriptor_sets(&g_dev, 0, NULL, 1, &cp);
   CHECK(!memcmp(t->cpu + 32 + 4, data + 8, 12) && t->cpu[32 + 3] == 0 && t->cpu[32 + 16] == 0, "inline copy by bytes");
   cp.descriptorCount = 40;   /* 40 bytes at src offset 8 overruns the block: ignored */
   memset(t->cpu + 32, 0, 40);
   mali_update_descriptor_sets(&g_dev, 0, NULL, 1, &cp);
   CHECK(t->cpu[32 + 4] == 0, "overrunning inline copy ignored");

   /* update template: user stride bigger than the Vulkan struct, element offset, inline entry (bytes, count = size) */
   struct user { VkDescriptorBufferInfo bi; char pad[24]; };
   struct blob { uint8_t inl[40]; struct user u[3]; } blob;
   memset(&blob, 0, sizeof blob);
   for (int i = 0; i < 40; i++) blob.inl[i] = (uint8_t)(0xa0 + i);
   for (int i = 0; i < 3; i++) blob.u[i].bi = (VkDescriptorBufferInfo){ (VkBuffer)1, 0x100u * (i + 1), 32u * (i + 1) };
   struct vk_descriptor_update_template *tp = calloc(1, sizeof *tp + 2 * sizeof(struct vk_descriptor_template_entry));
   tp->entry_count = 2;
   tp->entries[0] = (struct vk_descriptor_template_entry){ VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 0, 0, 40, offsetof(struct blob, inl), 0 };
   tp->entries[1] = (struct vk_descriptor_template_entry){ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2, 1, 2, offsetof(struct blob, u[1]), sizeof(struct user) };
   VkDescriptorSet h3[1];
   VkDescriptorPoolSize ps2[] = { { VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 80 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8 } };
   pi.maxSets = 1; pi.pPoolSizes = ps2;
   VkDescriptorPool pool2;
   CHECK(mali_create_descriptor_pool(&g_dev, arch, &pi, NULL, &pool2) == VK_SUCCESS, "pool2");
   CHECK(alloc_n(pool2, 1, l, h3) == VK_SUCCESS, "alloc 3");
   struct mali_dset *u = mali_dset_from_handle(h3[0]);
   mali_dset_update_template(u, tp, &blob);
   CHECK(!memcmp(u->cpu + 32, blob.inl, 40), "template inline bytes");
   /* elements 1 and 2 of binding 2 get user[1] and user[2], element 0 untouched */
   struct mali_hw_desc z; memset(&z, 0, sizeof z);
   mali_pack_buffer(arch, &ref, g_buf_va + 0x200, 64);
   CHECK(!memcmp(u->cpu + (5 + 0) * 32, &z, 32), "template: binding 2 element 0 untouched");
   CHECK(!memcmp(u->cpu + (5 + 1) * 32, &ref, 32), "template: element 1 from user[1] (stride %zu)", sizeof(struct user));
   mali_pack_buffer(arch, &ref, g_buf_va + 0x300, 96);
   CHECK(!memcmp(u->cpu + (5 + 2) * 32, &ref, 32), "template: element 2 from user[2]");
   free(tp);

   mali_destroy_descriptor_pool(&g_dev, pool, NULL);
   mali_destroy_descriptor_pool(&g_dev, pool2, NULL);
   vk_descriptor_set_layout_unref(&g_dev, &l->vk);
}

/* ---- compile with a layout ---- */
struct spv { uint32_t *w; size_t n; };
static struct spv
load(const char *name)
{
   char p[512];
   snprintf(p, sizeof p, "%s/%s", g_dir, name);
   struct spv s = {0};
   FILE *f = fopen(p, "rb");
   if (!f) { printf("  cannot open %s\n", p); g_fail++; return s; }
   fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
   s.w = malloc(n);
   if (fread(s.w, 1, n, f) != (size_t)n) { g_fail++; free(s.w); s.w = NULL; } else s.n = n / 4;
   fclose(f);
   return s;
}

static void
test_compile(unsigned arch)
{
   printf("mali_compile with pipeline layout, v%u\n", arch);
   /* set 0: UBO, sampled image, sampler (desc.frag); push constant 16 B */
   VkDescriptorSetLayoutBinding b0[] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = 0x1f },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1, .stageFlags = 0x1f },
      { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = 0x1f },
   };
   VkDescriptorSetLayoutBinding b1[] = { { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2, .stageFlags = 0x1f } };
   struct mali_dsl *l0 = mk_layout(arch, 3, b0), *l1 = mk_layout(arch, 1, b1);
   struct vk_pipeline_layout pl;
   memset(&pl, 0, sizeof pl);
   pl.set_count = 2; pl.set_layouts[0] = &l0->vk; pl.set_layouts[1] = &l1->vk;
   pl.push_range_count = 1; pl.push_ranges[0] = (VkPushConstantRange){ 0x1f, 0, 16 };
   struct mali_flat_layout fl;
   CHECK(mali_pipeline_layout_flatten(&pl, &fl) == VK_SUCCESS, "flatten");
   CHECK(fl.c.nsets == 2 && fl.c.push_size == 16 && fl.c.set[0].slot_count == 3 && fl.c.set[1].slot_count == 2, "flat shape");
   CHECK(mali_set_table(0) == 1 && mali_set_table(1) == 2, "tables");

   struct spv fs = load("desc.frag.spv"), cs = load("desc.comp.spv");
   struct mali_compile_opts o = { .arch = arch, .stage = MALI_STAGE_FRAGMENT, .spirv = fs.w, .spirv_words = fs.n, .layout = &fl.c, .cache_dir = "" };
   struct mali_compiled c;
   char err[256] = "";
   int r = mali_compile(&o, &c, err, sizeof err);
   CHECK(r == 0, "frag compile: %s", err);
   if (r == 0) {
      CHECK(c.bin_size > 0 && c.ubo_count >= 1, "ubo_count %u", c.ubo_count);
      CHECK(c.push_const_bytes == 16, "push bytes %u", c.push_const_bytes);
      struct mali_compiled c2;
      CHECK(mali_compile(&o, &c2, err, sizeof err) == 0 && c2.bin_size == c.bin_size && !memcmp(c.bin, c2.bin, c.bin_size), "deterministic");
      mali_compiled_free(&c2);
      mali_compiled_free(&c);
   }
   /* the lowering is binding-sensitive: moving the texture/sampler to other slots changes the code (handles are immediates) */
   struct mali_lbinding moved[3];
   memcpy(moved, fl.c.set[0].b, sizeof moved);
   moved[1].slot = 9; moved[2].slot = 12;
   struct mali_compile_layout lm = fl.c;
   lm.set[0].b = moved;
   o.layout = &lm;
   struct mali_compiled cm, c0;
   o.layout = &fl.c;
   CHECK(mali_compile(&o, &c0, err, sizeof err) == 0, "recompile");
   o.layout = &lm;
   CHECK(mali_compile(&o, &cm, err, sizeof err) == 0, "moved compile: %s", err);
   CHECK(cm.bin_size != c0.bin_size || memcmp(cm.bin, c0.bin, cm.bin_size) || cm.nimm != c0.nimm || memcmp(cm.imm, c0.imm, cm.nimm * sizeof *cm.imm),
         "moving texture/sampler slots must change the output");
   mali_compiled_free(&cm); mali_compiled_free(&c0);
   /* push constants beyond the range are rejected */
   struct mali_compile_layout small = fl.c;
   small.push_size = 8;
   o.layout = &small;
   CHECK(mali_compile(&o, &c, err, sizeof err) != 0, "push beyond layout must fail");
   /* missing binding rejected */
   struct mali_compile_layout nobind = fl.c;
   nobind.set[0].nbindings = 1;
   o.layout = &nobind;
   err[0] = 0;
   CHECK(mali_compile(&o, &c, err, sizeof err) != 0 && strstr(err, "binding"), "missing binding must fail: %s", err);
   /* compute: SSBO array + UBO + push */
   o = (struct mali_compile_opts){ .arch = arch, .stage = MALI_STAGE_COMPUTE, .spirv = cs.w, .spirv_words = cs.n, .layout = &fl.c, .cache_dir = "" };
   r = mali_compile(&o, &c, err, sizeof err);
   CHECK(r == 0, "comp compile: %s", err);
   if (r == 0) { CHECK(c.bin_size > 0 && c.ubo_count >= 1 && c.push_const_bytes == 4, "comp ubo %u push %u", c.ubo_count, c.push_const_bytes); mali_compiled_free(&c); }
   /* without layout the old descriptor-free path still compiles the plain shaders */
   free(fs.w); free(cs.w);
   vk_descriptor_set_layout_unref(&g_dev, &l0->vk);
   vk_descriptor_set_layout_unref(&g_dev, &l1->vk);
}

int
main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "-v")) g_verbose = 1;
      else g_dir = argv[i];
   }
   g_dev.alloc = *vk_default_allocator();
   mali_desc_set_res_ops(&k_res);
   struct timespec a, b;
   clock_gettime(CLOCK_MONOTONIC, &a);
   test_bytes(11);
   test_bytes(13);
   test_pool();
   for (unsigned a = 11; a <= 13; a += 2) {
      test_pool_limits(a);
      test_write_copy(a);
      test_inline_template_imm(a);
   }
   test_compile(11);
   test_compile(13);
   clock_gettime(CLOCK_MONOTONIC, &b);
   printf("%s (%d failures, %.2f s)\n", g_fail ? "FAIL" : "PASS", g_fail, (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9);
   return g_fail != 0;
}
