#include "my_vm.h"
#include <err.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#define ADDR_BITS 32

#define log_2(num) (log(num) / log(2))

#define top_bits(x, num) ((x) >> (32 - num))
#define mid_bits(x, num_mid, num_lower) ((x >> (num_lower)) & ((1UL << (num_mid)) - 1))
#define low_bits(x, num) (((1UL << num) - 1) & (x))

#define map_set_bit(map, index) \
	(((char *) map)[(index) / 8] |= (1UL << ((index) % 8)))

#define map_clear_bit(map, index) \
	(((char *) map)[(index) / 8] &= ~(1UL << ((index) % 8)))

#define map_get_bit(map, index) \
	(((char *) map)[(index) / 8] & (1UL << ((index) % 8)))

static struct tlb tlb_store;

static unsigned long total_pages;

static unsigned long phys_mem_size;
static void *phys_mem;
static pde_t *page_dir;

static unsigned int off_bits;
static unsigned int page_dir_bits;
static unsigned int page_table_bits;
static unsigned int phys_page_bits;

static unsigned char *alloc_map;

static pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;

static void free_phys_mem(void)
{
	munmap(phys_mem, phys_mem_size);
	free(alloc_map);
}

/* Function responsible for allocating and setting your physical memory */
void set_physical_mem(void)
{
	unsigned long phys_map_size;

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
	page_dir = phys_mem;

	off_bits = (unsigned int) log_2(PGSIZE);
	page_table_bits = (unsigned int) log_2(PGSIZE / sizeof(pte_t));
	page_dir_bits = ADDR_BITS - page_table_bits - off_bits;

	/* How many bits we use to address the physical pages */
	phys_page_bits = page_table_bits + page_dir_bits;
	total_pages = phys_mem_size / PGSIZE;

	/*
	 * The size of the bit map will be:
	 * 2^(page_table_bits + page_dir_bits) / (# bits in char)
	 * # bits in char = 2^3
	 */
	phys_map_size = (1UL << (page_table_bits + page_dir_bits)) >> 3;
	alloc_map = malloc(phys_map_size);
	if (!alloc_map)
		err(-1, "%s: Error allocating %lu bytes for bitmap",
				__func__, phys_map_size);
	memset(alloc_map, 0, phys_map_size);

	/* We use page 0 as our directory */
	map_set_bit(alloc_map, 0);

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
	unsigned long i, tag;

	/* Part 2 HINT: Add a virtual to physical page translation to the TLB */
	tag = (unsigned long) va >> off_bits;
	i = tag % TLB_ENTRIES;

	tlb_store.entries[i].virt_addr = tag;
	tlb_store.entries[i].page_number = (unsigned long) pa;
	tlb_store.entries[i].valid = true;
	return 0;
}

static unsigned int tlb_misses;
static unsigned int tlb_lookups;

/*
 * Part 2: Check TLB for a valid translation.
 * Returns the physical page address.
 * Feel free to extend this function and change the return type.
 */
pte_t *check_TLB(void *va)
{
	unsigned long i, tag;

	tag = (unsigned long) va >> off_bits;
	i = tag % TLB_ENTRIES;

	tlb_lookups++;
	if (tlb_store.entries[i].valid && tlb_store.entries[i].virt_addr == tag)
		return (pte_t *) tlb_store.entries[i].page_number;
	tlb_misses++;
	return NULL;
}

/*
 * Part 2: Print TLB miss rate.
 * Feel free to extend the function arguments or return type.
 */
