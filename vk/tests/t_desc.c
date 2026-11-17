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
   VkResult r = mali_create_descriptor_pool(&g_dev, arch, &pi, poolout);
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

   mali_destroy_descriptor_pool(&g_dev, pool);
   mali_destroy_descriptor_pool(&g_dev, pool2);
   mali_destroy_descriptor_pool(&g_dev, pool3);
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
   CHECK(mali_create_descriptor_pool(&g_dev, 11, &pi, &pool) == VK_SUCCESS, "pool");
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
   mali_destroy_descriptor_pool(&g_dev, pool);
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
   test_compile(11);
   test_compile(13);
   clock_gettime(CLOCK_MONOTONIC, &b);
   printf("%s (%d failures, %.2f s)\n", g_fail ? "FAIL" : "PASS", g_fail, (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9);
   return g_fail != 0;
}
