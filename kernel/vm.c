#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "fcntl.h"
#include "stddef.h"
extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
	struct cpu *c;

	// Map "logical" addresses to virtual addresses using identity map.
	// Cannot share a CODE descriptor for both kernel and user
	// because it would have to have DPL_USR, but the CPU forbids
	// an interrupt from CPL=0 to DPL=3.
	c = &cpus[cpuid()];
	c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
	c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
	c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
	c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
	lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
	pde_t *pde;
	pte_t *pgtab;

	pde = &pgdir[PDX(va)];
	if(*pde & PTE_P){
		pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
	} else {
		if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
			return 0;
		// Make sure all those PTE_P bits are zero.
		memset(pgtab, 0, PGSIZE);
		// The permissions here are overly generous, but they can
		// be further restricted by the permissions in the page table
		// entries, if necessary.
		*pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
	}
	return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{//(mappages(pgdir, pocetak, 4096, V2P(shm->adress[j]), mode)
	char *a, *last;
	pte_t *pte;

	a = (char*)PGROUNDDOWN((uint)va);
	last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
	for(;;){
		if((pte = walkpgdir(pgdir, a, 1)) == 0)
			return -1;
		if(*pte & PTE_P)
			panic("remap");
		*pte = pa | perm | PTE_P;
		if(a == last)
			break;
		a += PGSIZE;
		pa += PGSIZE;
	}
	return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
    void *virt;
    uint phys_start;
    uint phys_end;
    int perm;
} kmap[] = {
    { (void*)KERNBASE, 0,             EXTMEM,    PTE_W},   // I/O space
    { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},       // Kern text+rodata
    { (void*)data,     V2P(data),     PHYSTOP,   PTE_W},   // Kern data+memory
    { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W},   // More devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
	pde_t *pgdir;
	struct kmap *k;

	if((pgdir = (pde_t*)kalloc()) == 0)
		return 0;
	memset(pgdir, 0, PGSIZE);
	if (P2V(PHYSTOP) > (void*)DEVSPACE)
		panic("PHYSTOP too high");
	for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
		if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
		            (uint)k->phys_start, k->perm) < 0) {
			freevm(pgdir);
			return 0;
		}
	return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
	kpgdir = setupkvm();
	switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
	lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
	if(p == 0)
		panic("switchuvm: no process");
	if(p->kstack == 0)
		panic("switchuvm: no kstack");
	if(p->pgdir == 0)
		panic("switchuvm: no pgdir");

	pushcli();
	mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
		sizeof(mycpu()->ts)-1, 0);
	SEG_CLS(mycpu()->gdt[SEG_TSS]);
	mycpu()->ts.ss0 = SEG_KDATA << 3;
	mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
	// setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
	// forbids I/O instructions (e.g., inb and outb) from user space
	mycpu()->ts.iomb = (ushort) 0xFFFF;
	ltr(SEG_TSS << 3);
	lcr3(V2P(p->pgdir));  // switch to process's address space
	popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
	char *mem;

	if(sz >= PGSIZE)
		panic("inituvm: more than a page");
	mem = kalloc();
	memset(mem, 0, PGSIZE);
	mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
	memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
	uint i, pa, n;
	pte_t *pte;

	if((uint) addr % PGSIZE != 0)
		panic("loaduvm: addr must be page aligned");
	for(i = 0; i < sz; i += PGSIZE){
		if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
			panic("loaduvm: address should exist");
		pa = PTE_ADDR(*pte);
		if(sz - i < PGSIZE)
			n = sz - i;
		else
			n = PGSIZE;
		if(readi(ip, P2V(pa), offset+i, n) != n)
			return -1;
	}
	return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
	char *mem;
	uint a;

	if(newsz >= KERNBASE)
		return 0;

	if(newsz < oldsz)
		return oldsz;

	a = PGROUNDUP(oldsz);
	for(; a < newsz; a += PGSIZE){
		mem = kalloc();
		if(mem == 0){
			cprintf("allocuvm out of memory\n");
			deallocuvm(pgdir, newsz, oldsz);
			return 0;
		}
		memset(mem, 0, PGSIZE);
		if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
			cprintf("allocuvm out of memory (2)\n");
			deallocuvm(pgdir, newsz, oldsz);
			kfree(mem);
			return 0;
		}
	}
	return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
	pte_t *pte;
	uint a, pa;

	if(newsz >= oldsz)
		return oldsz;

	a = PGROUNDUP(newsz);
	for(; a  < oldsz; a += PGSIZE){
		pte = walkpgdir(pgdir, (char*)a, 0);
		if(!pte)
			a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
		else if((*pte & PTE_P) != 0){
			pa = PTE_ADDR(*pte);
			if(pa == 0)
				panic("kfree");
			char *v = P2V(pa);
			kfree(v);
			*pte = 0;
		}
	}
	return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
	uint i;

	if(pgdir == 0)
		panic("freevm: no pgdir");
	deallocuvm(pgdir, KERNBASE, 0);
	for(i = 0; i < NPDENTRIES; i++){
		if(pgdir[i] & PTE_P){
			char * v = P2V(PTE_ADDR(pgdir[i]));
			kfree(v);
		}
	}
	kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
	pte_t *pte;

	pte = walkpgdir(pgdir, uva, 0);
	if(pte == 0)
		panic("clearpteu");
	*pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
	pde_t *d;
	pte_t *pte;
	uint pa, i, flags;
	char *mem;

	if((d = setupkvm()) == 0)
		return 0;
	for(i = 0; i < sz; i += PGSIZE){
		if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
			panic("copyuvm: pte should exist");
		if(!(*pte & PTE_P))
			panic("copyuvm: page not present");
		pa = PTE_ADDR(*pte);
		flags = PTE_FLAGS(*pte);
		if((mem = kalloc()) == 0)
			goto bad;
		memmove(mem, (char*)P2V(pa), PGSIZE);
		if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
			kfree(mem);
			goto bad;
		}
	}
	return d;

