#include <err.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include "my_vm.h"

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

static unsigned long phys_mem_size;
static void *phys_mem;
static pde_t *page_dir;

static unsigned int off_bits;
static unsigned int page_dir_bits;
static unsigned int page_table_bits;
static unsigned int phys_page_bits;

static unsigned long total_pages;
static unsigned char *alloc_map;
static unsigned char *virt_map;

/* General lock */
static pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;

/* Locks used to ensure coherence in maps and tables */
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

/* Clean up physical memory mapping, and bitmaps when program finishes */
static void free_phys_mem(void)
{
	munmap(phys_mem, phys_mem_size);
	free(alloc_map);
	free(virt_map);
}

/*
 * Function responsible for allocating and setting your physical memory
 * Allocate physical memory using mmap or malloc; this is the total size of
 * your memory you are simulating
 */
static void set_physical_mem(void)
{
	unsigned long map_size;

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
	if (!page_dir_bits) {
		unsigned int bits_remaining = ADDR_BITS - off_bits;
		/*
		 * This only gets triggered on very large page sizes
		 * such as 128k pages.
		 */
		page_table_bits = bits_remaining / 2;
		if (bits_remaining % 2)
			page_table_bits++;
		page_dir_bits = bits_remaining / 2;
	}

	/* How many bits we use to address the physical pages */
	phys_page_bits = page_table_bits + page_dir_bits;
	total_pages = phys_mem_size / PGSIZE;

	map_size = total_pages / 8;
	alloc_map = calloc(1, map_size);
	virt_map = calloc(1, map_size);
	if (!alloc_map || !virt_map)
		err(-1, "%s: Error allocating %lu bytes for bitmap",
				__func__, map_size);

	/* We use page 0 as our directory */
	map_set_bit(alloc_map, 0);
	/* We do not want a virtual address to be 0x000... (or NULL) */
	map_set_bit(virt_map, 0);

	if (atexit(&free_phys_mem) != 0)
		warn("%s: Setup of automatic freeing of memory failed. "
			"Physical memory will not be freed at program exit.",
			__func__);
}


/*
 * Part 2: Add a virtual to physical page translation to the TLB.
 * Feel free to extend the function arguments or return type.
 */
static int add_TLB(void *va, void *pa)
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
 */
static pte_t *check_TLB(void *va)
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

