/**
 * Copyright (c) 2013 Himanshu Chauhan.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * @file amd_intercept.c
 * @author Himanshu Chauhan (hschauhan@nulltrace.org)
 * @brief SVM intercept handling/registration code.
 */
#include <vmm_error.h>
#include <vmm_types.h>
#include <vmm_stdio.h>
#include <vmm_host_aspace.h>
#include <vmm_guest_aspace.h>
#include <cpu_vm.h>
#include <cpu_inst_decode.h>
#include <cpu_features.h>
#include <cpu_mmu.h>
#include <cpu_pgtbl_helper.h>
#include <arch_guest_helper.h>
#include <vm/amd_intercept.h>
#include <vm/amd_svm.h>
#include <vmm_devemu.h>
#include <vmm_manager.h>
#include <vmm_main.h>

static char *exception_names[] = {
	"#DivError",	/* 0 */
	"#Debug",	/* 1 */
	"#NMI",		/* 2 */
	"#Breakpoint",	/* 3 */
	"#Overflow",	/* 4 */
	"#OutOfBounds",	/* 5 */
	"#InvOpcode",	/* 6 */
	"#NoDev",	/* 7 */
	"#DoubleFault",	/* 8 */
	"#CoprocOvrrun",/* 9 */
	"#InvalTSS",	/* 10 */
	"#MissingSeg",	/* 11 */
	"#MissingStack",/* 12 */
	"#GPF",		/* 13 */
	"#PGFault",	/* 14 */
	"#CoprocErr",	/* 15 */
	"#AlignCheck",	/* 16 */
	"#MachineCheck",/* 17 */
	"#SIMDErr",	/* 18 */
	"#Unknown19",	/* 19 */
	"#Unknown20",	/* 20 */
	"#Unknown21",	/* 21 */
	"#Unknown22",	/* 22 */
	"#Unknown23",	/* 23 */
	"#Unknown24",	/* 24 */
	"#Unknown25",	/* 25 */
	"#Unknown26",	/* 26 */
	"#Unknown27",	/* 27 */
	"#Unknown28",	/* 28 */
	"#Unknown29",	/* 29 */
	"#Unknown30",	/* 30 */
	"#Unknown31",	/* 31 */
};

extern int realmode_map_memory(struct vcpu_hw_context *context, virtual_addr_t vaddr,
			       physical_addr_t paddr, size_t size);

static inline void dump_guest_exception_insts(struct vmcb *vmcb)
{
	int i;
	u8 *guest_ins_base = (u8 *)((u8 *)(vmcb)) + 0xd0;

	for (i = 0; i < 16; i++) {
		vmm_printf("%x ", guest_ins_base[i]);
		if (i && !(i % 8)) vmm_printf("\n");
	}
}

static inline int guest_in_realmode(struct vcpu_hw_context *context)
{
	return (!(context->vmcb->cr0 & X86_CR0_PE));
}

static int guest_read_fault_inst(struct vcpu_hw_context *context, x86_inst *g_ins)
{
	physical_addr_t rip_phys;

	if (gva_to_gpa(context, context->vmcb->rip, &rip_phys)) {
		VM_LOG(LVL_ERR, "Failed to convert guest virtual 0x%x to guest physical.\n",
			context->vmcb->rip);
		return VMM_EFAIL;
	}

	/* FIXME: Should we always do cacheable memory access here ?? */
	if (vmm_guest_memory_read(context->assoc_vcpu->guest, rip_phys,
				  g_ins, sizeof(x86_inst), TRUE) < sizeof(x86_inst)) {
		VM_LOG(LVL_ERR, "Failed to read instruction at intercepted "
		       "instruction pointer. (%x)\n", rip_phys);
		return VMM_EFAIL;
	}

	return VMM_OK;
}

