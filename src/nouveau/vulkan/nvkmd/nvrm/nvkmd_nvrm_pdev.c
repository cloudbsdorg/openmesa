/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: MIT
 *
 * CloudBSD private fork (see FORK-NOTES.md at the top of the tree).
 *
 * nvkmd "nvrm" backend, physical device.  STUB: see nvkmd_nvrm.h.
 */
#include "nvkmd_nvrm.h"

#include "util/cache_ops.h"
#include "util/log.h"
#include "util/os_misc.h"
#include "util/u_atomic.h"
#include "util/u_memory.h"

#include "vk_log.h"
#include "vk_sync_dummy.h"

#include "cla040.h"
#include "cla140.h"
#include "cl902d.h"

#include <nvrm/nvrm.h>

#include <stdio.h>
#include <string.h>

struct nvkmd_nvrm_enum {
   uint32_t refcnt;
   nvrm_ctx *ctx;
};

struct nvkmd_nvrm_pdev {
   struct nvkmd_pdev base;

   /* Keeps the nvrm_device behind this pdev alive. */
   struct nvkmd_nvrm_enum *nvenum;
   nvrm_device *dev;

   /* The stub creates no device, so no real sync type is ever used.  The
    * runtime still needs a list to answer external-sync queries with "not
    * exportable".  The libnvrm S4 timeline type replaces this (#2438).
    */
   const struct vk_sync_type *sync_types[2];
};

NVKMD_DECL_SUBCLASS(pdev, nvrm);

/* Chipset ids NVK knows by number: architecture plus implementation, the way
 * the RM reports them (GB206 = 0x1b0 + 0x6).
 */
#define NVKMD_NVRM_CHIPSET_GB10 0x1bb

static struct nvkmd_nvrm_enum *
nvkmd_nvrm_enum_ref(struct nvkmd_nvrm_enum *nvenum)
{
   p_atomic_inc(&nvenum->refcnt);
   return nvenum;
}

void
nvkmd_nvrm_enum_unref(struct nvkmd_nvrm_enum *nvenum)
{
   if (nvenum == NULL)
      return;

   if (p_atomic_dec_zero(&nvenum->refcnt)) {
      nvrm_close(nvenum->ctx);
      FREE(nvenum);
   }
}

VkResult
nvkmd_nvrm_enum_open(struct vk_object_base *log_obj,
                     enum nvk_debug debug_flags,
                     struct nvkmd_nvrm_enum **enum_out)
{
   char err[256] = "";
   nvrm_ctx *ctx = NULL;

