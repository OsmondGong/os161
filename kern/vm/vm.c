#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <proc.h>
#include <elf.h>
#include <spl.h>

/* Page functions */
int pt_insert_top(struct addrspace *as, uint32_t top_table_index);
int pt_insert_second(struct addrspace *as, uint32_t top_table_index, uint32_t second_table_index);

/* Place your page table functions here */
int pt_insert_top(struct addrspace *as, uint32_t top_table_index) {
    as->pt[top_table_index] = kmalloc(SECOND_LEVEL_SIZE * sizeof(paddr_t *));
    if (as->pt[top_table_index] == NULL) {
        return ENOMEM;
    }
    for (int i = 0; i < SECOND_LEVEL_SIZE; i++) {
        as->pt[top_table_index][i] = NULL;
    }
    return 0;
}

int pt_insert_second(struct addrspace *as, uint32_t top_table_index, uint32_t second_table_index) {
    as->pt[top_table_index][second_table_index] = kmalloc(THIRD_LEVEL_SIZE * sizeof(paddr_t));
    if (as->pt[top_table_index][second_table_index] == NULL) {
        return ENOMEM;
    }
    for (int i = 0; i < THIRD_LEVEL_SIZE; i++) {
        as->pt[top_table_index][second_table_index][i] = 0;
    }
    return 0;
}

void vm_bootstrap(void)
{
    /* Initialise any global components of your VM sub-system here.  
     *  
     * You may or may not need to add anything here depending what's
     * provided or required by the assignment spec.
     */
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    // Writing to READ ONLY page, fault
    if (faulttype == VM_FAULT_READONLY) {
        return EFAULT;
    }
    if (faultaddress == 0x0) {
        return EINVAL;
    }

    // don't know if this is what they want as index.
    // gets bits from page number
    uint32_t page_number = faultaddress & PAGE_FRAME;
    uint32_t top_table_index = page_number >> 24;
    uint32_t second_table_index = page_number << 8 >> 26;
    uint32_t third_table_index = page_number << 14 >> 26;

    struct addrspace *as = proc_getas();

    // if not in page table, add to page table
    if (as->pt[top_table_index] == NULL) {
        int err = pt_insert_top(as, top_table_index);
        if (err) {
            return err;
        }
    }
    if (!as->pt[top_table_index][second_table_index]) {
        int err = pt_insert_second(as, top_table_index, second_table_index);
        if (err) {
            return err;
        }
    }
    if (!as->pt[top_table_index][second_table_index][third_table_index]) {
        //check if in region
        struct region *cur_region = as->regions;
        while (cur_region != NULL) {
            vaddr_t cur_end_address = cur_region->start_vaddr + (cur_region->npages * PAGE_SIZE);
            if (faultaddress >= cur_region->start_vaddr && faultaddress <= cur_end_address) {
                // cur_region is now the region :]
                break;
            }
        }
        // if no valid region
        if (cur_region == NULL) {
            return EFAULT;
        }
        // insert into page table
        vaddr_t vaddr = alloc_kpages(1);
        if (!vaddr) {
            return ENOMEM;
        }
        bzero(&vaddr, PAGE_SIZE);
        if (cur_region->flags & PF_W) {
            as->pt[top_table_index][second_table_index][third_table_index] = KVADDR_TO_PADDR(vaddr) | TLBLO_DIRTY | TLBLO_VALID;
        }
        else {
            as->pt[top_table_index][second_table_index][third_table_index] = KVADDR_TO_PADDR(vaddr) | TLBLO_VALID;
        }
    }

    uint32_t entry_lo = as->pt[top_table_index][second_table_index][third_table_index];

    int spl = splhigh();
    tlb_random(page_number, entry_lo);
    splx(spl); 
    return 0;
}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