void __handle_vm_npf (struct vcpu_hw_context *context)
{
	VM_LOG(LVL_INFO, "Unhandled Intercept: nested page fault.\n");
	if (context->vcpu_emergency_shutdown)
		context->vcpu_emergency_shutdown(context);
}

void __handle_vm_swint (struct vcpu_hw_context *context)
{
	VM_LOG(LVL_INFO, "Unhandled Intercept: software interrupt.\n");
	if (context->vcpu_emergency_shutdown)
		context->vcpu_emergency_shutdown(context);
}

void __handle_vm_exception (struct vcpu_hw_context *context)
{
	switch (context->vmcb->exitcode)
	{
	case VMEXIT_EXCEPTION_PF:
		VM_LOG(LVL_DEBUG, "Guest fault: 0x%x (rIP: %x)\n",
		       context->vmcb->exitinfo2, context->vmcb->rip);

		u64 fault_gphys = context->vmcb->exitinfo2;
		/* Guest is in real mode so faulting guest virtual is
		 * guest physical address. We just need to add faulting
		 * address as offset to host physical address to get
		 * the destination physical address.
		 */
		struct vmm_region *g_reg = vmm_guest_find_region(context->assoc_vcpu->guest,
								 fault_gphys,
								 VMM_REGION_MEMORY,
								 FALSE);
		if (!g_reg) {
			VM_LOG(LVL_ERR, "ERROR: Can't find the host physical address for guest physical: 0x%lx\n",
			       fault_gphys);
			goto guest_bad_fault;
		}

		/* If fault is on a RAM backed address, map and return. Otherwise do emulate. */
		if (g_reg->flags & VMM_REGION_REAL) {
			if (realmode_map_memory(context, fault_gphys,
						(g_reg->hphys_addr + fault_gphys),
						PAGE_SIZE) != VMM_OK) {
				VM_LOG(LVL_ERR, "ERROR: Failed to create map in guest's shadow page table.\n");
				goto guest_bad_fault;
			}
			context->vmcb->cr2 = context->vmcb->exitinfo2;
		} else {
			x86_inst ins;
			x86_decoded_inst_t dinst;
			u64 guest_rd;

			if (guest_read_fault_inst(context, &ins)) {
				VM_LOG(LVL_ERR, "Failed to read faulting guest instruction.\n");
				goto guest_bad_fault;
			}

			if (x86_decode_inst(ins, &dinst) != VMM_OK) {
				VM_LOG(LVL_ERR, "Failed to decode guest instruction.\n");
				goto guest_bad_fault;
			}

			if (unlikely(dinst.inst_type != INST_TYPE_MOV)) {
				VM_LOG(LVL_ERR, "IO Fault in guest without a move instruction!\n");
				goto guest_bad_fault;
			}

			if ((dinst.inst.gen_mov.src_addr >= g_reg->gphys_addr) &&
			    (dinst.inst.gen_mov.src_addr < (g_reg->gphys_addr + g_reg->phys_size))) {
				if (vmm_devemu_emulate_read(context->assoc_vcpu, fault_gphys,
							    &guest_rd, dinst.inst.gen_mov.op_size,
							    VMM_DEVEMU_NATIVE_ENDIAN) != VMM_OK) {
					vmm_printf("Failed to emulate IO instruction in guest.\n");
					goto guest_bad_fault;
				}

				if (dinst.inst.gen_mov.dst_addr >= RM_REG_AX &&
				    dinst.inst.gen_mov.dst_addr < RM_REG_MAX) {
					context->g_regs[dinst.inst.gen_mov.dst_addr] = guest_rd;
					if (dinst.inst.gen_mov.dst_addr == RM_REG_AX)
						context->vmcb->rax = guest_rd;
				} else {
					VM_LOG(LVL_ERR, "Memory to memory move instruction not supported.\n");
					goto guest_bad_fault;
				}
			}

			if ((dinst.inst.gen_mov.dst_addr >= g_reg->gphys_addr) &&
			    (dinst.inst.gen_mov.dst_addr < (g_reg->gphys_addr + g_reg->phys_size))) {
				if (dinst.inst.gen_mov.src_type == OP_TYPE_IMM) {
					guest_rd = dinst.inst.gen_mov.src_addr;
				} else if (dinst.inst.gen_mov.src_addr >= RM_REG_AX &&
					   dinst.inst.gen_mov.src_addr < RM_REG_MAX) {
					if (dinst.inst.gen_mov.dst_addr == RM_REG_AX)
						guest_rd = context->vmcb->rax;
					else
						guest_rd = context->g_regs[dinst.inst.gen_mov.src_addr];
				} else {
					VM_LOG(LVL_ERR, "Memory to memory move instruction not supported.\n");
					goto guest_bad_fault;
				}

				if (vmm_devemu_emulate_write(context->assoc_vcpu, fault_gphys,
							     &guest_rd, dinst.inst.gen_mov.op_size,
							     VMM_DEVEMU_NATIVE_ENDIAN) != VMM_OK) {
					vmm_printf("Failed to emulate IO instruction in guest.\n");
					goto guest_bad_fault;
				}
			}
			context->vmcb->rip += dinst.inst_size;
		}
		break;

	default:
		VM_LOG(LVL_ERR, "Unhandled guest exception %s (rIP: %x)\n",
		       exception_names[context->vmcb->exitcode - 0x40],
		       context->vmcb->rip);
		goto guest_bad_fault;
		break;
	}

	return;

 guest_bad_fault:
	if (context->vcpu_emergency_shutdown)
		context->vcpu_emergency_shutdown(context);
}