   /* Least privilege: reading device information needs no allocation or
    * mapping rights.  The device open in libnvrm S2 asks for its own.
    */
   const int ret = nvrm_enumerate(&ctx, NULL, err, sizeof(err));
   if (ret != NVRM_OK) {
      /* No nvidia driver loaded is the common case on a machine without the
       * GPU; it is not an error for the loader, so the GPU list is empty.
       */
      mesa_logd("nvk-nvrm: libnvrm enumeration failed (%s): %s",
                nvrm_error_id(ret), err[0] ? err : nvrm_strerror(ret));
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   struct nvkmd_nvrm_enum *nvenum = CALLOC_STRUCT(nvkmd_nvrm_enum);
   if (nvenum == NULL) {
      nvrm_close(ctx);
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   nvenum->refcnt = 1;
   nvenum->ctx = ctx;
   *enum_out = nvenum;

   return VK_SUCCESS;
}

uint32_t
nvkmd_nvrm_enum_count(const struct nvkmd_nvrm_enum *nvenum)
{
   return nvrm_device_count(nvenum->ctx);
}

/* Per-SM limits for the generations this stub supports.  The values are the
 * occupancy limits NVIDIA publishes for compute capability 12.x (CUDA C++
 * Programming Guide, "Technical Specifications per Compute Capability").
 * libnvrm does not report them yet (#2439); until it does, a GPU outside this
 * table is skipped rather than given guessed limits.
 */
struct nvkmd_nvrm_sm_limits {
   uint8_t sm;
   uint8_t max_warps_per_mp;
   uint8_t max_blocks_per_mp;
   uint16_t max_smem_per_wg_kB;
   uint8_t smem_size_count;
   /* CloudBSD fork: keep the source and destination array sizes in sync. */
   uint16_t smem_sizes_kB[
      ARRAY_SIZE(((struct nv_device_info *)0)->sm_smem_sizes_kB)];
};

static const struct nvkmd_nvrm_sm_limits nvkmd_nvrm_sm_limits[] = {
   { 120, 48, 32, 99, 6, { 0, 8, 16, 32, 64, 100 } },
   { 121, 48, 32, 99, 6, { 0, 8, 16, 32, 64, 100 } },
};

static const struct nvkmd_nvrm_sm_limits *
nvkmd_nvrm_find_sm_limits(uint8_t sm)
{
   for (unsigned i = 0; i < ARRAY_SIZE(nvkmd_nvrm_sm_limits); i++) {
      if (nvkmd_nvrm_sm_limits[i].sm == sm)
         return &nvkmd_nvrm_sm_limits[i];
   }
   return NULL;
}

static bool
nvkmd_nvrm_has_class(const nvrm_device_info *info, uint64_t cls)
{
   if (!NVRM_HAVE(info->classes))
      return false;

   for (uint32_t i = 0; i < info->classes.count; i++) {
      if (info->classes.value[i] == cls)
         return true;
   }
   return false;
}

static VkResult
nvkmd_nvrm_skip(uint32_t index, const char *why)
{
   mesa_logw("nvk-nvrm: GPU %u skipped: %s", index, why);
   return VK_ERROR_INCOMPATIBLE_DRIVER;
}

/* The values NVK needs must all be present, else the GPU is skipped. */
static VkResult
nvkmd_nvrm_check_required(uint32_t index, const nvrm_device_info *info)
{
   if (!NVRM_HAVE(info->architecture) || !NVRM_HAVE(info->implementation))
      return nvkmd_nvrm_skip(index, "no architecture or implementation");
   if (!NVRM_HAVE(info->sm_version_major) ||
       !NVRM_HAVE(info->sm_version_minor))
      return nvkmd_nvrm_skip(index, "no SM version");
   if (!NVRM_HAVE(info->gpc_count) || !NVRM_HAVE(info->tpc_count) ||
       !NVRM_HAVE(info->sm_count))
      return nvkmd_nvrm_skip(index, "no GPC, TPC or SM count");
   if (!NVRM_HAVE(info->cls_gpfifo) || !NVRM_HAVE(info->cls_compute) ||
       !NVRM_HAVE(info->cls_eng3d))
      return nvkmd_nvrm_skip(index, "no GPFIFO, compute or 3D class");
   if (!NVRM_HAVE(info->pci_device))
      return nvkmd_nvrm_skip(index, "no PCI device id");
   return VK_SUCCESS;
}

/* Chipset, device type, VRAM and PCI address. */
static VkResult
nvkmd_nvrm_fill_type_pci_vram(uint32_t index, const nvrm_device_info *info,
                              struct nv_device_info *out)
{
   out->chipset = info->architecture.value | info->implementation.value;
   out->device_id = info->pci_device.value;

   /* SoC only on a positive signal: the GB10 id or the RM SoC flag
    * (DESIGN.md note 6.1).
    */
   const nvrm_u64 *soc = &info->caps[4];
   const bool is_soc = out->chipset == NVKMD_NVRM_CHIPSET_GB10 ||
                       (NVRM_HAVE(*soc) && soc->value != 0);
   out->type = is_soc ? NV_DEVICE_TYPE_SOC : NV_DEVICE_TYPE_DIS;

   out->vram_size_B = NVRM_HAVE(info->vram_size) ? info->vram_size.value : 0;
   if (!is_soc && out->vram_size_B == 0)
      return nvkmd_nvrm_skip(index, "a discrete GPU that reports no VRAM");

   if (!is_soc) {
      if (!NVRM_HAVE(info->pci_domain) || !NVRM_HAVE(info->pci_bus) ||
          !NVRM_HAVE(info->pci_slot) || !NVRM_HAVE(info->pci_function))
         return nvkmd_nvrm_skip(index, "no PCI address");
      out->pci.domain = info->pci_domain.value;
      out->pci.bus = info->pci_bus.value;
      out->pci.dev = info->pci_slot.value;
      out->pci.func = info->pci_function.value;
      out->pci.revision_id =
         NVRM_HAVE(info->pci_revision) ? info->pci_revision.value : 0;
   }
   return VK_SUCCESS;
}

/* Name strings, SM, TPC and the per-SM limits. */
static VkResult
nvkmd_nvrm_fill_names_sm(uint32_t index, const nvrm_device_info *info,
                         struct nv_device_info *out)
{
   /* The RM gives no marketing name yet (#2439).  NVK appends
    * " (NVK <chipset_name>)" to the device name, so this reads
    * "NVIDIA libnvrm stub (NVK NV1B6)".
    */
   snprintf(out->chipset_name, sizeof(out->chipset_name), "NV%03X",
            out->chipset);
   snprintf(out->device_name, sizeof(out->device_name),
            "NVIDIA libnvrm stub");

   out->sm = info->sm_version_major.value * 10 + info->sm_version_minor.value;
   const struct nvkmd_nvrm_sm_limits *lim = nvkmd_nvrm_find_sm_limits(out->sm);
   if (lim == NULL)
      return nvkmd_nvrm_skip(index, "no per-SM limits for this SM version yet");

   out->gpc_count = info->gpc_count.value;
   out->tpc_count = info->tpc_count.value;
   if (out->tpc_count == 0 || info->sm_count.value % out->tpc_count != 0)
      return nvkmd_nvrm_skip(index, "SM count is not a multiple of TPC count");
   out->mp_per_tpc = info->sm_count.value / out->tpc_count;

   out->max_warps_per_mp = lim->max_warps_per_mp;
   out->max_blocks_per_mp = lim->max_blocks_per_mp;
   out->max_smem_per_wg_kB = lim->max_smem_per_wg_kB;
   out->sm_smem_size_count = lim->smem_size_count;
   memcpy(out->sm_smem_sizes_kB, lim->smem_sizes_kB,
          sizeof(out->sm_smem_sizes_kB));
   return VK_SUCCESS;
}

/* Classes, queues, video, atom size and the BAR. */
static void
nvkmd_nvrm_fill_classes_bar(const nvrm_device_info *info,
                            struct nv_device_info *out)
{
   out->cls_gpfifo = info->cls_gpfifo.value;
   out->cls_compute = info->cls_compute.value;
   out->cls_eng3d = info->cls_eng3d.value;
   out->cls_copy = NVRM_HAVE(info->cls_copy) ? info->cls_copy.value : 0;

   /* 2D and inline-to-memory come from the class list.  Which classes the RM
    * lists for them on Blackwell is to be checked on hardware (#2439).
    */
   if (nvkmd_nvrm_has_class(info, FERMI_TWOD_A))
      out->cls_eng2d = FERMI_TWOD_A;
   if (nvkmd_nvrm_has_class(info, KEPLER_INLINE_TO_MEMORY_B))
      out->cls_m2mf = KEPLER_INLINE_TO_MEMORY_B;
   else if (nvkmd_nvrm_has_class(info, KEPLER_INLINE_TO_MEMORY_A))
      out->cls_m2mf = KEPLER_INLINE_TO_MEMORY_A;
   out->cls_vdec = 0;

   out->has_transfer_queue = out->cls_copy != 0;
   out->has_video = false;

   out->nc_atom_size_B = util_cache_granularity();

   /* Fail closed on the host-visible VRAM heap: report the BAR only with a
    * positive signal that CPU maps of it allow any access.  On arm64 that is
    * the OS layer reporting write-combined device maps (capability 6).
    * Without it, VRAM is device-local and not host-visible (DESIGN.md 6.2).
    */
   const nvrm_u64 *wc = &info->caps[6];
   if (out->type != NV_DEVICE_TYPE_SOC && NVRM_HAVE(*wc) && wc->value != 0 &&
       NVRM_HAVE(info->bar1_size))
      out->bar_size_B = info->bar1_size.value;
}

/* Fills dev_info from libnvrm.  Returns VK_ERROR_INCOMPATIBLE_DRIVER, with a
 * logged reason, when a value NVK needs is not available.
 */
static VkResult
nvkmd_nvrm_fill_dev_info(uint32_t index, const nvrm_device_info *info,
                         struct nv_device_info *out)
{
   VkResult result = nvkmd_nvrm_check_required(index, info);
   if (result != VK_SUCCESS)
      return result;

   memset(out, 0, sizeof(*out));

   result = nvkmd_nvrm_fill_type_pci_vram(index, info, out);
   if (result != VK_SUCCESS)
      return result;

   result = nvkmd_nvrm_fill_names_sm(index, info, out);
   if (result != VK_SUCCESS)
      return result;

   nvkmd_nvrm_fill_classes_bar(info, out);

   out->has_zcull_info = false;

   return VK_SUCCESS;
}

static void
nvkmd_nvrm_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(_pdev);

   nvkmd_nvrm_enum_unref(pdev->nvenum);
   FREE(pdev);
}

static uint64_t
nvkmd_nvrm_pdev_get_vram_used(struct nvkmd_pdev *pdev)
{
   /* has_get_vram_used is false; NVK does not call this. */
   return 0;
}

static VkResult
nvkmd_nvrm_pdev_create_dev(struct nvkmd_pdev *pdev,
                           struct vk_object_base *log_obj,
                           struct nvkmd_dev **dev_out)
{
   mesa_loge("%s", NVK_NVRM_STUB_MSG);
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

const struct nvkmd_pdev_ops nvkmd_nvrm_pdev_ops = {
   .destroy = nvkmd_nvrm_pdev_destroy,
   .get_vram_used = nvkmd_nvrm_pdev_get_vram_used,
   .get_drm_primary_fd = NULL,
   .create_dev = nvkmd_nvrm_pdev_create_dev,
};

VkResult
nvkmd_nvrm_enum_create_pdev(struct nvkmd_nvrm_enum *nvenum,
                            uint32_t index,
                            struct vk_object_base *log_obj,
                            enum nvk_debug debug_flags,
                            struct nvkmd_pdev **pdev_out)
{
   nvrm_device *dev = nvrm_device_get(nvenum->ctx, index);
   if (dev == NULL)
      return nvkmd_nvrm_skip(index, "libnvrm has no device at this index");

