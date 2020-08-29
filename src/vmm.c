/*
 * Virtual machine monitor
 */

#include <stddef.h>
#include <stdint.h>
#include <khelper.h>
#include "vmcb.h"
#include "uart.h"
#include "x86.h"

/* The VMCB attrib bits are 55:52 and 47:40 from here */

static
uint64_t
gdt[] = {
	0,
	0,
	0x00209A0000000000,	/* __BOOT_CS */
	0x0000920000000000,	/* __BOOT_DS */
};

/*
 * VMCB
 */
static
struct vmcb vmcb;

/*
 * Host save state
 */
static
uint8_t host_save_state[4096] __attribute__((aligned(4096)));

#define MSR_EFER	0xC0000080
# define EFER_SVME	(1 << 12)

#define MSR_VM_HSAVE_PA	0xC0010117

void guest_entry();
void ge_kernel_addr();
void ge_boot_params();

void
vmm_startup(void *linux_entry, void *boot_params)
{
	u64 efer;

	uart_setup(); /* Make sure we can print stuff */

	/* Enable SVM */
	uart_print("Enabling SVM...\n", efer);
	efer = rdmsr(MSR_EFER);
	wrmsr(MSR_EFER, efer | EFER_SVME);

	/* Load host save state */
	uart_print("Host save state at: %p\n", host_save_state);
	wrmsr(MSR_VM_HSAVE_PA, (uint64_t) host_save_state);

	/* Setup VMCB */
	vmcb.guest_asid = 1;
	vmcb.vmrun = 1;

	vmcb.gdtr_limit = sizeof(gdt);
	vmcb.gdtr_base = (uint64_t) gdt;

	vmcb.es_selector = 0x18;
	vmcb.es_attrib = 0x0092;

	vmcb.cs_selector = 0x10;
	vmcb.cs_attrib = 0x029a;

	vmcb.ss_selector = 0x18;
	vmcb.ss_attrib = 0x0092;

	vmcb.ds_selector = 0x18;
	vmcb.ds_attrib = 0x0092;

	vmcb.fs_selector = 0x18;
	vmcb.fs_attrib = 0x0092;

	vmcb.gs_selector = 0x18;
	vmcb.gs_attrib = 0x0092;

	vmcb.cr0 = read_cr0();
	vmcb.cr3 = read_cr3();
	vmcb.cr4 = read_cr4();
	vmcb.efer = rdmsr(MSR_EFER) & ~EFER_SVME;

	*(uint64_t *) (ge_kernel_addr + 2) = (uint64_t) linux_entry;
	*(uint64_t *) (ge_boot_params + 2) = (uint64_t) boot_params;

	vmcb.rip = (uint64_t) guest_entry;
	uart_print("Guest entry point: %p\n", guest_entry);

	/* Start the guest */
	uart_print("VMCB at: %p\n", &vmcb);
	uart_print("Starting guest...\n");

	for (;;) {
		asm volatile (
			"movq %0, %%rax\n"
			"vmrun"
			:: "g" (&vmcb) : "rax");


		uart_print("#VMEXIT(%d)\n", vmcb.exitcode);
	}
}