void __handle_vm_wrmsr (struct vcpu_hw_context *context)
{
	VM_LOG(LVL_INFO, "Unhandled Intercept: msr write.\n");
	if (context->vcpu_emergency_shutdown)
		context->vcpu_emergency_shutdown(context);
}

void __handle_popf(struct vcpu_hw_context *context)
{
	VM_LOG(LVL_INFO, "Unhandled Intercept: popf.\n");
	if (context->vcpu_emergency_shutdown)
		context->vcpu_emergency_shutdown(context);
}

void __handle_vm_vmmcall (struct vcpu_hw_context *context)
{
	VM_LOG(LVL_INFO, "Unhandled Intercept: vmmcall.\n");
	if (context->vcpu_emergency_shutdown)
		context->vcpu_emergency_shutdown(context);
}

void __handle_vm_iret(struct vcpu_hw_context *context)
{
	VM_LOG(LVL_INFO, "Unhandled Intercept: iret.\n");
	if (context->vcpu_emergency_shutdown)
		context->vcpu_emergency_shutdown(context);
}

void __handle_crN_read(struct vcpu_hw_context *context)
{
	int crn = context->vmcb->exitcode - VMEXIT_CR0_READ;
	int cr_gpr;

	switch(crn) {
	case 0:
		/* Check if host support instruction decode assistance */
		if (context->cpuinfo->decode_assist) {
			if (context->vmcb->exitinfo1 & VALID_CRN_TRAP) {
				cr_gpr = (context->vmcb->exitinfo1 & 0xf);
				VM_LOG(LVL_DEBUG, "Guest writing 0x%lx to Cr0 from reg %d.\n",
				       context->g_regs[cr_gpr], cr_gpr);
			}
		} else {
			x86_inst ins64;
			x86_decoded_inst_t dinst;
			u64 rvalue;

			if (guest_read_fault_inst(context, &ins64)) {
				VM_LOG(LVL_ERR, "Failed to read faulting guest instruction.\n");
				goto guest_bad_fault;
			}

			if (x86_decode_inst(ins64, &dinst) != VMM_OK) {
				VM_LOG(LVL_ERR, "Failed to decode instruction.\n");
				goto guest_bad_fault;
			}

			if (likely(dinst.inst_type == INST_TYPE_MOV_CR)) {
				switch (dinst.inst.crn_mov.src_reg) {
				case RM_REG_CR0:
					rvalue = context->g_cr0;
					break;

				case RM_REG_CR1:
					rvalue = context->g_cr1;
					break;

				case RM_REG_CR2:
					rvalue = context->g_cr2;
					break;

				case RM_REG_CR3:
					rvalue = context->g_cr3;
					break;

				default:
					VM_LOG(LVL_ERR, "Unknown CR reg %d\n", dinst.inst.crn_mov.src_reg);
					goto guest_bad_fault;
				}

				if (!dinst.inst.crn_mov.dst_reg)
					context->vmcb->rax = rvalue;

				context->g_regs[dinst.inst.crn_mov.dst_reg] = context->g_cr0;
				context->vmcb->rip += dinst.inst_size;
				VM_LOG(LVL_DEBUG, "GR: CR0= 0x%8lx HCR0= 0x%8lx\n", context->g_cr0, context->vmcb->cr0);
			} else {
				VM_LOG(LVL_ERR, "Unknown fault instruction: 0x%lx\n", ins64);
				goto guest_bad_fault;
			}
		}
		break;
	case 3:
		break;
	default:
		VM_LOG(LVL_ERR, "Unhandled intercept cr%d read\n",
		       crn);
		break;
	}

	return;

 guest_bad_fault:
	if (context->vcpu_emergency_shutdown){
		context->vcpu_emergency_shutdown(context);
	}
}

