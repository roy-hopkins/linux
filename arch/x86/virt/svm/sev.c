// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD SVM-SEV Host Support.
 *
 * Copyright (C) 2023 Advanced Micro Devices, Inc.
 *
 * Author: Ashish Kalra <ashish.kalra@amd.com>
 *
 */

#include <linux/cc_platform.h>
#include <linux/printk.h>
#include <linux/mm_types.h>
#include <linux/set_memory.h>
#include <linux/memblock.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/cpumask.h>
#include <linux/iommu.h>
#include <linux/amd-iommu.h>

#include <asm/sev.h>
#include <asm/processor.h>
#include <asm/setup.h>
#include <asm/svm.h>
#include <asm/smp.h>
#include <asm/cpu.h>
#include <asm/apic.h>
#include <asm/cpuid.h>
#include <asm/cmdline.h>
#include <asm/iommu.h>

/*
 * The RMP entry format is not architectural. The format is defined in PPR
 * Family 19h Model 01h, Rev B1 processor.
 */
struct rmpentry {
	u64	assigned	: 1,
		pagesize	: 1,
		immutable	: 1,
		rsvd1		: 9,
		gpa		: 39,
		asid		: 10,
		vmsa		: 1,
		validated	: 1,
		rsvd2		: 1;
	u64 rsvd3;
} __packed;

/*
 * The first 16KB from the RMP_BASE is used by the processor for the
 * bookkeeping, the range needs to be added during the RMP entry lookup.
 */
#define RMPTABLE_CPU_BOOKKEEPING_SZ	0x4000

/* Mask to apply to a PFN to get the first PFN of a 2MB page */
#define PFN_PMD_MASK	(~((1ULL << (PMD_SHIFT - PAGE_SHIFT)) - 1))

static struct rmpentry *rmptable_start __ro_after_init;
static u64 rmptable_max_pfn __ro_after_init;

#undef pr_fmt
#define pr_fmt(fmt)	"SEV-SNP: " fmt

static int __mfd_enable(unsigned int cpu)
{
	u64 val;

	if (!cpu_feature_enabled(X86_FEATURE_SEV_SNP))
		return 0;

	rdmsrl(MSR_AMD64_SYSCFG, val);

	val |= MSR_AMD64_SYSCFG_MFDM;

	wrmsrl(MSR_AMD64_SYSCFG, val);

	return 0;
}

static __init void mfd_enable(void *arg)
{
	__mfd_enable(smp_processor_id());
}

static int __snp_enable(unsigned int cpu)
{
	u64 val;

	if (!cpu_feature_enabled(X86_FEATURE_SEV_SNP))
		return 0;

	rdmsrl(MSR_AMD64_SYSCFG, val);

	val |= MSR_AMD64_SYSCFG_SNP_EN;
	val |= MSR_AMD64_SYSCFG_SNP_VMPL_EN;

	wrmsrl(MSR_AMD64_SYSCFG, val);

	return 0;
}

static __init void snp_enable(void *arg)
{
	__snp_enable(smp_processor_id());
}

bool snp_get_rmptable_info(u64 *start, u64 *len)
{
	u64 max_rmp_pfn, calc_rmp_sz, rmp_sz, rmp_base, rmp_end;

	rdmsrl(MSR_AMD64_RMP_BASE, rmp_base);
	rdmsrl(MSR_AMD64_RMP_END, rmp_end);

	if (!rmp_base || !rmp_end) {
		pr_err("Memory for the RMP table has not been reserved by BIOS\n");
		return false;
	}

	rmp_sz = rmp_end - rmp_base + 1;

	/*
	 * Calculate the amount the memory that must be reserved by the BIOS to
	 * address the whole RAM, including the bookkeeping area. The RMP itself
	 * must also be covered.
	 */
	max_rmp_pfn = max_pfn;
	if (PHYS_PFN(rmp_end) > max_pfn)
		max_rmp_pfn = PHYS_PFN(rmp_end);

	calc_rmp_sz = (max_rmp_pfn << 4) + RMPTABLE_CPU_BOOKKEEPING_SZ;

	if (calc_rmp_sz > rmp_sz) {
		pr_err("Memory reserved for the RMP table does not cover full system RAM (expected 0x%llx got 0x%llx)\n",
		       calc_rmp_sz, rmp_sz);
		return false;
	}

	*start = rmp_base;
	*len = rmp_sz;

	return true;
}

