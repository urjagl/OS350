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
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"
/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */
/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12
/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
#if OPT_A3
bool coremapCreated = false;
int *coremap;
unsigned int totalFrames;
paddr_t startaddr;
#endif
void
vm_bootstrap(void)
{
#if OPT_A3
	paddr_t hi;
	paddr_t lo;
	ram_getsize(&lo, &hi);
	coremap = (int*) PADDR_TO_KVADDR(lo);
	totalFrames = (hi-lo)/PAGE_SIZE;
	lo += totalFrames*(sizeof(int));
	while (lo % PAGE_SIZE != 0) {
		lo +=1;
	}
	startaddr = lo;
	totalFrames = (hi-lo)/PAGE_SIZE;
	for (unsigned int i = 0; i < totalFrames; ++i) {
		coremap[i] = 0;
	}
	coremapCreated = true;
#else
#endif /*OPT_A3*/
}
static
paddr_t
getppages(unsigned long npages)
{
#if OPT_A3
	paddr_t addr;
	spinlock_acquire(&stealmem_lock);
	if (!coremapCreated) {
		addr = ram_stealmem(npages);
	} else {
		int pagesNeeded = npages;
		unsigned int i;
		int start = 0;
		for (i = 0; i < totalFrames; i++) {
			if (coremap[i] == 0) {
				pagesNeeded--;
				if (pagesNeeded == 0) {
					break;
				}
			} else {
				pagesNeeded = npages;
				start = i+1;
			}
		}
		
		if (pagesNeeded != 0) {
			spinlock_release(&stealmem_lock);
			kprintf("Out of memory to allocate frames\n");
			return ENOMEM;
		} else {
			//found appropriate pages
			for (unsigned int i = 0; i < npages; ++i) {
				coremap[start+i] = npages;
			}
			addr = startaddr + (start*PAGE_SIZE);
		}
	}
	spinlock_release(&stealmem_lock);
	return addr;
#else
	paddr_t addr;
	spinlock_acquire(&stealmem_lock);
	addr = ram_stealmem(npages);
		
	spinlock_release(&stealmem_lock);
	return addr;
#endif /*OPT_A3*/
}
/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
#if OPT_A3
	if (pa == ENOMEM) {
		return ENOMEM;
	}
#endif /*OPT_A3*/
	return PADDR_TO_KVADDR(pa);
}
void 
free_kpages(vaddr_t addr)
{
#if OPT_A3
	spinlock_acquire(&stealmem_lock);
	if (coremapCreated) {
		if (!addr) {
			spinlock_release(&stealmem_lock);
			kprintf("Freeing error\n");
			return;
		}
		for (unsigned int i = 0; i < totalFrames; ++i) {
			if (PADDR_TO_KVADDR(startaddr+(i*PAGE_SIZE)) == addr) {
				int contiguousBlocks = coremap[i];
				for (int j = 0; j < contiguousBlocks; ++j) {
					coremap[i+j] = 0;
					//kprintf("Setting coremap to 0);
				}
				break;
			}
		}
	}
	spinlock_release(&stealmem_lock);
	return;
#else
	/* nothing - leak the memory. */
	(void)addr;
#endif /*OPT_A3*/
}
void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}
void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}
int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;
	faultaddress &= PAGE_FRAME;
	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);
	switch (faulttype) {
	    case VM_FAULT_READONLY:
#if OPT_A3
	    return 1;
#else
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
#endif /*OPT_A3*/
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}
	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}
	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}
	/* Assert that the address space has been set up properly. */
#if OPT_A3
	KASSERT(as->as_pbase1 != NULL);
	KASSERT(as->as_pbase2 != NULL);
	KASSERT(as->as_stackpbase != NULL);
#else
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_stackpbase != 0);
#endif /*OPT_A3*/
	KASSERT(as->as_vbase1 != 0);