   const nvrm_device_info *info = nvrm_device_info_get(dev);
   if (info == NULL)
      return nvkmd_nvrm_skip(index, "libnvrm has no device information");

   struct nvkmd_nvrm_pdev *pdev = CALLOC_STRUCT(nvkmd_nvrm_pdev);
   if (pdev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = nvkmd_nvrm_fill_dev_info(index, info,
                                              &pdev->base.dev_info);
   if (result != VK_SUCCESS) {
      FREE(pdev);
      return result;
   }

   pdev->base.ops = &nvkmd_nvrm_pdev_ops;
   pdev->base.debug_flags = debug_flags;

   /* The stub implements none of the optional nvkmd features. */
   pdev->base.kmd_info = (struct nvkmd_info) { 0 };

   uint64_t os_page_size;
   if (!os_get_page_size(&os_page_size) || os_page_size > UINT32_MAX)
      os_page_size = 4096;
   pdev->base.bind_align_B = os_page_size;

   /* No DRM device: VK_EXT_physical_device_drm is not reported. */
   pdev->base.drm.render_dev = 0;
   pdev->base.drm.primary_dev = 0;

   pdev->sync_types[0] = &vk_sync_dummy_type;
   pdev->sync_types[1] = NULL;
   pdev->base.sync_types = pdev->sync_types;

   pdev->nvenum = nvkmd_nvrm_enum_ref(nvenum);
   pdev->dev = dev;

   *pdev_out = &pdev->base;

   return VK_SUCCESS;
}
