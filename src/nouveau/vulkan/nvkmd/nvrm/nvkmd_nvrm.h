/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: MIT
 *
 * CloudBSD private fork (see FORK-NOTES.md at the top of the tree).
 *
 * nvkmd "nvrm" backend: NVK on NVIDIA's open RM through libnvrm, with no
 * nouveau kernel driver and no DRM render node.  Design: nvidia-gb10-freebsd
 * mesa/nvk-nvrm/DESIGN.md (Track #2379).
 *
 * THIS IS A STUB.  It enumerates GPUs and reports their properties.  It cannot
 * create a device: memory, VA and submission need libnvrm S2 (#2419), S3
 * (#2437) and S4 (#2438), so vkCreateDevice returns
 * VK_ERROR_FEATURE_NOT_PRESENT and logs NVK_NVRM_STUB_MSG.
 */
#ifndef NVKMD_NVRM_H
#define NVKMD_NVRM_H 1

#include "nvkmd/nvkmd.h"

#include "nvkmd_nvrm_msg.h"

/* One libnvrm enumeration, shared by the pdevs made from it. */
struct nvkmd_nvrm_enum;

VkResult nvkmd_nvrm_enum_open(struct vk_object_base *log_obj,
                              enum nvk_debug debug_flags,
                              struct nvkmd_nvrm_enum **enum_out);

uint32_t nvkmd_nvrm_enum_count(const struct nvkmd_nvrm_enum *nvenum);

/* VK_ERROR_INCOMPATIBLE_DRIVER means "skip this GPU", as for DRM devices. */
VkResult nvkmd_nvrm_enum_create_pdev(struct nvkmd_nvrm_enum *nvenum,
                                     uint32_t index,
                                     struct vk_object_base *log_obj,
                                     enum nvk_debug debug_flags,
                                     struct nvkmd_pdev **pdev_out);

void nvkmd_nvrm_enum_unref(struct nvkmd_nvrm_enum *nvenum);

#endif /* NVKMD_NVRM_H */