//	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
//	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
//	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
#if OPT_A3
	for (unsigned int i = 0; i < as->as_npages1; ++i) {
		KASSERT((as->as_pbase1[i] & PAGE_FRAME) == as->as_pbase1[i]);
	}
	for (unsigned int j = 0; j < as->as_npages2; ++j) {
		KASSERT((as->as_pbase2[j] & PAGE_FRAME) == as->as_pbase2[j]);
	}
	for (int k = 0; k < DUMBVM_STACKPAGES; ++k) {
		KASSERT((as->as_stackpbase[k] & PAGE_FRAME) == as->as_stackpbase[k]); 
	}
#else
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
#endif /*OPT_A3*/
//	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
//	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
//	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;
	if (faultaddress >= vbase1 && faultaddress < vtop1) {
#if OPT_A3
		int index = (faultaddress - vbase1)/PAGE_SIZE;
		paddr = as->as_pbase1[index];
#else
		paddr = (faultaddress - vbase1) + as->as_pbase1;
#endif /*OPT_A3*/
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
#if OPT_A3
		int index = (faultaddress - vbase2)/PAGE_SIZE;
		paddr = as->as_pbase2[index];
#else
		paddr = (faultaddress - vbase2) + as->as_pbase2;
#endif /*OPT_A3*/
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
#if OPT_A3
		int index = (faultaddress - stackbase)/PAGE_SIZE;
		paddr = as->as_stackpbase[index];
#else
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
#endif /*OPT_A3*/
	}
	else {
		return EFAULT;
	}
	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);
	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();
	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
#if OPT_A3
		if (as->complete && (faultaddress >= vbase1) && (faultaddress < vtop1)) {
			elo &= ~TLBLO_DIRTY;
		}
#endif
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
#if OPT_A3
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	ehi = faultaddress;
	if (as->complete && (faultaddress >= vbase1) && (faultaddress < vtop1)) {
		elo &= ~TLBLO_DIRTY;
	}
	tlb_random(ehi,elo);
	splx(spl);
	return 0;
#else
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
#endif /*OPT_A3*/
}
struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}
#if OPT_A3
	as->as_pbase1 = NULL;
	as->as_pbase2 = NULL;
	as->as_stackpbase = NULL;
#else
	as->as_pbase1 = 0;
	as->as_pbase2 = 0;
	as->as_stackpbase = 0;
#endif /*OPT_A3*/
	as->as_vbase1 = 0;
//	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
//	as->as_pbase2 = 0;
	as->as_npages2 = 0;
//	as->as_stackpbase = 0;
#if OPT_A3
	as->complete = false;
#endif /*OPT_A3*/
	return as;
}
void
as_destroy(struct addrspace *as)
{
#if OPT_A3
	for (unsigned int i = 0; i < as->as_npages1; i++) {
		free_kpages(PADDR_TO_KVADDR(as->as_pbase1[i]));
	}	
	for (unsigned int j = 0; j < as->as_npages2; j++) {
		free_kpages(PADDR_TO_KVADDR(as->as_pbase2[j]));
	}
	for (int k = 0; k < DUMBVM_STACKPAGES; k++) {
		free_kpages(PADDR_TO_KVADDR(as->as_stackpbase[k]));
	}		
	kfree(as->as_pbase1);
	kfree(as->as_pbase2);
	kfree(as->as_stackpbase);
#else
	free_kpages(PADDR_TO_KVADDR(as->as_pbase1));
	free_kpages(PADDR_TO_KVADDR(as->as_pbase2));
	free_kpages(PADDR_TO_KVADDR(as->as_stackpbase));
#endif /*OPT_A3*/
	kfree(as);
}
void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;
	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
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
void
as_deactivate(void)
{
	/* nothing */
}
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 
	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;
	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;
	npages = sz / PAGE_SIZE;
#if OPT_A3
	if (readable) {
		as->readable = true;
	} else {
		as->readable = false;
	}
	if (writeable) {
		as->writeable = true;
	} else {
		as->writeable = false;
	}
	if (executable) {
		as->executable = true;
	} else {
		as->executable = false;
	}
