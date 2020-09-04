/*
 * Virtual machine monitor
 */

#include <stddef.h>
#include <stdint.h>
#include <khelper.h>
#include <include/x86.h>
#include <kernel/acpi.h>
#include <kernel/alloc.h>
#include <kernel/kernel.h>
#include <kernel/uart.h>
#include "vmcb.h"

#define MSR_VM_CR	0xC0010114
# define VM_CR_R_INIT	(1 << 1)

#define MSR_VM_HSAVE_PA	0xC0010117

#define MSR_EFER	0xC0000080
# define EFER_SVME	(1 << 12)

struct vmcb *
vmm_setup_core(void)
{
	/* Enable SVM */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SVME);
	/* Allocate and configure host save state */
	wrmsr(MSR_VM_HSAVE_PA, (uint64_t) alloc_pages(1, 0));

	/* Allocate VMCB */
	return alloc_pages(1, 0);
}

static uint64_t *nested_pml4 = NULL;
static uint64_t *lapic_entry = NULL;

#if 0
static
void *
make_ident(void)
{
	uint64_t cur_phys, *pdp, *pd;
	size_t pdp_idx, pd_idx;

	if (!nested_pml4) {
		nested_pml4 = alloc_pages(1, 0);
		cur_phys = 0;

		pdp = alloc_pages(1, 0);
		for (pdp_idx = 0; pdp_idx < 512; ++pdp_idx) {
			pdp[pdp_idx] = (uint64_t) cur_phys | 0x87;
			cur_phys += 0x40000000;
		}
		nested_pml4[0] = (uint64_t) pdp | 7;
	}
	return nested_pml4;
}
/* TODO: investigate why KVM is a broken with 1GiB pages */
#endif

static
void *
make_ident(void)
{
	uint64_t cur_phys, *pdp, *pd, *pt;
	size_t pdp_idx, pd_idx;

	if (!nested_pml4) {
		nested_pml4 = alloc_pages(1, 0);
		cur_phys = 0;

		pdp = alloc_pages(1, 0);
		for (pdp_idx = 0; pdp_idx < 512; ++pdp_idx) {
			pd = alloc_pages(1, 0);
			for (pd_idx = 0; pd_idx < 512; ++pd_idx) {
				if (cur_phys == (uint64_t) lapic_addr) {
					/* FIXME: this leaves a
						memory hole after the LAPIC */
					pt = alloc_pages(1, 0);
					pt[0] = (uint64_t) cur_phys | 5;
					lapic_entry = &pt[0];
					pd[pd_idx] = (uint64_t) pt | 7;
				} else {
					pd[pd_idx] = cur_phys | 0x87;
				}
				cur_phys += 0x200000;
			}
			pdp[pdp_idx] = (uint64_t) pd | 7;
		}
		nested_pml4[0] = (uint64_t) pdp | 7;
	}
	return nested_pml4;
}


void
vmm_setup(struct vmcb *vmcb)
{
	/* Setup VMCB */
	vmcb->guest_asid = 1;
	vmcb->vmrun = 1;
	vmcb->vmmcall = 1;

	/* Enable nested paging */
	if (!acpi_get_apic_id()) {
		vmcb->np_en = 1;
		vmcb->n_cr3 = (uint64_t) make_ident();
	}

	/* CPUID emulation */
	vmcb->cpuid = 1;

	/* FIXME: the current segmentation setup has no GDT backing it,
		only hidden segmnet registers are set up, but Linux loads its
		own so we don't care for now */
	vmcb->es_selector = 0x18;
	vmcb->es_attrib = 0x0092;
	vmcb->cs_selector = 0x10;
	vmcb->cs_attrib = 0x029a;
	vmcb->ss_selector = 0x18;
	vmcb->ss_attrib = 0x0092;
	vmcb->ds_selector = 0x18;
	vmcb->ds_attrib = 0x0092;
	vmcb->fs_selector = 0x18;
	vmcb->fs_attrib = 0x0092;
	vmcb->gs_selector = 0x18;
	vmcb->gs_attrib = 0x0092;

	vmcb->cr0 = read_cr0();
	vmcb->cr3 = read_cr3();
	vmcb->cr4 = read_cr4();
	vmcb->efer = rdmsr(MSR_EFER);
}