void __handle_crN_write(struct vcpu_hw_context *context)
{
	int crn = context->vmcb->exitcode - VMEXIT_CR0_WRITE;
	int cr_gpr;
	u32 bits_clrd;
	u32 bits_set;
	u64 htr;

	switch(crn) {
	case 0:
		/* Check if host support instruction decode assistance */
		if (context->cpuinfo->decode_assist) {
			if (context->vmcb->exitinfo1 & VALID_CRN_TRAP) {
				cr_gpr = (context->vmcb->exitinfo1 & 0xf);
				VM_LOG(LVL_DEBUG, "Guest writing 0x%lx to Cr0 from reg %d.\n",
				       context->g_regs[cr_gpr], cr_gpr);
			}
		} else {
			x86_inst ins64;
			x86_decoded_inst_t dinst;

			if (guest_read_fault_inst(context, &ins64)) {
				VM_LOG(LVL_ERR, "Failed to read guest instruction.\n");
				goto guest_bad_fault;
			}

			if (x86_decode_inst(ins64, &dinst) != VMM_OK) {
				VM_LOG(LVL_ERR, "Failed to code instruction.\n");
				goto guest_bad_fault;
			}

			if (dinst.inst_type == INST_TYPE_MOV_CR) {
				switch(dinst.inst.crn_mov.dst_reg) {
				case RM_REG_CR0:
					if (!dinst.inst.crn_mov.src_reg) {
						bits_set = (~context->g_cr0 & context->vmcb->rax);
						bits_clrd = (context->g_cr0 & ~context->vmcb->rax);
						context->g_cr0 = context->vmcb->rax;
					} else {
						bits_set = (~context->g_cr0 & context->g_regs[dinst.inst.crn_mov.src_reg]);
						bits_clrd = (context->g_cr0 & ~context->g_regs[dinst.inst.crn_mov.src_reg]);
						context->g_cr0 = context->g_regs[dinst.inst.crn_mov.src_reg];
					}

					if (bits_set & X86_CR0_PE)
						context->vmcb->cr0 |= X86_CR0_PE;

					if (bits_set & X86_CR0_PG)
						context->vmcb->cr0 |= X86_CR0_PG;

					if (bits_clrd & X86_CR0_CD)
						context->vmcb->cr0 &= ~X86_CR0_CD;

					if (bits_clrd & X86_CR0_NW)
						context->vmcb->cr0 &= ~X86_CR0_NW;

					break;
				default:
					VM_LOG(LVL_ERR, "Write to CR%d not supported.\n",
					       dinst.inst.crn_mov.dst_reg - RM_REG_CR0);
					goto guest_bad_fault;
				}

				context->vmcb->rip += dinst.inst_size;

				asm volatile("str %0\n"
					     :"=r"(htr));
				VM_LOG(LVL_DEBUG, "GW: CR0= 0x%8lx HCR0: 0x%8lx TR: 0x%8x HTR: 0x%x\n",
				       context->g_cr0, context->vmcb->cr0, context->vmcb->tr, htr);
			} else {
				VM_LOG(LVL_ERR, "Unknown fault instruction\n");
				goto guest_bad_fault;
			}
		}
		break;
	case 3:
		break;
	default:
		VM_LOG(LVL_ERR, "Unhandled intercept cr%d write\n",
		       crn);
		break;
	}

	return;

 guest_bad_fault:
	if (context->vcpu_emergency_shutdown){
		context->vcpu_emergency_shutdown(context);
	}
}

