/* SPDX-License-Identifier: MIT */
/* Is this descriptor a dma-buf? Two arms, because one alone is wrong on a platform we ship to. */
#pragma once

#include <stdbool.h>

bool mali_dmabuf_probe(int fd);