static
uint64_t
read_guest_gpr(struct vmcb *vmcb, struct gprs *gprs, int idx)
{
	switch (idx) {
	case 0:
		return vmcb->rax;
	case 1:
		return gprs->rcx;
	case 2:
		return gprs->rdx;
	case 3:
		return gprs->rbx;
	case 4:
		return vmcb->rsp;
	case 5:
		return gprs->rbp;
	case 6:
		return gprs->rsi;
	case 7:
		return gprs->rdi;
	case 8:
		return gprs->r8;
	case 9:
		return gprs->r9;
	case 0xa:
		return gprs->r10;
	case 0xb:
		return gprs->r11;
	case 0xc:
		return gprs->r12;
	case 0xd:
		return gprs->r13;
	case 0xe:
		return gprs->r14;
	case 0xf:
		return gprs->r15;
	default: /* Bad idx */
		return 0;
	}
}

static
void
write_guest_gpr(struct vmcb *vmcb, struct gprs *gprs, int idx, uint64_t val)
{
	switch (idx) {
	case 0:
		vmcb->rax = val;
		break;
	case 1:
		gprs->rcx = val;
		break;
	case 2:
		gprs->rdx = val;
		break;
	case 3:
		gprs->rbx = val;
		break;
	case 4:
		vmcb->rsp = val;
		break;
	case 5:
		gprs->rbp = val;
		break;
	case 6:
		gprs->rsi = val;
		break;
	case 7:
		gprs->rdi = val;
		break;
	case 8:
		gprs->r8 = val;
		break;
	case 9:
		gprs->r9 = val;
		break;
	case 0xa:
		gprs->r10 = val;
		break;
	case 0xb:
		gprs->r11 = val;
		break;
	case 0xc:
		gprs->r12 = val;
		break;
	case 0xd:
		gprs->r13 = val;
		break;
	case 0xe:
		gprs->r14 = val;
		break;
	case 0xf:
		gprs->r15 = val;
		break;
	}
}

#define PT_ADDR(x)	(x & 0xffffffffff000)
#define PT_ADDR_HUGE(x)	(x & 0xfffffffffe000)

static
void *
guest_pgwalk(uint64_t *Pml4, uint64_t VirtAddr)
{
	uint64_t Pml4Idx, PdpIdx, PdIdx, PtIdx;
	uint64_t *TmpPtr;

	Pml4Idx = VirtAddr >> 39 & 0x1ff;
	PdpIdx  = VirtAddr >> 30 & 0x1ff;
	PdIdx   = VirtAddr >> 21 & 0x1ff;
	PtIdx   = VirtAddr >> 12 & 0x1ff;

	/* Page map level 4 */
	if (!(Pml4[Pml4Idx] & 1))
		return NULL;
	TmpPtr = (void *) PT_ADDR(Pml4[Pml4Idx]);

	/* Page directory pointer */
	if (!(TmpPtr[PdpIdx] & 1))
		return NULL;
	else if (TmpPtr[PdpIdx] & 0x80) /* 1 GiB page */
		return (void *) PT_ADDR_HUGE(TmpPtr[PdpIdx])
			+ (VirtAddr & 0x3fffffffULL);

	TmpPtr = (void *) PT_ADDR(TmpPtr[PdpIdx]); /* Page directory */

	/* Page directory */
	if (!(TmpPtr[PdIdx] & 1))
		return NULL;
	else if (TmpPtr[PdIdx] & 0x80) /* 2 MiB page */
		return (void *) PT_ADDR_HUGE(TmpPtr[PdIdx])
			+ (VirtAddr & 0x1fffffULL);

	TmpPtr = (void *) PT_ADDR(TmpPtr[PdIdx]);  /* Page table */

	/* Page table */
	if (!(TmpPtr[PtIdx] & 1))
		return NULL;
	else /* 4 KiB page */
		return (void *) PT_ADDR(TmpPtr[PtIdx]) + (VirtAddr & 0xfffULL);
}

uint8_t sipi_core = 0;
uint8_t sipi_vector = 0;

#define VMEXIT_EXP_SX	0x5e
#define VMEXIT_CPUID	0x72
#define VMEXIT_VMRUN	0x80
#define VMEXIT_VMMCALL	0x81
#define VMEXIT_NPF	0x400