void __handle_ioio(struct vcpu_hw_context *context)
{
	u32 io_port = (context->vmcb->exitinfo1 >> 16);
	u8 in_inst = (context->vmcb->exitinfo1 & (0x1 << 0));
	u8 str_op = (context->vmcb->exitinfo1 & (0x1 << 2));
	u8 rep_access = (context->vmcb->exitinfo1 & (0x1 << 3));
	u8 op_size = (context->vmcb->exitinfo1 & (0x1 << 4) ? 8
		      : ((context->vmcb->exitinfo1 & (0x1 << 5)) ? 16
			 : 32));
	u8 seg_num = (context->vmcb->exitinfo1 >> 10) & 0x7;
	u32 guest_rd = 0;
	u32 wval;

	VM_LOG(LVL_DEBUG, "RIP: %x exitinfo1: %x\n", context->vmcb->rip, context->vmcb->exitinfo1);
	VM_LOG(LVL_DEBUG, "IOPort: 0x%x is accssed for %sput. Size is %d. Segment: %d String operation? %s Repeated access? %s\n",
	       io_port, (in_inst ? "in" : "out"), op_size,seg_num,(str_op ? "yes" : "no"),(rep_access ? "yes" : "no"));

	if (in_inst) {
		if (vmm_devemu_emulate_ioread(context->assoc_vcpu, io_port,
					      &guest_rd, op_size/8,
					      VMM_DEVEMU_NATIVE_ENDIAN) != VMM_OK) {
			vmm_printf("Failed to emulate IO instruction in guest.\n");
			goto _fail;
		}

		context->g_regs[GUEST_REGS_RAX] = guest_rd;
		context->vmcb->rax = guest_rd;
	} else {
		wval = (u32)context->vmcb->rax;
		if (vmm_devemu_emulate_iowrite(context->assoc_vcpu, io_port,
					       &wval, op_size/8,
					       VMM_DEVEMU_NATIVE_ENDIAN) != VMM_OK) {
			vmm_printf("Failed to emulate IO instruction in guest.\n");
			goto _fail;
		}
	}

	context->vmcb->rip = context->vmcb->exitinfo2;

	return;

 _fail:
	if (context->vcpu_emergency_shutdown){
		context->vcpu_emergency_shutdown(context);
	}
}

