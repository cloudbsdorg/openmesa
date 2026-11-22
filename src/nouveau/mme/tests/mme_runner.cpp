/*
 * Copyright © 2022 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */
#include "mme_runner.h"

#include <string.h>

#include "mme_fermi_sim.h"
#include "mme_tu104_sim.h"

/* CloudBSD fork: nouveau winsys and DRM includes removed; NVK runs on
 * libnvrm.
 */

mme_runner::mme_runner() :
  devinfo(NULL), data_addr(0), data(NULL)
{ }

mme_runner::~mme_runner()
{ }

/* CloudBSD fork: nouveau hardware-runner construction removed; NVK runs on
 * libnvrm.
 */
void
mme_runner::mme_store_data(mme_builder *b, uint32_t dw_idx,
                           mme_value data, bool free_reg)
{
   mme_store_imm_addr(b, data_addr + dw_idx * 4, data, free_reg);
}

/* CloudBSD fork: nouveau hardware MME execution removed; NVK runs on libnvrm.
 * The simulation runners below remain and do not use the winsys.
 */
/* CPD-OFF -- upstream Mesa: the Fermi and Turing sim runners are parallel on purpose */
mme_fermi_sim_runner::mme_fermi_sim_runner(uint64_t data_addr)
{
   memset(&info, 0, sizeof(info));
   info.cls_eng3d = FERMI_A;

   memset(data_store, 0, sizeof(data_store));

   this->devinfo = &info;
   this->data_addr = data_addr,
   this->data = data_store;
}

mme_fermi_sim_runner::~mme_fermi_sim_runner()
{ }

void
mme_fermi_sim_runner::run_macro(const std::vector<uint32_t>& macro,
                                const std::vector<uint32_t>& params)
{
   std::vector<mme_fermi_inst> insts(macro.size());
   mme_fermi_decode(&insts[0], &macro[0], macro.size());

   /* First, make a copy of the data and simulate the macro */
   mme_fermi_sim_mem sim_mem = {
      .addr = data_addr,
      .data = data,
      .size = DATA_BO_SIZE,
   };
   const uint32_t* p_params = params.size() ? &params[0] : NULL;
   mme_fermi_sim(insts.size(), &insts[0],
                 params.size(), p_params,
                 1, &sim_mem);
}

mme_tu104_sim_runner::mme_tu104_sim_runner(uint64_t data_addr)
{
   memset(&info, 0, sizeof(info));
   info.cls_eng3d = TURING_A;

   memset(data_store, 0, sizeof(data_store));

   this->devinfo = &info;
   this->data_addr = data_addr,
   this->data = data_store;
}

mme_tu104_sim_runner::~mme_tu104_sim_runner()
{ }

void
mme_tu104_sim_runner::run_macro(const std::vector<uint32_t>& macro,
                               const std::vector<uint32_t>& params)
{
   std::vector<mme_tu104_inst> insts(macro.size());
   mme_tu104_decode(&insts[0], &macro[0], macro.size() / 3);

   /* First, make a copy of the data and simulate the macro */
   mme_tu104_sim_mem sim_mem = {
      .addr = data_addr,
      .data = data,
      .size = DATA_BO_SIZE,
   };
   const uint32_t* p_params = params.size() ? &params[0] : NULL;
   mme_tu104_sim(insts.size(), &insts[0], params.size(), p_params, 1, &sim_mem,
                 devinfo->cls_eng3d);
}
/* CPD-ON */