bad:
	freevm(d);
	return 0;
}

// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
	pte_t *pte;

	pte = walkpgdir(pgdir, uva, 0);
	if((*pte & PTE_P) == 0)
		return 0;
	if((*pte & PTE_U) == 0)
		return 0;
	return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
	char *buf, *pa0;
	uint n, va0;

	buf = (char*)p;
	while(len > 0){
		va0 = (uint)PGROUNDDOWN(va);
		pa0 = uva2ka(pgdir, (char*)va0);
		if(pa0 == 0)
			return -1;
		n = PGSIZE - (va - va0);
		if(n > len)
			n = len;
		memmove(pa0 + (va - va0), buf, n);
		len -= n;
		buf += n;
		va = va0 + PGSIZE;
	}
	return 0;
}
#define MAX_SHM_OBJS 64
#define MAX_SHM_OBJS_FOR_PROCESS 16

static int num_of_shm_objs = 0;
static struct shm_obj* shm_objs[MAX_SHM_OBJS] = {NULL};

int find_empty_index() {
	for (int i = 0; i < MAX_SHM_OBJS; i++) {
		if (shm_objs[i] == NULL) {
			return i;
		}
	}

	return -1;
}

int find_empty_index_in_proc_oobj(struct proc* p) {
	for (int i = 0; i < MAX_SHM_OBJS_FOR_PROCESS; i++) {
		if (p->oobj[i] == NULL) {
			return i;
		}
	}

	return -1;
}

int process_already_oppened_shm_obj(struct shm_obj* shm, struct proc* p) {
	for (int i = 0; i < MAX_SHM_OBJS_FOR_PROCESS; i++) {
		if (p->oobj[i] == shm) {
			return 1;
		}
	}

	return 0;
}