static __init int __snp_rmptable_init(void)
{
	u64 rmp_base, rmp_size;
	void *rmp_start;
	u64 val;

	if (!snp_get_rmptable_info(&rmp_base, &rmp_size))
		return 1;

	pr_info("RMP table physical address [0x%016llx - 0x%016llx]\n",
		rmp_base, rmp_base + rmp_size - 1);

	rmp_start = memremap(rmp_base, rmp_size, MEMREMAP_WB);
	if (!rmp_start) {
		pr_err("Failed to map RMP table addr 0x%llx size 0x%llx\n", rmp_base, rmp_size);
		return 1;
	}

	/*
	 * Check if SEV-SNP is already enabled, this can happen in case of
	 * kexec boot.
	 */
	rdmsrl(MSR_AMD64_SYSCFG, val);
	if (val & MSR_AMD64_SYSCFG_SNP_EN)
		goto skip_enable;

	/* Initialize the RMP table to zero */
	memset(rmp_start, 0, rmp_size);

	/* Flush the caches to ensure that data is written before SNP is enabled. */
	wbinvd_on_all_cpus();

	/* MFDM must be enabled on all the CPUs prior to enabling SNP. */
	on_each_cpu(mfd_enable, NULL, 1);

	/* Enable SNP on all CPUs. */
	on_each_cpu(snp_enable, NULL, 1);

skip_enable:
	rmp_start += RMPTABLE_CPU_BOOKKEEPING_SZ;
	rmp_size -= RMPTABLE_CPU_BOOKKEEPING_SZ;

	rmptable_start = (struct rmpentry *)rmp_start;
	rmptable_max_pfn = rmp_size / sizeof(struct rmpentry) - 1;

	return 0;
}

static int __init snp_rmptable_init(void)
{
	int family, model;

	if (!cpu_feature_enabled(X86_FEATURE_SEV_SNP))
		return 0;

	family = boot_cpu_data.x86;
	model  = boot_cpu_data.x86_model;

	/*
	 * RMP table entry format is not architectural and it can vary by processor and
	 * is defined by the per-processor PPR. Restrict SNP support on the known CPU
	 * model and family for which the RMP table entry format is currently defined for.
	 */
	if (family != 0x19 || model > 0xaf)
		goto nosnp;

	if (amd_iommu_snp_enable())
		goto nosnp;

	if (__snp_rmptable_init())
		goto nosnp;

	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "x86/rmptable_init:online", __snp_enable, NULL);

	return 0;

nosnp:
	setup_clear_cpu_cap(X86_FEATURE_SEV_SNP);
	return -ENOSYS;
}

/*
 * This must be called after the PCI subsystem. This is because amd_iommu_snp_enable()
 * is called to ensure the IOMMU supports the SEV-SNP feature, which can only be
 * called after subsys_initcall().
 *
 * NOTE: IOMMU is enforced by SNP to ensure that hypervisor cannot program DMA
 * directly into guest private memory. In case of SNP, the IOMMU ensures that
 * the page(s) used for DMA are hypervisor owned.
 */
fs_initcall(snp_rmptable_init);

static int rmptable_entry(u64 pfn, struct rmpentry *entry)
{
	if (unlikely(pfn > rmptable_max_pfn))
		return -EFAULT;

	*entry = rmptable_start[pfn];

	return 0;
}

static int __snp_lookup_rmpentry(u64 pfn, struct rmpentry *entry, int *level)
{
	struct rmpentry large_entry;
	int ret;

	if (!cpu_feature_enabled(X86_FEATURE_SEV_SNP))
		return -ENXIO;

	ret = rmptable_entry(pfn, entry);
	if (ret)
		return ret;

	/* Read a large RMP entry to get the correct page level used in RMP entry. */
	ret = rmptable_entry(pfn & PFN_PMD_MASK, &large_entry);
	if (ret)
		return ret;

	*level = RMP_TO_X86_PG_LEVEL(large_entry.pagesize);

	return 0;
}

int snp_lookup_rmpentry(u64 pfn, bool *assigned, int *level)
{
	struct rmpentry e;
	int ret;

	ret = __snp_lookup_rmpentry(pfn, &e, level);
	if (ret)
		return ret;

	*assigned = !!e.assigned;
	return 0;
}
EXPORT_SYMBOL_GPL(snp_lookup_rmpentry);
