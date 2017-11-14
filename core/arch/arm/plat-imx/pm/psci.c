// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 * All rights reserved.
 * Copyright 2017 NXP
 *
 */
#include <console.h>
#include <drivers/imx_uart.h>
#include <drivers/imx_wdog.h>
#include <io.h>
#include <imx.h>
#include <imx_pm.h>
#include <imx-regs.h>
#include <kernel/generic_boot.h>
#include <kernel/misc.h>
#include <kernel/panic.h>
#include <kernel/pm_stubs.h>
#include <mm/core_mmu.h>
#include <mm/core_memprot.h>
#include <platform_config.h>
#include <stdint.h>
#include <sm/optee_smc.h>
#include <sm/psci.h>
#include <tee/entry_std.h>
#include <tee/entry_fast.h>

#ifdef CFG_BOOT_SECONDARY_REQUEST
int psci_cpu_on(uint32_t core_idx, uint32_t entry,
		uint32_t context_id __attribute__((unused)))
{
	uint32_t val;
	vaddr_t va = core_mmu_get_va(SRC_BASE, MEM_AREA_IO_SEC);

	if (!va)
		EMSG("No SRC mapping\n");

	if ((core_idx == 0) || (core_idx >= CFG_TEE_CORE_NB_CORE))
		return PSCI_RET_INVALID_PARAMETERS;

	/* set secondary cores' NS entry addresses */
	ns_entry_addrs[core_idx] = entry;

	val = virt_to_phys((void *)TEE_TEXT_VA_START);
#ifdef CFG_MX7
	if (soc_is_imx7ds()) {
		write32(val, va + SRC_GPR1 + core_idx * 8);

		imx_gpcv2_set_core1_pup_by_software();

		/* release secondary core */
		val = read32(va + SRC_A7RCR1);
		val |=  BIT32(BP_SRC_A7RCR1_A7_CORE1_ENABLE +
			      (core_idx - 1));
		write32(val, va + SRC_A7RCR1);

		return PSCI_RET_SUCCESS;
	}
#else
	/* boot secondary cores from OP-TEE load address */
	write32(val, va + SRC_GPR1 + core_idx * 8);

	/* release secondary core */
	val = read32(va + SRC_SCR);
	val |=  BIT32(BP_SRC_SCR_CORE1_ENABLE + (core_idx - 1));
	val |=  BIT32(BP_SRC_SCR_CORE1_RST + (core_idx - 1));
	write32(val, va + SRC_SCR);

	imx_set_src_gpr(core_idx, 0);
#endif
	return PSCI_RET_SUCCESS;
}

int psci_cpu_off(void)
{
	uint32_t core_id;

	core_id = get_core_pos();

	DMSG("core_id: %" PRIu32, core_id);

	psci_armv7_cpu_off();

	imx_set_src_gpr(core_id, UINT32_MAX);

	thread_mask_exceptions(THREAD_EXCP_ALL);

	while (true)
		wfi();

	return PSCI_RET_INTERNAL_FAILURE;
}

int psci_affinity_info(uint32_t affinity,
		       uint32_t lowest_affnity_level __unused)
{
	vaddr_t va = core_mmu_get_va(SRC_BASE, MEM_AREA_IO_SEC);
	vaddr_t gpr5 = core_mmu_get_va(IOMUXC_BASE, MEM_AREA_IO_SEC) +
				       IOMUXC_GPR5_OFFSET;
	uint32_t cpu, val;
	bool wfi;

	cpu = affinity;

	if (soc_is_imx7ds())
		wfi = true;
	else
		wfi = read32(gpr5) & ARM_WFI_STAT_MASK(cpu);

	if ((imx_get_src_gpr(cpu) == 0) || !wfi)
		return PSCI_AFFINITY_LEVEL_ON;

	DMSG("cpu: %" PRIu32 "GPR: %" PRIx32, cpu, imx_get_src_gpr(cpu));
	/*
	 * Wait secondary cpus ready to be killed
	 * TODO: Change to non dead loop
	 */
#ifdef CFG_MX7
	if (soc_is_imx7ds()) {
		while (read32(va + SRC_GPR1 + cpu * 8 + 4) != UINT_MAX)
			;

		val = read32(va + SRC_A7RCR1);
		val &=  ~BIT32(BP_SRC_A7RCR1_A7_CORE1_ENABLE + (cpu - 1));
		write32(val, va + SRC_A7RCR1);
	}
#else
	while (read32(va + SRC_GPR1 + cpu * 8 + 4) != UINT32_MAX)
		;

	/* Kill cpu */
	val = read32(va + SRC_SCR);
	val &= ~BIT32(BP_SRC_SCR_CORE1_ENABLE + cpu - 1);
	val |=  BIT32(BP_SRC_SCR_CORE1_RST + cpu - 1);
	write32(val, va + SRC_SCR);
#endif

	/* Clean arg */
	imx_set_src_gpr(cpu, 0);

	return PSCI_AFFINITY_LEVEL_OFF;
}
#endif

void psci_system_reset(void)
{
	imx_wdog_restart();
}

#if defined(CFG_BOOT_SYNC_CPU)
void pcsi_boot_allcpus(void)
{
	vaddr_t src_base = core_mmu_get_va(SRC_BASE, MEM_AREA_TEE_COHERENT);

	/* set secondary entry address and release core */
	write32(CFG_TEE_LOAD_ADDR, src_base + SRC_GPR1 + 8);
	write32(CFG_TEE_LOAD_ADDR, src_base + SRC_GPR1 + 16);
	write32(CFG_TEE_LOAD_ADDR, src_base + SRC_GPR1 + 24);

	write32(SRC_SCR_CPU_ENABLE_ALL, src_base + SRC_SCR);
}
#endif

__weak int imx7_cpu_suspend(uint32_t power_state __unused,
			    uintptr_t entry __unused,
			    uint32_t context_id __unused,
			    struct sm_nsec_ctx *nsec __unused)
{
	return 0;
}

int psci_cpu_suspend(uint32_t power_state,
		     uintptr_t entry, uint32_t context_id __unused,
		     struct sm_nsec_ctx *nsec)
{
	uint32_t id, type;
	int ret = PSCI_RET_INVALID_PARAMETERS;

	id = power_state & PSCI_POWER_STATE_ID_MASK;
	type = (power_state & PSCI_POWER_STATE_TYPE_MASK) >>
		PSCI_POWER_STATE_TYPE_SHIFT;

	if ((type != PSCI_POWER_STATE_TYPE_POWER_DOWN) &&
	    (type != PSCI_POWER_STATE_TYPE_STANDBY)) {
		DMSG("Not supported %x\n", type);
		return ret;
	}

	/*
	 * ID 0 means suspend
	 * ID 1 means low power idle
	 * TODO: follow PSCI StateID sample encoding.
	 */
	DMSG("ID = %d\n", id);
	if (id == 1) {
		/* Not supported now */
		return ret;
	} else if (id == 0) {
		if (soc_is_imx7ds()) {
			return imx7_cpu_suspend(power_state, entry,
						context_id, nsec);
		}
		return ret;
	}

	DMSG("ID %d not supported\n", id);

	return ret;
}