void __handle_cpuid(struct vcpu_hw_context *context)
{
	struct x86_vcpu_priv *priv = x86_vcpu_priv(context->assoc_vcpu);
	struct cpuid_response *func;

	switch (context->vmcb->rax) {
	case CPUID_BASE_VENDORSTRING:
		func = &priv->standard_funcs[CPUID_BASE_VENDORSTRING];
		context->vmcb->rax = func->resp_eax;
		context->g_regs[GUEST_REGS_RBX] = func->resp_ebx;
		context->g_regs[GUEST_REGS_RCX] = func->resp_ecx;
		context->g_regs[GUEST_REGS_RDX] = func->resp_edx;
		break;

	case CPUID_BASE_FEATURES:
		func = &priv->standard_funcs[CPUID_BASE_FEATURES];
		context->vmcb->rax = func->resp_eax;
		context->g_regs[GUEST_REGS_RBX] = func->resp_ebx;
		context->g_regs[GUEST_REGS_RCX] = func->resp_ecx;
		context->g_regs[GUEST_REGS_RDX] = func->resp_edx;
		break;

	case CPUID_EXTENDED_BASE:
	case CPUID_EXTENDED_BRANDSTRING:
	case CPUID_EXTENDED_BRANDSTRINGMORE:
		func = &priv->extended_funcs[context->vmcb->rax - CPUID_EXTENDED_BASE];
		context->vmcb->rax = func->resp_eax;
		context->g_regs[GUEST_REGS_RBX] = func->resp_ebx;
		context->g_regs[GUEST_REGS_RCX] = func->resp_ecx;
		context->g_regs[GUEST_REGS_RDX] = func->resp_edx;
		break;

	default:
		VM_LOG(LVL_DEBUG, "GCPUID/R: Func: %x\n", context->vmcb->rax);
		goto _fail;
	}

	context->vmcb->rip += 2;

	return;

 _fail:
	if (context->vcpu_emergency_shutdown){
		context->vcpu_emergency_shutdown(context);
	}
}

/**
 * \brief Handle the shutdown condition in guest.
 *
 * If the guest has seen a shutdown condition (a.k.a. triple fault)
 * give the notification to guest and the guest must be
 * destroyed then. If the guest as multiple vCPUs, all of then
 * should be sent a notification of this.
 *
 * @param context
 * The hardware context of the vcpu of the guest which saw the triple fault.
 */
void __handle_triple_fault(struct vcpu_hw_context *context)
{
	VM_LOG(LVL_ERR, "Triple fault in guest: %s!!\n", context->assoc_vcpu->guest->name);

	if (context->vcpu_emergency_shutdown)
		context->vcpu_emergency_shutdown(context);

	vmm_hang();
}

void handle_vcpuexit(struct vcpu_hw_context *context)
{
	VM_LOG(LVL_DEBUG, "**** #VMEXIT - exit code: %x\n", (u32) context->vmcb->exitcode);

	switch (context->vmcb->exitcode) {
	case VMEXIT_CR0_READ ... VMEXIT_CR15_READ: __handle_crN_read(context); break;
	case VMEXIT_CR0_WRITE ... VMEXIT_CR15_WRITE: __handle_crN_write(context); break;
	case VMEXIT_MSR:
		if (context->vmcb->exitinfo1 == 1) __handle_vm_wrmsr (context);
		break;
	case VMEXIT_EXCEPTION_DE ... VMEXIT_EXCEPTION_XF:
		__handle_vm_exception(context); break;

	case VMEXIT_SWINT: __handle_vm_swint(context); break;
	case VMEXIT_NPF: __handle_vm_npf (context); break;
	case VMEXIT_VMMCALL: __handle_vm_vmmcall(context); break;
	case VMEXIT_IRET: __handle_vm_iret(context); break;
	case VMEXIT_POPF: __handle_popf(context); break;
	case VMEXIT_SHUTDOWN: __handle_triple_fault(context); break;
	case VMEXIT_CPUID: __handle_cpuid(context); break;
	case VMEXIT_IOIO: __handle_ioio(context); break;
	case VMEXIT_INTR: break; /* silently */
	default:
		VM_LOG(LVL_ERR, "#VMEXIT: Unhandled exit code: %x\n",
		       (u32)context->vmcb->exitcode);
		if (context->vcpu_emergency_shutdown)
			context->vcpu_emergency_shutdown(context);
	}
}
