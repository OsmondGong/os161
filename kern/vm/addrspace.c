/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>

#include <elf.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	// Malloc memory for as
	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	// Malloc memory for top level table
	as->pt = kmalloc(FIRST_LEVEL_SIZE * sizeof(paddr_t **));
	for (int i = 0; i < FIRST_LEVEL_SIZE; i++) {
		// Initialise first level to all NULL
		as->pt[i] = NULL;
	}

	// Initialise as regions as empty
	as->regions = NULL;
	
	
	return as;
}
int
as_copy(struct addrspace *old, struct addrspace **ret)
{	
	struct addrspace *newas;
    newas = as_create();

    // return error if not enough memory
    if (newas == NULL) {
        return ENOMEM;
    }

    // loop through old page table and hard copy contents to new address space
    for (int i = 0; i < FIRST_LEVEL_SIZE; i++) {
        if (!old->pt[i]) continue;
        newas->pt[i] = kmalloc(SECOND_LEVEL_SIZE * sizeof(paddr_t *));
        // return error if not enough memory
        if (newas->pt[i] == NULL) {
            return ENOMEM;
        }
        for (int j = 0; j < SECOND_LEVEL_SIZE; j++) {
            if (old->pt[i][j]) {
                newas->pt[i][j] = kmalloc(THIRD_LEVEL_SIZE *sizeof(paddr_t));
                // return error if not enough memory
                if (newas->pt[i][j] == NULL) {
                    return ENOMEM;
                }
                for (int k = 0; k < THIRD_LEVEL_SIZE; k++) {
                    if (old->pt[i][j][k]) {
                        int dirty_bit = old->pt[i][j][k] & TLBLO_DIRTY;
                        vaddr_t new_frame_addr = alloc_kpages(1);
                        memcpy((void *)new_frame_addr, (const void *)PADDR_TO_KVADDR(old->pt[i][j][k] & PAGE_FRAME), PAGE_SIZE);
                        newas->pt[i][j][k] = (KVADDR_TO_PADDR(new_frame_addr) & PAGE_FRAME) | dirty_bit | TLBLO_VALID;
                    }
                    else {
                        newas->pt[i][j][k] = 0;
                    }
                }
            }
            else {
                newas->pt[i][j] = NULL;
            }

        }
    }

	// no regions
    if (old->regions == NULL) {
        newas->regions = NULL;
        *ret = newas;
        return 0;
    }

	// copy head of linked list
    struct region *reg = kmalloc(sizeof(struct region));
	if (!reg) {
		as_destroy(newas);
		return ENOMEM;
	}
    reg->start_vaddr = old->regions->start_vaddr;
    reg->npages = old->regions->npages;
    reg->writeable = old->regions->writeable;
    reg->old_writeable = old->regions->old_writeable;
    reg->next = NULL;
	newas->regions = reg;
    struct region *list = old->regions->next;
	
    // copy rest of linked list
    while(list) {
        struct region *temp_reg = kmalloc(sizeof(struct region));
        if (!temp_reg) {
            as_destroy(newas);
            return ENOMEM;
        }
        temp_reg->start_vaddr = list->start_vaddr;
        temp_reg->npages = list->npages;
        temp_reg->writeable = list->writeable;
        temp_reg->old_writeable = list->old_writeable;
		temp_reg->next = NULL;
		reg->next = temp_reg;
		reg = reg->next;
        list = list->next;
    }
    
    *ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{	
	// Free page table
	for (int i = 0; i < FIRST_LEVEL_SIZE; i++) {
		if (!as->pt[i]) continue;
		for (int j = 0; j < SECOND_LEVEL_SIZE; j++) {
			if (!as->pt[i][j]) continue;
			for (int k = 0; k < THIRD_LEVEL_SIZE; k++) {
				if (!as->pt[i][j][k]) continue;
				free_kpages(PADDR_TO_KVADDR(as->pt[i][j][k] & PAGE_FRAME));
			}
			kfree(as->pt[i][j]);
		}
		kfree(as->pt[i]);
	}
	kfree(as->pt);

	// Free region linked list
	struct region *cur = as->regions;
	struct region *tmp;
	while (cur != NULL) {
		tmp = cur;
		cur = cur->next;
		kfree(tmp);
	}

	kfree(as);

	as = NULL;
}

// Put translations into TLB of current address space, flush TLB
void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

// Remove translations in TLB, flush TLB (override with invalid entries)
void
as_deactivate(void)
{
	/* nothing */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
	size_t npages = memsize / PAGE_SIZE;
	if (memsize % PAGE_SIZE) npages++; 		// round up npages

	// Make region
	struct region *r = kmalloc(sizeof(struct region));
	if (r == NULL) return ENOMEM;
	r->start_vaddr = vaddr & PAGE_FRAME;
	r->npages = npages;
	r->writeable = r->old_writeable = writeable;
	r->next = NULL;
	
	// Add region to address space
	if (as->regions == NULL) as->regions = r;
	else {
		struct region *cur = as->regions;
		struct region *prev = NULL;
		// Find correct vaddr
		while (cur != NULL) {
			if (cur->start_vaddr >= r->start_vaddr) break;
			prev = cur;
			cur = cur->next;
		}
		// At correct vadrr
		prev->next = r;
		r->next = cur;
	}

	//(void)as;
	//(void)vaddr;
	//(void)memsize;
	(void)readable;
	//(void)writeable;
	(void)executable;
	
	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	// Set as writeable for each region in address space
	struct region *cur = as->regions;
	while (cur != NULL) {
        cur->writeable = 1;
        cur = cur->next;
	}

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	// set back to READ ONLY for READ ONLY regions
	struct region *cur = as->regions;
	while (cur != NULL) {
        cur->writeable = cur->old_writeable;
        cur = cur->next;
	}

	// flush TLB at end since TLB has writing enabled while loading segments
	as_activate();

	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	// Bottom of stack (16 pages) to top of stack defined as RW
	int err = as_define_region(as, USERSTACK - VIRTUAL_STACK_SIZE, VIRTUAL_STACK_SIZE, 1, 1, 0);
	if (err) return err;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}

