#ifndef MY_VM_H_INCLUDED
#define MY_VM_H_INCLUDED
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * Assume the address space is 32 bits, so the max memory size is 4GB
 * Page size is 4KB
 */

/* Add any important includes here which you may need */

#define PGSIZE 4096

// Maximum size of virtual memory
#define MAX_MEMSIZE 4ULL*1024*1024*1024

// Size of "physcial memory"
#define MEMSIZE 1024*1024*1024

// Represents a page table entry
typedef unsigned long pte_t;

// Represents a page directory entry
typedef unsigned long pde_t;

#define TLB_ENTRIES 512

struct tlb {
	/*
	 * We store the virtual address (tag) so we can convert
	 * directly to a page number. Also we can hold a valid variable
	 * to keep track if we can use a mapping through multiple malloc's
	 * and free's
	 */
	struct {
		unsigned long virt_addr;
		unsigned long page_number;
		bool valid;
	} entries[TLB_ENTRIES];
};

pte_t *check_TLB(void *va);
pte_t* translate(pde_t *pgdir, void *va);
int add_TLB(void *va, void *pa);
int page_map(pde_t *pgdir, void *va, void* pa);
void a_free(void *va, int size);
void *a_malloc(unsigned int num_bytes);
void put_value(void *va, void *val, int size);
void get_value(void *va, void *val, int size);
void mat_mult(void *mat1, void *mat2, int size, void *answer);
void print_TLB_missrate(void);
void set_physical_mem(void);

#endif
