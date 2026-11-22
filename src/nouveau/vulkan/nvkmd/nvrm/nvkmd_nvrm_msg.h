/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: MIT
 *
 * CloudBSD private fork (see FORK-NOTES.md at the top of the tree).
 *
 * The one source of the nvrm stub's log line.  The backend logs it, and the
 * device probe in cloudbsd/tests includes this header to check for it, so
 * the text is never copied.  Plain C, no Mesa headers.
 */
#ifndef NVKMD_NVRM_MSG_H
#define NVKMD_NVRM_MSG_H 1

#define NVK_NVRM_STUB_MSG                                                    \
   "nvk-nvrm: this build can list NVIDIA GPUs but cannot create a Vulkan "  \
   "device yet (queues are not implemented); to use another driver, set "   \
   "VK_LOADER_DRIVERS_SELECT or VK_ICD_FILENAMES"

#endif /* NVKMD_NVRM_MSG_H */
