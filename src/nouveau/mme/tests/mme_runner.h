/*
 * Copyright © 2022 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */
#include <gtest/gtest.h>
#include <vector>

#include "mme_builder.h"

// CloudBSD fork: nouveau winsys declarations removed; NVK runs on libnvrm.
#include "nv_push.h"
#include "nv_push_cl9097.h"
#include "nv_push_clc7c0.h"

#define DATA_BO_SIZE 4096
#define DATA_DWORDS 1024

class mme_runner {
public:
   mme_runner();
   virtual ~mme_runner();

   virtual void run_macro(const std::vector<uint32_t>& macro,
                          const std::vector<uint32_t>& params) = 0;

   void mme_store_data(mme_builder *b, uint32_t dw_idx,
                       mme_value data, bool free_reg = false);

   const nv_device_info *devinfo;
   uint64_t data_addr;
   uint32_t *data;
};

// CloudBSD fork: nouveau hardware MME runner removed; NVK runs on libnvrm.
class mme_fermi_sim_runner : public mme_runner {
public:
   mme_fermi_sim_runner(uint64_t data_addr);
   virtual ~mme_fermi_sim_runner();

   virtual void run_macro(const std::vector<uint32_t>& macro,
                          const std::vector<uint32_t>& params);

private:
   struct nv_device_info info;
   uint32_t data_store[DATA_DWORDS];
};

class mme_tu104_sim_runner : public mme_runner {
public:
   mme_tu104_sim_runner(uint64_t data_addr);
   virtual ~mme_tu104_sim_runner();

   virtual void run_macro(const std::vector<uint32_t>& macro,
                          const std::vector<uint32_t>& params);

private:
   struct nv_device_info info;
   uint32_t data_store[DATA_DWORDS];
};

inline std::vector<uint32_t>
mme_builder_finish_vec(mme_builder *b)
{
   size_t size = 0;
   uint32_t *dw = mme_builder_finish(b, &size);
   std::vector<uint32_t> vec(dw, dw + (size / 4));
   free(dw);
   return vec;
}

inline uint32_t
high32(uint64_t x)
{
   return (uint32_t)(x >> 32);
}

inline uint32_t
low32(uint64_t x)
{
   return (uint32_t)x;
}

inline void
mme_store_imm_addr(mme_builder *b, uint64_t addr, mme_value v,
                   bool free_reg = false)
{
   mme_mthd(b, NV9097_SET_REPORT_SEMAPHORE_A);
   mme_emit(b, mme_imm(high32(addr)));
   mme_emit(b, mme_imm(low32(addr)));
   mme_emit(b, v);
   mme_emit(b, mme_imm(0x10000000));

   if (free_reg && v.type == MME_VALUE_TYPE_REG)
      mme_free_reg(b, v);
}

inline void
mme_store(mme_builder *b, struct mme_value64 addr, mme_value v,
          bool free_reg = false)
{
   mme_mthd(b, NV9097_SET_REPORT_SEMAPHORE_A);
   mme_emit(b, addr.hi);
   mme_emit(b, addr.lo);
   mme_emit(b, v);
   mme_emit(b, mme_imm(0x10000000));

   if (free_reg && v.type == MME_VALUE_TYPE_REG)
      mme_free_reg(b, v);
}

inline void
mme_store_compute_imm_addr(mme_builder *b, uint64_t addr, mme_value v,
                           bool free_reg = false)
{
   mme_mthd(b, NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_LOWER);
   mme_emit(b, v);
   mme_emit(b, mme_imm(0));
   mme_emit(b, mme_imm(low32(addr)));
   mme_emit(b, mme_imm(high32(addr)));
   mme_emit(b, mme_imm(0x8));

   if (free_reg && v.type == MME_VALUE_TYPE_REG)
      mme_free_reg(b, v);
}

inline void
mme_store_compute(mme_builder *b, struct mme_value64 addr, mme_value v,
                  bool free_reg = false)
{
   mme_mthd(b, NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_LOWER);
   mme_emit(b, v);
   mme_emit(b, mme_imm(0));
   mme_emit(b, addr.lo);
   mme_emit(b, addr.hi);
   mme_emit(b, mme_imm(0x8));

   if (free_reg && v.type == MME_VALUE_TYPE_REG)
      mme_free_reg(b, v);
}