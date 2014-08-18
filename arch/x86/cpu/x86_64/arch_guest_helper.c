/**
 * Copyright (c) 2014 Himanshu Chauhan
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * @file arch_guest_helper.c
 * @author Himanshu Chauhan (hschauhan@nulltrace.org)
 * @brief Guest management functions.
 */

#include <vmm_error.h>
#include <vmm_heap.h>
#include <vmm_stdio.h>
#include <vmm_manager.h>
#include <vmm_guest_aspace.h>
#include <vmm_host_aspace.h>
#include <vmm_macros.h>
#include <cpu_mmu.h>
#include <cpu_features.h>
#include <cpu_vm.h>
#include <vm/amd_svm.h>
#include <libs/stringlib.h>
#include <arch_guest_helper.h>

int arch_guest_init(struct vmm_guest * guest)
{
	struct x86_guest_priv *priv = vmm_zalloc(sizeof(struct x86_guest_priv));

	if (priv == NULL) {
		VM_LOG(LVL_ERR, "ERROR: Failed to create guest private data.\n");
		return VMM_EFAIL;
	}

	/*
	 * Create the nested paging table that map guest physical to host physical
	 * return the (host) physical base address of the table.
	 * Note: Nested paging table must use the same paging mode as the host,
	 * regardless of guest paging mode - See AMD man vol2:
	 * The extra translation uses the same paging mode as the VMM used when it
	 * executed the most recent VMRUN.
	 *
	 * Also, it is important to note that gCR3 and the guest page table entries
	 * contain guest physical addresses, not system physical addresses.
	 * Hence, before accessing a guest page table entry, the table walker first
	 * translates that entry's guest physical address into a system physical
	 * address.
	 *
	 * npt should be created should be used when nested page table
	 * walking is available. But we will create it anyways since
	 * we are creating only the first level.
	 */
	priv->g_npt = mmu_pgtbl_alloc(&host_pgtbl_ctl, PGTBL_STAGE_2);

	if (priv->g_npt == NULL) {
		VM_LOG(LVL_ERR, "ERROR: Failed to create nested page table for guest.\n");
		vmm_free(priv);
		return VMM_EFAIL;
	}

	guest->arch_priv = (void *)priv;

	VM_LOG(LVL_VERBOSE, "Guest init successful!\n");
	return VMM_OK;
}

int arch_guest_deinit(struct vmm_guest * guest)
{
	struct x86_guest_priv *priv = x86_guest_priv(guest);

	if (priv) {
		if (mmu_pgtbl_free(&host_pgtbl_ctl, priv->g_npt) != VMM_OK)
			VM_LOG(LVL_ERR, "ERROR: Failed to unmap the nested page table. Will Leak.\n");

		vmm_free(priv);
	}

	return VMM_OK;
}

int arch_guest_add_region(struct vmm_guest *guest, struct vmm_region *region)
{
	struct vmm_vcpu *vcpu;
	u32 i, flags;

	if (region->flags & VMM_REGION_IO) {
		u32 reg_end = region->gphys_addr + region->phys_size;

		for (i = region->gphys_addr; i < reg_end; i++) {
			vmm_read_lock_irqsave_lite(&guest->vcpu_lock, flags);

			list_for_each_entry(vcpu, &guest->vcpu_list, head) {
				enable_ioport_intercept(x86_vcpu_priv(vcpu)->hw_context, i);
			}

			vmm_read_unlock_irqrestore_lite(&guest->vcpu_lock, flags);
		}
	} else if ((region->flags & VMM_REGION_MEMORY)
		   && (region->flags & VMM_REGION_REAL)) {
		struct x86_guest_priv *priv = x86_guest_priv(guest);

		/* += ? Multiple memory regions may be */
		priv->tot_ram_sz += region->phys_size;
	}

	return VMM_OK;
}

int arch_guest_del_region(struct vmm_guest *guest, struct vmm_region *region)
{
	struct vmm_vcpu *vcpu;
	u32 i, flags;

	if (region->flags & VMM_REGION_IO) {
		u32 reg_end = region->gphys_addr + region->phys_size;

		for (i = region->gphys_addr; i < reg_end; i++) {
			vmm_read_lock_irqsave_lite(&guest->vcpu_lock, flags);

			list_for_each_entry(vcpu, &guest->vcpu_list, head) {
				disable_ioport_intercept(x86_vcpu_priv(vcpu)->hw_context, i);
			}

			vmm_read_unlock_irqrestore_lite(&guest->vcpu_lock, flags);
		}
	} else if (region->flags & (VMM_REGION_REAL | VMM_REGION_MEMORY)) {
		struct x86_guest_priv *priv = x86_guest_priv(guest);

		if (priv->tot_ram_sz && priv->tot_ram_sz >= region->phys_size)
			/* += ? Multiple memory regions may be */
			priv->tot_ram_sz += region->phys_size;
	}

	return VMM_OK;
}