void print_TLB_missrate(void)
{
	double miss_rate;

	miss_rate = tlb_lookups ? (double) tlb_misses / tlb_lookups : 0.0;
	fprintf(stderr, "TLB miss rate %lf\n", miss_rate);
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

	page_num = (unsigned long) check_TLB(va);
	if (page_num)
		goto tlb_hit;

	/*
	 * Retrieve the entry from the page directory to the get physical page
	 * of our page table.
	 */
	dir_entry = pgdir[dir_index];
	if (!dir_entry) {
		printf("%s: No directory entry for 0x%p", __func__, va);
		return NULL;
	}
	table_num = low_bits(dir_entry, phys_page_bits);

	/* Go to the relevant page table, and retrieve the page table entry. */
	page_table = (pte_t *) ((char *) phys_mem + table_num * PGSIZE);
	table_entry = page_table[table_index];
	if (!table_entry) {
		printf("%s: No table entry for 0x%p", __func__, va);
		return NULL;
	}
	page_num = low_bits(table_entry, phys_page_bits);

	/*
	 * From the page table entry or TLB, we grab the physical page number
	 * and factor in our offset to grab the final address.
	 */
tlb_hit:
	addr = (unsigned long) ((char *) phys_mem + page_num * PGSIZE);
	addr += offset;
	return (pte_t *) addr;
}

static pde_t alloc_new_table(void)
{
	unsigned long i;

	for (i = 0; i < total_pages; i++) {
		if (!map_get_bit(alloc_map, i)) {
			map_set_bit(alloc_map, i);
			return i;
		}
	}
	return 0;
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
		/* We are mapping to a new table */
		dir_entry = alloc_new_table();
		map_set_bit(alloc_map, dir_entry);
		printf("Allocating new page table at ppn: %lu\n", dir_entry);
		pgdir[dir_index] = dir_entry;
	}
	table_num = low_bits(dir_entry, phys_page_bits);

	page_table = (pte_t *) ((char *) phys_mem + table_num * PGSIZE);
	table_entry = page_table[table_index];
	if (!table_entry) {
		/* If there is no entry or non valid, we need to map it. */
		pte_t new_entry;
		unsigned long page_num;

		page_num = ((pte_t) pa - (pte_t) phys_mem) / PGSIZE;
		new_entry = page_num;
		page_table[table_index] = new_entry;
		add_TLB(va, (void *) page_num);
	}
	return 0;
}


/* Function that gets the next available page */
void *get_next_avail(int num_pages)
{
	unsigned int i, free_page = 0, available_pages = 0;

	for (i = 0; i < total_pages; i++) {
		if (!map_get_bit(alloc_map, i)) {
			if (!free_page)
				free_page = i;
			available_pages++;
			if (available_pages == num_pages)
				return (void *) free_page;
		} else {
			free_page = 0;
			available_pages = 0;
		}
	}
	return 0;
}

static void *create_virt_addr(unsigned long ppn)
{
	unsigned long new_va, entries_on_dir, entries_on_table;

	entries_on_dir = 1 << page_dir_bits;
	entries_on_table = 1 << page_table_bits;
	new_va = (ppn / entries_on_dir) << page_table_bits;
	new_va |= ppn % entries_on_table;
	new_va <<= off_bits;
	return (void *) new_va;
}

/* Function responsible for allocating pages and used by the benchmark */
void *a_malloc(unsigned int num_bytes)
{
	static int initialized;
	void *va = NULL;
	unsigned long page_num;
	unsigned int num_pages, i;

	if (!num_bytes)
		return NULL;

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

	page_num = (unsigned long) get_next_avail(num_pages);
	if (!page_num)
		goto malloc_fail_unlock;
	for (i = 0; i < num_pages; i++)
		map_set_bit(alloc_map, page_num + i);

	printf("Allocating %u page(s) starting at ppn: %lu\n", num_pages, page_num);

	va = create_virt_addr(page_num);
	page_map(page_dir, va, (char *) phys_mem + page_num * PGSIZE);

	/* Allocate extra pages if we need to */
	for (i = 1; i < num_pages; i++) {
		page_num++;
		page_map(page_dir, create_virt_addr(page_num),
				(char *) phys_mem + page_num * PGSIZE);
	}

	/*
	 * HINT: If the page directory is not initialized, then initialize the
	 * page directory. Next, using get_next_avail(), check if there are free pages. If
	 * free pages are available, set the bitmaps and map a new page. Note, you will
	 * have to mark which physical pages are used.
	 */

malloc_fail_unlock:
	pthread_mutex_unlock(&mut);
	return va;
}