#else
	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;
#endif /*oPT_A3*/
	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
#if OPT_A3
//		kprintf("Number of pages in vbase1: %d\n", npages);
		as->as_pbase1 = kmalloc(sizeof(paddr_t)*npages);
#endif /*OPT_A3*/
		return 0;
	}
	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
#if OPT_A3
		as->as_pbase2 = kmalloc(sizeof(paddr_t)*npages);
#endif /*OPT_A3*/
		return 0;
	}
	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}
static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}
int
as_prepare_load(struct addrspace *as)
{
#if OPT_A3
	KASSERT(as->as_pbase1 != NULL);
	KASSERT(as->as_pbase2 != NULL);
	KASSERT(as->as_stackpbase == NULL);
#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);
#endif
#if OPT_A3
	for (unsigned int i = 0; i < as->as_npages1; ++i) {
		as->as_pbase1[i] = getppages(1);
	}
#else
	as->as_pbase1 = getppages(as->as_npages1);
#endif
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}
#if OPT_A3
	for (unsigned int j = 0; j < as->as_npages2; ++j) {
		as->as_pbase2[j] = getppages(1);
	}
#else
	as->as_pbase2 = getppages(as->as_npages2);
#endif /*OPT_A3*/
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}
#if OPT_A3
	as->as_stackpbase = kmalloc(sizeof(paddr_t)*DUMBVM_STACKPAGES);
	for (int k = 0; k < DUMBVM_STACKPAGES; ++k) {
		as->as_stackpbase[k] = getppages(1);
	}
#else
	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
#endif /*OPT_A3*/
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
#if OPT_A3
	for (unsigned int x = 0; x < as->as_npages1; ++x) {
		as_zero_region(as->as_pbase1[x], 1);
	}
	for(unsigned int y = 0; y < as->as_npages2; ++y) {
		as_zero_region(as->as_pbase2[y], 1);
	}
	for (int z = 0; z < DUMBVM_STACKPAGES; z++) {
		as_zero_region(as->as_stackpbase[z], 1);
	}
#else
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
#endif /*OPT_A3*/
	return 0;
}
int
as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}
int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
#if OPT_A3
	KASSERT(as->as_stackpbase != NULL);
#else
	KASSERT(as->as_stackpbase != 0);
#endif /*OPT_A3*/
	*stackptr = USERSTACK;
	return 0;
}
int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;
	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}
	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;
#if OPT_A3
	new->as_pbase1 = kmalloc(sizeof(paddr_t)*new->as_npages1);
	new->as_pbase2 = kmalloc(sizeof(paddr_t)*new->as_npages2);
#endif /* OPT_A3 */
	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}
#if OPT_A3
	KASSERT(new->as_pbase1 != NULL);
	KASSERT(new->as_pbase2 != NULL);
	KASSERT(new->as_stackpbase != NULL);
#else
	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);
#endif /*OPT_A3*/
#if OPT_A3
	for (unsigned int i = 0; i < old->as_npages1; ++i) {
		memmove((void *)PADDR_TO_KVADDR(new->as_pbase1[i]),
			(const void *)PADDR_TO_KVADDR(old->as_pbase1[i]),
			PAGE_SIZE);
	}
	for (unsigned int j = 0; j < old->as_npages2; ++j) {
		memmove((void *)PADDR_TO_KVADDR(new->as_pbase2[j]),
			(const void *)PADDR_TO_KVADDR(old->as_pbase2[j]),
			PAGE_SIZE);
	}
	for (int k = 0; k < DUMBVM_STACKPAGES; ++k) {
		memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase[k]),
			(const void *)PADDR_TO_KVADDR(old->as_stackpbase[k]),
			PAGE_SIZE);
	}
#else
	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);
	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);
	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
#endif /*OPT_A3*/	
	*ret = new;
	return 0;
}