static void guest_cmos_init(struct vmm_guest *guest)
{
	int val;
	struct x86_guest_priv *priv = x86_guest_priv(guest);
	struct cmos_rtc_state *s = priv->rtc_cmos;

	/* memory size */
	/* base memory (first MiB) */
	val = min((int)(priv->tot_ram_sz / 1024), 640);

	s->rtc_cmos_write(s, RTC_REG_BASE_MEM_LO, val);
	s->rtc_cmos_write(s, RTC_REG_BASE_MEM_HI, val >> 8);

	/* extended memory (next 64MiB) */
	if (priv->tot_ram_sz > 1024 * 1024) {
		val = (priv->tot_ram_sz - 1024 * 1024) / 1024;
	} else {
		val = 0;
	}
	if (val > 65535)
		val = 65535;
	s->rtc_cmos_write(s, RTC_REG_EXT_MEM_LO, val);
	s->rtc_cmos_write(s, RTC_REG_EXT_MEM_HI, val >> 8);
	s->rtc_cmos_write(s, RTC_REG_EXT_MEM_LO_COPY, val);
	s->rtc_cmos_write(s, RTC_REG_EXT_MEM_HI_COPY, val >> 8);

	/* memory between 16MiB and 4GiB */
	if (priv->tot_ram_sz > 16 * 1024 * 1024) {
		val = (priv->tot_ram_sz - 16 * 1024 * 1024) / 65536;
	} else {
		val = 0;
	}
	if (val > 65535)
		val = 65535;
	s->rtc_cmos_write(s, RTC_REG_EXT_MEM_64K_LO, val);
	s->rtc_cmos_write(s, RTC_REG_EXT_MEM_64K_HI, val >> 8);

	/* set the number of CPU */
	s->rtc_cmos_write(s, RTC_REG_NR_PROCESSORS, 1);
}

void arch_guest_set_cmos (struct vmm_guest *guest, struct cmos_rtc_state *s)
{
	struct x86_guest_priv *priv = x86_guest_priv(guest);

	if (priv)
		priv->rtc_cmos = s;

	guest_cmos_init(guest);
}

inline void *arch_get_guest_pic_list(struct vmm_guest *guest)
{
	return ((void *)x86_guest_priv(guest)->pic_list);
}

inline void arch_set_guest_pic_list(struct vmm_guest *guest, void *plist)
{
	x86_guest_priv(guest)->pic_list = plist;
}

void arch_set_guest_master_pic(struct vmm_guest *guest, struct i8259_state *pic)
{
	x86_guest_priv(guest)->master_pic = pic;
}

/*---------------------------------*
 * Guest's vCPU's helper funstions *
 *---------------------------------*/
/*!
 * \fn physical_addr_t gva_to_gp(struct vcpu_hw_context *context, virtual_addr_t vaddr)
 * \brief Convert a guest virtual address to guest physical
 *
 * This function converts a guest virtual address to a guest
 * physical address. Until the guest enables paging, the conversion
 * is identical otherwise its page table will be walked.
 *
 *\param context The guest VCPU context which has falted.
 *\param vaddr The guest virtual address which needs translation.
 *
 * \return If succeeds, it returns a guest physical address, NULL otherwise.
 */
int gva_to_gpa(struct vcpu_hw_context *context, virtual_addr_t vaddr, physical_addr_t *gpa)
{
	physical_addr_t rva = 0;

	/* If guest hasn't enabled paging, va = pa */
	if (!(context->g_cr0 & X86_CR0_PG)) {
		rva = vaddr;
		/* If still in real mode, apply segmentation */
		if (!(context->g_cr0 & X86_CR0_PE))
			rva = (context->vmcb->cs.sel << 4) | vaddr;

		*gpa = rva;
		return VMM_OK;
	}

	return VMM_EFAIL;
}

/*!
 * \fn physical_addr_t gpa_to_hpa(struct vcpu_hw_context *context, virtual_addr_t vaddr)
 * \brief Convert a guest physical address to host physical address.
 *
 * This function converts a guest physical address to a host physical
 * address.
 *
 *\param context The guest VCPU context which has falted.
 *\param vaddr The guest physical address which needs translation.
 *
 * \return If succeeds, it returns a guest physical address, NULL otherwise.
 */