/*
 * Responsible for releasing one or more memory pages using
 * virtual address (va)
 */
void a_free(void *va, int size)
{
	unsigned long i, num_to_free, phys_addr, ppn;

	/*
	 * Part 1: Free the page table entries starting from this virtual address
	 * (va). Also mark the pages free in the bitmap. Perform free only if the
	 * memory from "va" to va+size is valid.
	 *
	 * Part 2: Also, remove the translation from the TLB
	 */
	if (!va || size <= 0)
		return;

	num_to_free = size / PGSIZE;
	if (size % PGSIZE)
		num_to_free++;

	phys_addr = (unsigned long) translate(page_dir, va);
	if (!phys_addr)
		return;

	ppn = (phys_addr - (unsigned long) phys_mem) / PGSIZE;
	pthread_mutex_lock(&mut);
	for (i = 0; i < num_to_free; i++, ppn++) {
		if (!map_get_bit(alloc_map, ppn))
			break;
		map_clear_bit(alloc_map, ppn);
	}
	pthread_mutex_unlock(&mut);
}


/*
 * The function copies data pointed by "val" to physical
 * memory pages using virtual address (va)
 */
void put_value(void *va, void *val, int size)
{
	int i;
	char *phys_addr, *val_ptr = val, *virt_addr = va;

	/*
	 * HINT: Using the virtual address and translate(), find the physical page. Copy
	 * the contents of "val" to a physical page. NOTE: The "size" value can be larger
	 * than one page. Therefore, you may have to find multiple pages using translate()
	 * function
	 */
	if (!va || !val || size <= 0)
		return;

	for (i = 0; i < size; i++, virt_addr++) {
		phys_addr = (char *) translate(page_dir, virt_addr);
		if (!phys_addr) {
			printf("%s: Address translation failed!\n", __func__);
			return;
		}
		*phys_addr = *val_ptr++;
	}
}


/*
 * Given a virtual address, this function copies the contents of the page
 * to val
 */
void get_value(void *va, void *val, int size)
{
	int i;
	char *phys_addr, *val_ptr = val, *virt_addr = va;

	/*
	 * HINT: put the values pointed to by "va" inside the physical memory at given
	 * "val" address. Assume you can access "val" directly by derefencing them
	 */
	if (!va || !val || size <= 0)
		return;

	for (i = 0; i < size; i++, virt_addr++) {
		phys_addr = (char *) translate(page_dir, virt_addr);
		if (!phys_addr)
			return;
		*val_ptr++ = *phys_addr;
	}
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
	int i, k, j, num1, num2, total;
	unsigned int addr_mat1, addr_mat2, addr_ans;

	if (!mat1 || !mat2 || !answer || size <= 0)
		return;

	for (i = 0; i < size; i++) {
		for (j = 0; j < size; j++) {
			total = 0;
			/* answer[i][j] += mat1[i][k] * mat2[k][j] */
			for (k = 0; k < size; k++) {
				addr_mat1 = (unsigned int) mat1 +
						(i * size * sizeof(int)) +
						(k * sizeof(int));

				addr_mat2 = (unsigned int) mat2 +
						(k * size * sizeof(int)) +
						(j * sizeof(int));

				get_value((void *) addr_mat1, &num1, sizeof(int));
				get_value((void *) addr_mat2, &num2, sizeof(int));
				total += num1 * num2;
			}
			addr_ans = (unsigned int) answer +
					(i * size * sizeof(int)) + (j * sizeof(int));
			put_value((void *) addr_ans, &total, sizeof(int));
		}
	}
}