void
vmexit_handler(struct vmcb *vmcb, struct gprs *gprs)
{
	uint64_t rax, rcx, rdx, rbx;
	uint8_t *guest_rip;
	uint32_t val;

	switch (vmcb->exitcode) {
	case VMEXIT_CPUID:
		/* Print CPUID leaf and subleaf */
		uart_print("CPUID RAX=%lx, RCX=%lx\n", vmcb->rax, gprs->rcx);

		rax = vmcb->rax;
		rcx = gprs->rcx;
		asm volatile (
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"cpuid\n"
			"movq %%rax, %0\n"
			"movq %%rcx, %1\n"
			"movq %%rdx, %2\n"
			"movq %%rbx, %3\n"
			: "=m" (rax), "=m" (rcx), "=m" (rdx), "=m" (rbx)
			:: "rax", "rcx", "rdx", "rbx");

		switch (vmcb->rax) {
		case 0:	/* Change the CPUID string to BootlegAMD */
			vmcb->rax = rax;
			gprs->rbx = 0x746f6f42;
			gprs->rcx = 0x0000444d;
			gprs->rdx = 0x4167656c;
			break;
		case 1:	/* Hide x2APIC support from the OS */
			vmcb->rax = rax;
			gprs->rbx = rbx;
			gprs->rcx = rcx & ~(1 << 21);
			gprs->rdx = rdx;
			break;
		default:
			vmcb->rax = rax;
			gprs->rbx = rbx;
			gprs->rcx = rcx;
			gprs->rdx = rdx;
			break;
		}
		vmcb->rip += 2;
		break;
	case VMEXIT_VMMCALL: /* AP guest code fires this
				when the BSP sends a SIPI */
		uart_print("VMMCALL\n");

		/* Switch to real mode */
		vmcb->cr0 = 0;
		vmcb->cr3 = 0;
		vmcb->cr4 = 0;
		vmcb->efer = EFER_SVME;

		/* Start at the kernel's trampoline */
		vmcb->cs_selector = (uint16_t) sipi_vector << 8;
		vmcb->cs_base = sipi_vector << 12;
		vmcb->cs_limit = 0xffff;
		vmcb->cs_attrib = 0x9b;
		vmcb->rip = 0;

		vmcb->ds_selector = 0;
		vmcb->ds_base = 0;
		vmcb->ds_limit = 0xffff;
		vmcb->ds_attrib = 0x93;
		vmcb->es_selector = 0;
		vmcb->es_base = 0;
		vmcb->es_limit = 0xffff;
		vmcb->es_attrib = 0x93;
		vmcb->ss_selector = 0;
		vmcb->ss_base = 0;
		vmcb->ss_limit = 0xffff;
		vmcb->ss_attrib = 0x93;
		vmcb->fs_selector = 0;
		vmcb->fs_base = 0;
		vmcb->fs_limit = 0xffff;
		vmcb->fs_attrib = 0x93;
		vmcb->gs_selector = 0;
		vmcb->gs_base = 0;
		vmcb->gs_limit = 0xffff;
		vmcb->gs_attrib = 0x93;

		break;
	case VMEXIT_NPF:
		uart_print("Nested page fault at: %p!\n", vmcb->exitinfo2);
		guest_rip = guest_pgwalk((void *) vmcb->cr3, vmcb->rip);

		uart_print("%x %x %x %x %x %x %x\n", guest_rip[0],
			guest_rip[1], guest_rip[2], guest_rip[3],
			guest_rip[4], guest_rip[5], guest_rip[6]);

		if (guest_rip[1] == 0xb7) {
			val = gprs->rsi;
		} else if (guest_rip[1] == 0x3c) {
			val = gprs->rdi;
		} else { /* 0x14 */
			val = gprs->rdx;
		}
		vmcb->rip += 7;

		/* Do the write if it's not a IPI */
		if (vmcb->exitinfo2 == 0xfee00300) {
			if ((val >> 8 & 0xf) == 5) {
				uart_print("INIT apic: %p\n",
					(*(uint32_t *) 0xfee00310) >> 24);
			} else if ((val >> 8 & 0xf) == 6) {
				sipi_vector = val & 0xff;
				sipi_core = (*(uint32_t *) 0xfee00310) >> 24;
				uart_print("SIPI apic: %p, vector: %p\n", sipi_core, sipi_vector);
			} else {
				*(uint32_t *) vmcb->exitinfo2 = val;
			}
		} else {
			*(uint32_t *) vmcb->exitinfo2 = val;
		}

		break;
	default:
		uart_print("Unknown #VMEXIT %p %p %p\n",
			vmcb->exitcode,	vmcb->exitinfo1, vmcb->exitinfo2);
		for (;;)
			;
		break;
	}
}
