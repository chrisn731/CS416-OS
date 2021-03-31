#include "my_vm.h"
#include <err.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#define ADDR_BITS 32

#define NUM_PAGES (MEMSIZE / PGSIZE)
#define log_2(num) (log(num) / log(2))

#define top_bits(x, num) ((x) >> (32 - num))
#define mid_bits(x, num_mid, num_lower) ((x >> (num_lower)) & ((1UL << (num_mid)) - 1))
#define low_bits(x, num) (((1UL << num) - 1) & (x))

#define set_bit(x, num) ((x) |= (1UL << (num)))
#define clear_bit(x, num) ((x) &= ~(1UL << (num)))

#define valid_bit_set(x) ((x) & 0x80000000)
#define set_valid_bit(x) (set_bit(x, 31))

#define free_bit_set(x) ((x) & 0x40000000)
#define set_free_bit(x) (set_bit(x, 30))

static struct tlb tlb_store;

static unsigned long phys_mem_size;
static void *phys_mem;
static pde_t *page_dir;

static unsigned int off_bits;
static unsigned int page_dir_bits;
static unsigned int page_table_bits;
static unsigned int phys_page_bits;

static unsigned int next_page;

static pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;

static void free_phys_mem(void)
{
	munmap(phys_mem, phys_mem_size);
}

static void init_page_directory(void)
{
	pte_t *first_table;
	unsigned int i, entries = PGSIZE / sizeof(pte_t);
	page_dir = phys_mem;

	/* We allocate the first page table for the directory to use. */
	*page_dir = 1;
	first_table = (pte_t *) (((char *) phys_mem) + (1 * PGSIZE));

	for (i = 0; i < entries; i++) {
		set_valid_bit(first_table[i]);
		set_free_bit(first_table[i]);
		/* First 2 pages go to directory and first table */
		first_table[i] += i + 2;
	}
	set_valid_bit(*page_dir);
}

