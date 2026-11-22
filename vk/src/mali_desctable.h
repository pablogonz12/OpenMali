/* SPDX-License-Identifier: MIT */
/* VK_EXT_descriptor_indexing stage 3/4 building block: a per-set binding-address table and the write-while-in-use rule.
 * Pure (no device, no hardware). Not wired into the compiler or the command stream yet (compile output unchanged, no version bump);
 * see docs/descidx-plan.md "Descriptor tables". */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* One entry per binding, indexed by binding number (not by position), so a shader reaches any binding with one load instead of
 * a fixed 4/24 slot cap: addr = set_va + (slot + idx * stride) * slot_size. */
struct mali_dtab_entry {
   uint32_t slot;       /* first slot of the binding inside the set */
   uint32_t stride;     /* slots per element */
   uint32_t count;      /* elements (already shrunk to the allocate-time count for a variable binding) */
   uint32_t flags;      /* VkDescriptorBindingFlags */
   bool present;        /* binding exists in the layout */
};

enum mali_dtab_res { MALI_DTAB_OK, MALI_DTAB_NO_BINDING, MALI_DTAB_OOB };

/* Resolve (binding, index) to a slot offset in the set. Out of range or missing bindings never return a slot (robust to
 * non-uniform or garbage indices: the lowering clamps to the null slot on failure). */
static inline enum mali_dtab_res
mali_dtab_slot(const struct mali_dtab_entry *t, uint32_t nentries, uint32_t binding, uint32_t idx, uint32_t *slot_out)
{
   if (binding >= nentries || !t[binding].present) return MALI_DTAB_NO_BINDING;
   if (idx >= t[binding].count) return MALI_DTAB_OOB;
   if (slot_out) *slot_out = t[binding].slot + idx * t[binding].stride;
   return MALI_DTAB_OK;
}

/* Spec 14.2.4: a descriptor may be written while the set is bound/pending only with UPDATE_AFTER_BIND (set bound, not yet
 * submitted... in use by a pending submission) or UPDATE_UNUSED_WHILE_PENDING (pending, if that element is not dynamically used).
 * in_use = the set is referenced by a pending submission. */
static inline bool
mali_dtab_write_allowed(uint32_t binding_flags, bool in_use)
{
   return !in_use || (binding_flags & (VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT));
}

/* PARTIALLY_BOUND: a never-written slot may be skipped by the lowering only if the binding says so; otherwise it must be valid. */
static inline bool
mali_dtab_may_skip_unwritten(uint32_t binding_flags)
{
   return binding_flags & VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
}