/* Invalidates a virtual address in the TLB. */
static void invalidate_entry_TLB(void *va)
{
	unsigned long i, tag;

	tag = (unsigned long) va >> off_bits;
	i = tag % TLB_ENTRIES;

	if (tlb_store.entries[i].valid && tlb_store.entries[i].virt_addr == tag)
		tlb_store.entries[i].valid = false;
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
static pte_t *translate(pde_t *pgdir, void *va)
{
	pte_t table_entry, *page_table;
	pde_t dir_entry;
	unsigned long dir_index, offset, table_index, table_num, page_num, addr;
	unsigned long virt_addr = (unsigned long) va;

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
	if (!dir_entry)
		return NULL;
	table_num = low_bits(dir_entry, phys_page_bits);

	/* Go to the relevant page table, and retrieve the page table entry. */
	page_table = (pte_t *) ((char *) phys_mem + table_num * PGSIZE);
	table_entry = page_table[table_index];
	if (!table_entry)
		return NULL;
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
static int page_map(pde_t *pgdir, void *va, void *pa)
{
	pte_t table_entry, *page_table;
	pde_t dir_entry;
	unsigned long dir_index, table_index, table_num, page_num, virt_addr;

	if (!pgdir || !va || !pa)
		return -1;

	virt_addr = (unsigned long) va;
	dir_index = top_bits(virt_addr, page_dir_bits);
	table_index = mid_bits(virt_addr, page_table_bits, off_bits);

	dir_entry = pgdir[dir_index];
	if (!dir_entry) {
		/* We are mapping to a new table */
		dir_entry = alloc_new_table();
		map_set_bit(alloc_map, dir_entry);
		pgdir[dir_index] = dir_entry;
	}
	table_num = low_bits(dir_entry, phys_page_bits);

	page_table = (pte_t *) ((char *) phys_mem + table_num * PGSIZE);
	table_entry = page_table[table_index];
	page_num = ((pte_t) pa - (pte_t) phys_mem) / PGSIZE;
	/* Update the table entry if the mapping is invalid */
	if (table_entry != (pte_t) page_num)
		page_table[table_index] = (pte_t) page_num;
	add_TLB(va, (void *) page_num);
	return 0;
}

/* Returns the next avaiable (non-valid) entries from bitmap */
static unsigned int get_next_avail_entry(int num_entries, unsigned char *bmap)
{
	unsigned int i, free_page = 0, available_pages = 0;

	for (i = 0; i < total_pages; i++) {
		if (!map_get_bit(bmap, i)) {
			if (!free_page)
				free_page = i;
			available_pages++;
			if (available_pages == num_entries)
				return free_page;
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
	unsigned int num_pages, i, open_entry, first_page;

	if (!num_bytes)
		return NULL;

	pthread_mutex_lock(&mut);
	if (!initialized) {
		/* Initalize memory if we have not yet. */
		set_physical_mem();
		initialized = 1;
	}
	pthread_mutex_unlock(&mut);

	num_pages = num_bytes / PGSIZE;
	if (num_bytes % PGSIZE)
		num_pages++;

	pthread_mutex_lock(&map_lock);
	/* Find continuous open entries */
	open_entry = get_next_avail_entry(num_pages, virt_map);
	if (!open_entry)
		goto err_unlock_map;

	/* Set all the entries we are going to use to valid */
	for (i = 0; i < num_pages; i++)
		map_set_bit(virt_map, open_entry + i);
	va = create_virt_addr(open_entry++);

	/*
	 * Find the first available page and set corresponding bit in the map
	 * and map it in the table.
	 */
	first_page = get_next_avail_entry(1, alloc_map);
	if (!first_page)
		goto err_unlock_map;
	map_set_bit(alloc_map, first_page);
	page_map(page_dir, va, (char *) phys_mem + first_page * PGSIZE);
	pthread_mutex_unlock(&map_lock);

	/* Map additional pages if num_bytes > PGSIZE */
	for (i = 1; i < num_pages; i++, open_entry++) {
		unsigned int ppn;
		void *virt_addr_entry = create_virt_addr(open_entry);

		/* Set the physical page and virtual page maps, and map the page */
		pthread_mutex_lock(&map_lock);
		ppn = get_next_avail_entry(1, alloc_map);
		map_set_bit(alloc_map, ppn);
		pthread_mutex_unlock(&map_lock);

		pthread_mutex_lock(&table_lock);
		page_map(page_dir, virt_addr_entry, (char *) phys_mem + ppn * PGSIZE);
		pthread_mutex_unlock(&table_lock);
	}

	return va;

err_unlock_map:
	pthread_mutex_unlock(&map_lock);
	return NULL;
}

/* Convert a virtual address to the index in the virtual bitmap */
static unsigned int virt_addr_to_index(void *va)
{
	unsigned long virt_addr = (unsigned long) va;
	unsigned int index;

	index = top_bits(virt_addr, page_dir_bits) << page_table_bits;
	index += mid_bits(virt_addr, page_table_bits, off_bits);
	return index;
}

/*
 * Responsible for releasing one or more memory pages using
 * virtual address (va)
 */
void a_free(void *va, int size)
{
	unsigned long i, num_to_free, virt_map_index, virt_addr;

	if (!va || size <= 0)
		return;

	num_to_free = size / PGSIZE;
	if (size % PGSIZE)
		num_to_free++;

	/* Find which entries the virtual address corresponds to */
	virt_map_index = virt_addr_to_index(va);
	virt_addr = (unsigned long) va;

	/* Ensure that we can free all the pages requested. */
	pthread_mutex_lock(&map_lock);
	for (i = 0; i < num_to_free; i++) {
		if (!map_get_bit(virt_map, virt_map_index + i)) {
			/* We are trying to free an invalid mapping */
			pthread_mutex_unlock(&map_lock);
			return;
		}
	}
	pthread_mutex_unlock(&map_lock);

	/*
	 * At this point, we know that va + size are all valid to free,
	 * thus we can go ahead and start freeing memory.
	 */
	for (i = 0; i < num_to_free; i++) {
		unsigned long phys_addr, ppn;

		/* Get the physical page number */
		phys_addr = (unsigned long) translate(page_dir, (void *) virt_addr);
		ppn = (phys_addr - (unsigned long) phys_mem) / PGSIZE;

		/* Clear the bits of the physical and virtual bitmaps */
		pthread_mutex_lock(&map_lock);
		map_clear_bit(alloc_map, ppn);
		map_clear_bit(virt_map, virt_map_index);
		pthread_mutex_unlock(&map_lock);

		invalidate_entry_TLB((void *) virt_addr);

		/* Move on to the next pages */
		virt_map_index++;
		virt_addr += PGSIZE;
	}

}


/*
 * The function copies data pointed by "val" to physical
 * memory pages using virtual address (va)
 */
void put_value(void *va, void *val, int size)
{
	int i;
	char *phys_addr, *val_ptr = val, *virt_addr = va;

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
 * Given a virtual address, this function copies (size) bytes from the page
 * to val
 */
void get_value(void *va, void *val, int size)
{
	int i;
	char *phys_addr, *val_ptr = val, *virt_addr = va;

	if (!va || !val || size <= 0)
		return;

	for (i = 0; i < size; i++, virt_addr++) {
		phys_addr = (char *) translate(page_dir, virt_addr);
		if (!phys_addr) {
			printf("%s: Address translation failed!\n", __func__);
			return;
		}
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