/* Function responsible for allocating and setting your physical memory */
void set_physical_mem(void)
{
	/*
	 * Allocate physical memory using mmap or malloc; this is the total size of
	 * your memory you are simulating
	 *
	 * HINT: Also calculate the number of physical and virtual pages and allocate
	 * virtual and physical bitmaps and initialize them
	 */
	phys_mem_size = MEMSIZE < MAX_MEMSIZE ? MEMSIZE : MAX_MEMSIZE;
	phys_mem = mmap(NULL, phys_mem_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (phys_mem == MAP_FAILED)
		err(-1, "%s: Error allocating %lu bytes for physical memory",
				__func__, phys_mem_size);

	/* memset(phys_mem, 0, phys_mem_size); */
	off_bits = (unsigned int) log_2(PGSIZE);
	page_table_bits = (unsigned int) log_2(PGSIZE / sizeof(pte_t));
	page_dir_bits = ADDR_BITS - page_table_bits - off_bits;
	/* How many bits we use to address the physical pages */
	phys_page_bits = log_2(NUM_PAGES);

	init_page_directory();

	/* Free our physical memory when our application finishes */
	if (atexit(&free_phys_mem) != 0)
		warn("%s: Setup of automatic freeing of memory failed. "
			"Physical memory will not be freed at program exit.",
			__func__);
}


/*
 * Part 2: Add a virtual to physical page translation to the TLB.
 * Feel free to extend the function arguments or return type.
 */
int add_TLB(void *va, void *pa)
{
	/* Part 2 HINT: Add a virtual to physical page translation to the TLB */

	return -1;
}


/*
 * Part 2: Check TLB for a valid translation.
 * Returns the physical page address.
 * Feel free to extend this function and change the return type.
 */
pte_t *check_TLB(void *va)
{
	/* Part 2: TLB lookup code here */
	return NULL;
}


/*
 * Part 2: Print TLB miss rate.
 * Feel free to extend the function arguments or return type.
 */
void print_TLB_missrate(void)
{
	double miss_rate = 0;

	/* Part 2 Code here to calculate and print the TLB miss rate */

	fprintf(stderr, "TLB miss rate %lf \n", miss_rate);
}

/*
 * The function takes a virtual address and page directories starting address and
 * performs translation to return the physical address
 */
pte_t *translate(pde_t *pgdir, void *va)
{
	pte_t table_entry, *page_table;
	pde_t dir_entry;
	unsigned long dir_index, offset, table_index, table_num, page_num, addr;
	unsigned long virt_addr = (unsigned long) va;

	/*
	 * Part 1 HINT: Get the Page directory index (1st level) Then get the
	 * 2nd-level-page table index using the virtual address.  Using the page
	 * directory index and page table index get the physical address.
	 *
	 * Part 2 HINT: Check the TLB before performing the translation. If
	 * translation exists, then you can return physical address from the TLB.
	 */
	if (!pgdir || !va)
		return NULL;

	/*
	 * Take apart the virtual address. In the case of 4K pages, the values
	 * would be 10 for dir_index, 10 for table_index, and 12 for the offset.
	 */
	dir_index = top_bits(virt_addr, page_dir_bits);
	table_index = mid_bits(virt_addr, page_table_bits, off_bits);
	offset = low_bits(virt_addr, off_bits);

	/*
	 * Retrieve the entry from the page directory to the get physical page
	 * of our page table.
	 */
	dir_entry = pgdir[dir_index];
	if (!dir_entry)
		return NULL;
	table_num = low_bits(dir_entry, phys_page_bits);

	/* Go to the relevant page table, and retrieve the page table entry. */
	page_table = (pte_t *) ((char *) phys_mem + table_num * PGSIZE);
	table_entry = page_table[table_index];
	if (!table_entry || !valid_bit_set(table_entry))
		return NULL;
	page_num = low_bits(table_entry, phys_page_bits);

	/*
	 * From the page table entry, we can grab the physical page number
	 * and factor in our offset to grab the final address.
	 */
	addr = (unsigned long) ((char *) phys_mem + page_num * PGSIZE);
	addr += offset;
	return (pte_t *) addr;
}


/*
 * The function takes a page directory address, virtual address, physical address
 * as an argument, and sets a page table entry. This function will walk the page
 * directory to see if there is an existing mapping for a virtual address. If the
 * virtual address is not present, then a new entry will be added
 */
int page_map(pde_t *pgdir, void *va, void *pa)
{
	pte_t table_entry, *page_table;
	pde_t dir_entry;
	unsigned long dir_index, table_index, table_num;
	unsigned long virt_addr = (unsigned long) va;
	/*
	 * HINT: Similar to translate(), find the page directory (1st level)
	 * and page table (2nd-level) indices. If no mapping exists, set the
	 * virtual to physical mapping
	 */
	if (!pgdir || !va || !pa)
		return -1;

	dir_index = top_bits(virt_addr, page_dir_bits);
	table_index = mid_bits(virt_addr, page_table_bits, off_bits);

	dir_entry = pgdir[dir_index];
	if (!dir_entry) {
		/* Need to map a new table for our directory. */
		pgdir[dir_index] = next_page++;
	}
	table_num = low_bits(dir_entry, phys_page_bits);

	page_table = (pte_t *) ((char *) phys_mem + table_num * PGSIZE);
	table_entry = page_table[table_index];
	if (!table_entry || !valid_bit_set(table_entry)) {
		/* If there is no entry or non valid, we need to map it. */
		pte_t new_entry;
		unsigned long page_num;

		page_num = ((pte_t) pa - (pte_t) phys_mem) / PGSIZE;
		new_entry = page_num;
		set_valid_bit(new_entry);
		page_table[table_index] = new_entry;
		return 0;
	}
	return -1;
}


/* Function that gets the next available page */
void *get_next_avail(int num_pages)
{
	void *free_page = NULL;
	unsigned int dir_index, table_index, entries_on_page;
	/* Use virtual address bitmap to find the next free page */

	entries_on_page = NUM_PAGES / sizeof(pte_t);

	/* For each entry within the directory... */
	for (dir_index = 0; dir_index < entries_on_page; dir_index++) {
		pde_t dir_entry = page_dir[dir_index];
		pte_t *table;
		unsigned int available_pages = 0;

		if (!dir_entry) {
			/* Allocate a new page */
			page_dir[dir_index] = next_page++;
		}

		table = (pte_t *) (((char *) phys_mem) + dir_entry * PGSIZE);
		/* For each entry within the table */
		for (table_index = 0; table_index < entries_on_page; table_index++) {
			/*
			 * We need to find (num_pages) consecutive entries
			 * so that if we allocate more than one page, the user
			 * refers to the correct pages.
			 */
			if (valid_bit_set(table[table_index])) {
				if (!free_page)
					free_page = &table[table_index];
				available_pages++;
			} else {

				available_pages = 0;
				free_page = NULL;
			}

			if (available_pages == num_pages)
				goto found;

		}
	}
found:
	return free_page;
}


/* Function responsible for allocating pages and used by the benchmark */
void *a_malloc(unsigned int num_bytes)
{
	static int initialized;
	void *free_pages;
	unsigned int num_pages, i;

	if (!num_bytes)
		goto malloc_fail;

	pthread_mutex_lock(&mut);
	if (!initialized) {
		/*
		 * HINT: If the physical memory is not yet initialized,
		 * then allocate and initialize.
		 */
		set_physical_mem();
		initialized = 1;
	}

	num_pages = num_bytes / PGSIZE;
	if (num_bytes % PGSIZE)
		num_pages++;

	free_pages = get_next_avail(num_pages);
	if (!free_pages)
		goto malloc_fail_unlock;

	for (i = 0; i < num_pages; i++) {

	}

	/*
	 * HINT: If the page directory is not initialized, then initialize the
	 * page directory. Next, using get_next_avail(), check if there are free pages. If
	 * free pages are available, set the bitmaps and map a new page. Note, you will
	 * have to mark which physical pages are used.
	 */

malloc_fail_unlock:
	pthread_mutex_unlock(&mut);
malloc_fail:
	return NULL;
}

/*
 * Responsible for releasing one or more memory pages using
 * virtual address (va)
 */
void a_free(void *va, int size)
{
	/*
	 * Part 1: Free the page table entries starting from this virtual address
	 * (va). Also mark the pages free in the bitmap. Perform free only if the
	 * memory from "va" to va+size is valid.
	 *
	 * Part 2: Also, remove the translation from the TLB
	 */
	if (!va || !size)
		return;
}


/*
 * The function copies data pointed by "val" to physical
 * memory pages using virtual address (va)
 */
void put_value(void *va, void *val, int size)
{
	unsigned int i, num_pages = size / PGSIZE;
	char *phys_addr, *val_ptr = val;
	/*
	 * HINT: Using the virtual address and translate(), find the physical page. Copy
	 * the contents of "val" to a physical page. NOTE: The "size" value can be larger
	 * than one page. Therefore, you may have to find multiple pages using translate()
	 * function
	 */
	if (!va || !val || !size)
		return;

	for (i = 0; i < size; i++) {
		phys_addr = (char *) translate(NULL, va);
		if (!phys_addr)
			return;
		*phys_addr = *val_ptr++;
		(char *) va += 1;
	}
}


/*
 * Given a virtual address, this function copies the contents of the page
 * to val
 */
void get_value(void *va, void *val, int size)
{
	int i;
	char *phys_addr, *val_ptr = val;
	/*
	 * HINT: put the values pointed to by "va" inside the physical memory at given
	 * "val" address. Assume you can access "val" directly by derefencing them
	 */
	if (!va || !val || !size)
		return;

	phys_addr = (char *) translate(NULL, va);
	if (!phys_addr)
		return;

	for (i = 0; i < size; i++)
		*val_ptr++ = *phys_addr++;
}



/*
 * This function receives two matrices mat1 and mat2 as an argument with size
 * argument representing the number of rows and columns. After performing matrix
 * multiplication, copy the result to answer.
 */
void mat_mult(void *mat1, void *mat2, int size, void *answer)
{
	/*
	 * Hint: You will index as [i * size + j] where  "i, j" are the indices of the
	 * matrix accessed. Similar to the code in test.c, you will use get_value() to
	 * load each element and perform multiplication. Take a look at test.c! In addition to
	 * getting the values from two matrices, you will perform multiplication and
	 * store the result to the "answer array"
	 */
	if (!mat1 || !mat2)
		return;
}