void shm_close_wrapper(struct proc *p, int id, int *uspelo)
{
	struct shm_obj *shm;
	int shm_od;
	int z = strlen(p->oobj[id]->name);
	for(int i = 0; i < MAX_SHM_OBJS; i++)
	{
		if(strncmp(shm_objs[i]->name, p->oobj[id]->name, z) == 0)
		{
			shm = shm_objs[i];
			shm_od = i;
			break;
		}
	}

	void *va = p->virtual_addrs;
	void *last = va + shm->size;

	while(va < last){
		pte_t *pte;
		pte = walkpgdir(p->pgdir, va, 0);
		if (pte != 0)
			*pte = 0;
		va += 4096;
	}

	p->virtual_addrs = 0;
	p->oobj[id] = NULL;
	p->num_of_opened_shm_objs--;

	shm->num_of_processes--;
	if(shm->num_of_processes == 0)
	{
		int pages = shm->size / 4096;
		for(int i = 0; i < pages; i++){
			kfree((char *)shm->adress[i]);
		}
		kfree((char *)shm);
		shm_objs[shm_od] = NULL;
		num_of_shm_objs--;
    }
	*uspelo = 0;
	return;
}

void shm_map_wrapper(struct proc *p, int id, int *uspelo, int mode)
{
	void **va;

	struct shm_obj *shm;

	int z = strlen(p->oobj[id]->name);
	for(int i = 0; i < MAX_SHM_OBJS; i++)
	{
		if(strncmp(shm_objs[i]->name, p->oobj[id]->name, z) == 0)
		{
			shm = shm_objs[i];
			break;
		}
	}

	int perm = PTE_W|PTE_U;
	if(mode == O_WRONLY){
		*uspelo = -1;
		return;
	}
	else if(mode == O_RDONLY)
		perm = PTE_U;

	pde_t *pgdir = p->pgdir;
	pte_t *pte;

	if(p->virtual_addrs != 0){
		cprintf("\n Virtual address for process with name: %s is already mapped \n", p->name);
		*uspelo = -1;
		return;
	}

	void *a = (void*)(KERNBASE / 2);
	void *last = (void*)KERNBASE;
	int size = shm->size;
	*va = a;

	for(int j = 0; j < 32; j++){
		if((pte = walkpgdir(pgdir, a, 0)) == 0  || (*pte & PTE_P) == 0)
		{
			uint pa = V2P(shm->adress[j]);
			if(mappages(pgdir, a, 4096, pa, perm) == 0) {
				size -= 4096;
			}
		} else{
			*pte = 0;
			cprintf("Failed to map\n");
			*uspelo = -1;
			return;
		}
		if(a == last || size <= 0)
			break;
		a += PGSIZE;
	}
	p->virtual_addrs = *va;

	*uspelo = 0;
	return;
}
int
shm_open(void)
{
	char *name;
	if(argstr(0, &name) < 0) {
		return -1;
	}

	if(strlen(name) == 0) {
		cprintf("shm name can't be empty");
		return -1;
	}

	struct proc *p = myproc();

	if (p->num_of_opened_shm_objs == MAX_SHM_OBJS_FOR_PROCESS) {
		cprintf("Process with name: %s opened max number of shm_objs. Couldn't open shm_obj with name: %s", p->name, name);
		return -1;
	}

	int found_shm = 0;
	int index = -1;
	struct shm_obj* shm;
	for (int i = 0; i < MAX_SHM_OBJS; i++) {
		shm = shm_objs[i];
		if (shm == NULL) {
			continue;
		}

		int shm_name_length = strlen(shm->name);
		int name_length = strlen(name);

		if (strncmp(shm->name, name, shm_name_length >= name_length ? shm_name_length : name_length) == 0) {
			found_shm = 1;
			index = i;
			break;
		}
	}

	if (found_shm == 0) {
		if (num_of_shm_objs == MAX_SHM_OBJS) {
			cprintf("Max number of shm_objs reached. Couldn't allocate memory for new shm_obj");
			return -1;
		}

		index = find_empty_index();
		if (index == -1) {
			cprintf("There is no space in shm_obj to insert new shm_obj with name: %s", name);
			return -1;
		}

		shm = (struct shm_obj*)kalloc();
		if (shm == NULL) {
			cprintf("Couldn't allocate memory for shm with name: %s", name);
			return -1;
		}

		for (int i = 0; i < strlen(name); i++) {
			shm->name[i] = name[i];
		}
		shm->name[strlen(name)] = '\0';

		shm->size = 0;
		shm->num_of_processes = 1;
		shm->trunc_called = 0;

		shm_objs[index] = shm;
		num_of_shm_objs++;

		int index_in_proc = find_empty_index_in_proc_oobj(p);

		p->oobj[index_in_proc] = shm;
		p->virtual_addrs = 0;
		p->num_of_opened_shm_objs++;

		return index;
	}

	shm = shm_objs[index];

	if (process_already_oppened_shm_obj(shm, p) == 1) {
		return index;
	}

	shm->num_of_processes++;

	int index_in_proc = find_empty_index_in_proc_oobj(p);

	p->oobj[index_in_proc] = shm;
	p->virtual_addrs = 0;
	p->num_of_opened_shm_objs++;

	return index;
}
int
shm_trunc(void)
{
	int size, id, bytes;

	if(argint(0, &id) < 0 || argint(1, &size) < 0) {
		return -1;
	}

	if(id < 0 || id >= MAX_SHM_OBJS) {
		return -1;
	}

	struct shm_obj *shm = shm_objs[id];

	if (shm->trunc_called == 1) {
		return shm->size;
	}

	int counter = 0;
	bytes = size % 4096;
	if(bytes > 0)
		bytes = size / 4096 + 1;
	else
		bytes = size / 4096;
	if(bytes > 32)
		return -1;
	while(bytes)
	{
		void *mem = kalloc();
		if(!mem){
			kfree(mem);
			while(counter)
			{
				kfree(shm->adress[--counter]);
			}
			shm->size = 0;
			return -1;
		}
		memset(mem, 0, 4096);
		shm->adress[counter] = mem;
		counter++;
		shm->size = 4096 * counter;
		--bytes;
	}

	shm->trunc_called = 1;
	return shm->size;
}
int
shm_map(void) {
	void **va;
	int id, mode;

	struct proc *p = myproc();
	if(argint(0, &id) < 0 || argptr(1, (void *)&va, sizeof(void *)) < 0 || argint(2, &mode) < 0) {
		return -1;
	}
	p->mode = mode;

	struct shm_obj *shm = shm_objs[id];

	int perm = PTE_W|PTE_U;
	if(mode == O_WRONLY)
		return -1;
	else if(mode == O_RDONLY)
		perm = PTE_U;

	pde_t *pgdir = p->pgdir;

	if(p->virtual_addrs != 0){
		cprintf("\n Virtual address for process with name: %s is already mapped \n", p->name);
		return -1;
	}

	void *a = (void*)(KERNBASE / 2);
	void *last = (void*)KERNBASE;
	int size = shm->size;
	*va = a;

	for(int j = 0; j < 32; j++){
		uint pa = V2P(shm->adress[j]);
		if(mappages(pgdir, a, 4096, pa, perm) == 0) {
			size -= 4096;
		} else{
			cprintf("Failed to map\n");
			return -1;
		}
		if(a == last || size <= 0)
			break;
		a += PGSIZE;
	}
	p->virtual_addrs = *va;

	return 0;
}