int gpa_to_hpa(struct vcpu_hw_context *context, physical_addr_t vaddr, physical_addr_t *hpa)
{
	u32 tcr3 = (u32)context->vmcb->cr3;

	/*
	 * FIXME: Check if guest has moved to long mode, in which case
	 * This page walk won't apply. This is only for 32-bit systems.
	 *
	 * FIXME: Here physical address extension and page size extention
	 * is not accounted.
	 */
        u32 pde, pte;
	physical_addr_t pte_addr, pde_addr;

        /* page directory entry */
        pde_addr = (tcr3 & ~0xfff) + (4 * ((vaddr >> 22) & 0x3ff));
	/* FIXME: Should we always do cacheable memory access here ?? */
	if (vmm_host_memory_read(pde_addr, &pde,
				 sizeof(pde), TRUE) < sizeof(pde))
		return VMM_EFAIL;

        if (!(pde & 0x1))
		return VMM_EFAIL;

	/* page directory entry */
	pte_addr = (((u32)(pde & ~0xfff)) + (4 * ((vaddr >> 12) & 0x3ff)));
	/* FIXME: Should we always do cacheable memory access here ?? */
	if (vmm_host_memory_read(pte_addr, &pte,
				 sizeof(pte), TRUE) < sizeof(pte))
		return VMM_EFAIL;

	if (!(pte & 0x1))
		return VMM_EFAIL;

	*hpa = ((pte & PAGE_MASK) + (vaddr & ~PAGE_MASK));

	return VMM_OK;
}

int realmode_map_memory(struct vcpu_hw_context *context, virtual_addr_t vaddr,
			physical_addr_t paddr, size_t size)
{
	union page32 pde, pte;
	union page32 *pde_addr, *temp;
	physical_addr_t tpaddr, pte_addr;
	virtual_addr_t tvaddr;
	u32 index, boffs;

	pde_addr = &context->shadow32_pgt[((vaddr >> 22) & 0x3ff)];
	pde = *pde_addr;

	if (!pde.present) {
		if (context->pgmap_free_cache) {
			index = context->pgmap_free_cache;
			context->pgmap_free_cache = 0;
		} else {
			boffs = bitmap_find_free_region(context->shadow32_pg_map,
							NR_32BIT_PGLIST_PAGES, 0);
			index = boffs;
			context->pgmap_free_cache = boffs+1;
		}

		pde_addr->present = 1;
		pde_addr->rw = 1;
		tvaddr = (virtual_addr_t)(((virtual_addr_t)context->shadow32_pg_list) + (index * PAGE_SIZE));
		if (vmm_host_va2pa(tvaddr, &tpaddr) != VMM_OK)
			vmm_panic("%s: Failed to map vaddr to paddr for pde.\n",
				  __func__);
		pde_addr->paddr = (tpaddr >> PAGE_SHIFT);
	}

	temp = (union page32 *)((u64)(pde_addr->paddr << PAGE_SHIFT));
	pte_addr = (physical_addr_t)(temp + ((vaddr >> 12) & 0x3ff));
	/*pte_addr = ((pde_addr->paddr << PAGE_SHIFT) + ((vaddr >> 10) & 0xffc));*/
	/* FIXME: Should this be cacheable memory access ? */
	if (vmm_host_memory_read(pte_addr, (void *)&pte,
				 sizeof(pte), TRUE) < sizeof(pte))
		return VMM_EFAIL;

	if (pte.present)
		return VMM_EFAIL;

	pte.present = 1;
	pte.rw = 1;
	pte.paddr = (paddr >> PAGE_SHIFT);

	/* FIXME: Should this be cacheable memory access ? */
	if (vmm_host_memory_write(pte_addr, (void *)&pte,
				  sizeof(pte), TRUE) < sizeof(pte))
		return VMM_EFAIL;

	return VMM_OK;
}

int realmode_unmap_memory(struct vcpu_hw_context *context, virtual_addr_t vaddr,
			  size_t size)
{
	return VMM_OK;
}

/**
 * \brief Take exception to handle VM EXIT
 *
 * Xvisor by design handles VM EXIT as part of exception.
 * It assumes that VM EXIT causes an exception. To fit
 * in that world, we use the software interrupt method
 * to induce a fake exception. Complete VM EXIT is
 * handled while in that exception handler.
 */
void arch_guest_handle_vm_exit(struct vcpu_hw_context *context)
{
	__asm__ __volatile__("movq %0,  %%rdi\n"
			     "movq %1,  %%rsi\n"
			     "int $0x80\n"
			     ::"ri"(GUEST_VM_EXIT_SW_CODE), "r"(context)
			     :"rdi","rsi");
}

/**
 * \brief Initiate a guest halt.
 *
 * This function is to be used by the vCPU which is
 * currently active and running. Since that vCPU
 * cannot destroy itself and associated guest,
 * it gets itself out of execution and tells
 * VMM by special opcode that it want to shutdown.
 *
 * @param guest
 * The guest that needs to be shutdown in emergency.
 */
void arch_guest_halt(struct vmm_guest *guest)
{
	__asm__ __volatile__("movq %0, %%rdi\n"
			     "movq %1, %%rsi\n"
			     "int $0x80\n"
			     ::"ri"(GUEST_HALT_SW_CODE), "r"(guest)
			     :"rdi","rsi");
}