int
shm_close(void)
{
	int id;
	struct proc *p = myproc();
	struct shm_obj *shm;

	if(argint(0, &id) < 0) {
		return -1;
	}

	shm = shm_objs[id];
	int index_in_proc = -1;

	for(int i = 0; i < MAX_SHM_OBJS_FOR_PROCESS; i++) {
		if (p->oobj[i] == shm) {
			index_in_proc = i;
			break;
		}
	}

	if (index_in_proc == -1) {
		cprintf("\n Couldn't find shm_obj with id: %d for process with name: %s \n", id, p->name);
		return -1;
	}

	void *va = p->virtual_addrs;
	void *last = va + shm->size;

	while(va < last){
		pte_t *pte;
		pte = walkpgdir(p->pgdir, va, 0);
		if (pte != 0)
			*pte = 0;
		va += 4096;
	}

	p->virtual_addrs = 0;
	p->oobj[index_in_proc] = NULL;
	p->num_of_opened_shm_objs--;

	shm->num_of_processes--;
	if(shm->num_of_processes == 0)
	{
		int pages = shm->size / 4096;

		for(int i = 0; i < pages; i++){
			kfree((char *)shm->adress[i]);
		}

		kfree((char *)shm);
		shm_objs[id] = NULL;
		num_of_shm_objs--;

    }

	return 0;
}
