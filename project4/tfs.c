/*
 *  Copyright (C) 2021 CS416 Rutgers CS
 *	Tiny File System
 *	File:	tfs.c
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>

#include "block.h"
#include "tfs.h"

/* File types */
#define TYPE_REG 0
#define TYPE_DIR 1

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define DIRENTS_IN_BLOCK (BLOCK_SIZE / sizeof(struct dirent))
#define INODES_IN_BLOCK (BLOCK_SIZE / sizeof(struct inode))
#define sizeof_field(type, member) (sizeof(((type *) 0)->member))

char diskfile_path[PATH_MAX];

/* Declare your in-memory data structures here */
static bitmap_t block_map;
static bitmap_t inode_map;

/*
 * Important information for the OS to identify where things are located.
 * Using OSTEP implementation:
 * 	- The super block will be contained within the first block, index 0.
 * 	- Inode bitmap will be second block, index 1.
 * 	- Data block bitmap will be index 2.
 * 	- The inode table will be index 3.
 * 	- Our data blocks will be the index following the last inode region,
 * 		which is determined by the amount of inodes we have.
 */
static struct superblock __superblock_defaults = {
	.magic_num = MAGIC_NUM,
	.max_inum = MAX_INUM,
	.max_dnum = MAX_DNUM,
	.i_bitmap_blk = 1,
	.d_bitmap_blk = 2,
	.i_start_blk = 3,
	.d_start_blk = 3 + (sizeof(struct inode) * MAX_INUM / BLOCK_SIZE),
};

/* In-memory superblock structure so we can read and write to heap */
static struct superblock *superblock;

/*
 * tfs_log - print log message, usually for errors
 * fmt: msg to print
 *
 * We print to stdout because using the debug option (-d) for tfs only shows
 * output to stdout
 */
static void tfs_log(const char *fmt, ...)
{
	va_list argp;
	va_start(argp, fmt);
	fprintf(stdout, "## [LOG] ");
	vfprintf(stdout, fmt, argp);
	va_end(argp);
	fputc('\n', stdout);
}

/*
 * Get available inode number from bitmap
 */
int get_avail_ino(void)
{
	int inode_num;

	// Step 1: Read inode bitmap from disk
	if (!bio_read(superblock->i_bitmap_blk, inode_map)) {
		tfs_log("%s: Error reading inode map from disk", __func__);
		return -1;
	}

	// Step 2: Traverse inode bitmap to find an available slot
	for (inode_num = 0; inode_num < MAX_INUM; inode_num++) {
		if (!get_bitmap(inode_map, inode_num)) {
			// Step 3: Update inode bitmap and write to disk
			set_bitmap(inode_map, inode_num);
			bio_write(superblock->i_bitmap_blk, inode_map);
			return inode_num;;
		}
	}
	return -1;
}

/*
* Get available data block number from bitmap
*/
int get_avail_blkno(void)
{
	int block_num;

	// Step 1: Read data block bitmap from disk
	if (!bio_read(superblock->d_bitmap_blk, block_map)) {
		tfs_log("%s: Error reading block map from disk\n", __func__);
		return -1;
	}

	// Step 2: Traverse data block bitmap to find an available slot
	for (block_num = 0; block_num < MAX_DNUM; block_num++) {
		if (!get_bitmap(block_map, block_num)) {
			// Step 3: Update data block bitmap and write to disk
			set_bitmap(block_map, block_num);
			bio_write(superblock->d_bitmap_blk, block_map);
			return block_num + superblock->d_start_blk;
		}
	}
	return -1;
}

/* Convert inode number to containing block number */
static inline int inumber_to_blk(uint16_t ino)
{
	return (ino * sizeof(struct inode)) / BLOCK_SIZE;
}

/* inode operations */

/*
 * readi - reads in inode from disk
 * ino: the inode's index
 * inode: ptr to where to store the contents of the inode
 */
int readi(uint16_t ino, struct inode *inode)
{
	struct inode *block;
	int inode_block_index, offset;

	block = malloc(BLOCK_SIZE);
	if (!block)
		return -ENOMEM;

	// Step 1: Get the inode's on-disk block number
	inode_block_index = superblock->i_start_blk + inumber_to_blk(ino);

	// Step 2: Get offset of the inode in the inode on-disk block
	offset = ino % INODES_IN_BLOCK;

	// Step 3: Read the block from disk and then copy into inode structure
	bio_read(inode_block_index, block);
	*inode = block[offset];
	free(block);
	return 0;
}

/*
 * writei - write inode to disk
 * ino:	the inodes index
 * inode: ptr to the inodes contents
 */
int writei(uint16_t ino, struct inode *inode)
{
	struct inode *block;
	int inode_block_index, offset;

	block = malloc(BLOCK_SIZE);
	if (!block)
		return -ENOMEM;

	// Step 1: Get the block number where this inode resides on disk
	inode_block_index = superblock->i_start_blk + inumber_to_blk(ino);

	// Step 2: Get the offset in the block where this inode resides on disk
	offset = ino % INODES_IN_BLOCK;

	/*
	 * Step 3: Write inode to disk
	 * Read in the relevant block that contains our inode from disk.
	 * Once the block has been read from disk, factor in offset and update
	 * the location in which the inode sits.
	 * Write the inode block back to disk.
	 */
	bio_read(inode_block_index, block);
	block[offset] = *inode;
	bio_write(inode_block_index, block);
	free(block);
	return 0;
}


/* directory operations */
/*
 * dir_find - searches a directory for a filename
 * ino: the inode of the directory to scan
 * fname: the filename to look for
 * name_len: the length of the filename
 * dirent: ptr to memory that is updated with the contents of the file
 * 		if it is found.
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent)
{
	struct inode dir_node;
	struct dirent *entries;
	int block_ptr, err = -ENOENT;

	/*
	 * Step 1: Call readi() to get the inode using ino
	 * (inode number of current directory)
	 */
	if (readi(ino, &dir_node) < 0) {
		tfs_log("%s: Error finding inode for ino (%d)", __func__, ino);
		return err;
	}

	entries = malloc(BLOCK_SIZE);
	if (!entries)
		return -ENOMEM;

	for (block_ptr = 0; block_ptr < ARRAY_SIZE(dir_node.direct_ptr); block_ptr++) {
		struct dirent *entry_parser = entries;
		int entry_index;

		if (!dir_node.direct_ptr[block_ptr])
			break;

		// Step 2: Get data block of current directory from inode
		if (!bio_read(dir_node.direct_ptr[block_ptr], entries))
			break;

		for (entry_index = 0; entry_index < DIRENTS_IN_BLOCK;
			       			entry_index++, entry_parser++) {
			/*
			 * Step 3: Read directory's data block and check each
			 * directory entry. If the name matches,
			 * then copy directory entry to dirent structure.
			 */
			if (entry_parser->valid && !strcmp(entry_parser->name, fname)) {
				*dirent = *entry_parser;
				err = 0;
				goto out;
			}
		}
	}
out:
	free(entries);
	return err;
}

/*
 * dir_add - adds a file to a directory
 * dir_inode: the inode of the directory
 * f_ino: the inode of the file that we create a link to
 * fname: the name of the file
 * name_len: length of fname
 */
int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len)
{
	struct dirent de, *entries;
	int block_ptr;

	/* Check to see if the file we are looking to add already exists */
	if (!dir_find(dir_inode.ino, fname, name_len, &de)) {
		tfs_log("%s: file '%s' already exists.", __func__, fname);
		return -EEXIST;
	}

	entries = malloc(BLOCK_SIZE);
	if (!entries)
		return -ENOMEM;

	for (block_ptr = 0; block_ptr < ARRAY_SIZE(dir_inode.direct_ptr); block_ptr++) {
		struct dirent *entry_parser = entries;
		int entry_index;

		/*
		 * Allocate a new data block for this directory
		 * if it does not exist
		 */
		if (!dir_inode.direct_ptr[block_ptr]) {
			dir_inode.direct_ptr[block_ptr] = get_avail_blkno();
			dir_inode.vstat.st_blocks++;
		}

		/*
		 * Step 1: Read dir_inode's data block and check each
		 * directory entry of dir_inode
		 */
		if (!bio_read(dir_inode.direct_ptr[block_ptr], entries))
			break;

		for (entry_index = 0; entry_index < DIRENTS_IN_BLOCK;
						entry_index++, entry_parser++) {
			/*
			 * If we found a non valid entry, we can use this space
			 * to write the link to our new file.
			 * Step 3: Add directory entry in dir_inode's data block
			 * and write to disk
			 */
			if (!entry_parser->valid) {
				entry_parser->valid = 1;
				entry_parser->ino = f_ino;
				strcpy(entry_parser->name, fname);
				dir_inode.size += sizeof(struct dirent);
				dir_inode.vstat.st_size += sizeof(struct dirent);
				time(&dir_inode.vstat.st_mtime);
				/* Update directory inode */
				writei(dir_inode.ino, &dir_inode);
				/* Write directory entry */
				bio_write(dir_inode.direct_ptr[block_ptr], entries);
				free(entries);
				return 0;
			}
		}

	}
	free(entries);
	return -ENOSPC;
}

/*
 * dir_remove - removes a file from a directory
 * dir_inode: the inode of the directory
 * fname: the name of the file to remove
 * name_len: the length of fname
 */
int dir_remove(struct inode dir_inode, const char *fname, size_t name_len)
{
	struct dirent *entries;
	int block_ptr;

	entries = malloc(BLOCK_SIZE);
	if (!entries)
		return -ENOMEM;

	for (block_ptr = 0; block_ptr < ARRAY_SIZE(dir_inode.direct_ptr); block_ptr++) {
		struct dirent *entry_parser = entries;
		int entry_index;

		if (!dir_inode.direct_ptr[block_ptr])
			break;
		/*
		 * Step 1: Read dir_inode's data block and checks each
		 * directory entry of dir_inode
		 */
		if (!bio_read(dir_inode.direct_ptr[block_ptr], entries))
			break;

		for (entry_index = 0; entry_index < DIRENTS_IN_BLOCK;
			       			entry_index++, entry_parser++) {
			/* Step 2: Check if fname exist */
			if (entry_parser->valid && !strcmp(entry_parser->name, fname)) {
				/*
				 * Step 3: If exist, then remove it from
				 * dir_inode's data block and write to disk
				 */
				entry_parser->valid = 0;
				dir_inode.size -= sizeof(*entry_parser);
				dir_inode.vstat.st_size -= sizeof(*entry_parser);
				time(&dir_inode.vstat.st_mtime);
				writei(dir_inode.ino, &dir_inode);
				bio_write(dir_inode.direct_ptr[block_ptr], entries);
				free(entries);
				return 0;
			}
		}
	}
	free(entries);
	return -ENOENT;
}

/*
 * namei operation.
 *
 * get_node_by_path - retrieves the inode of a file
 * path: the file to look for through a given path
 * ino: the inode of the starting path
 * 		- We usually just pass 0 so we can walk from "root"
 * inode: struct to update if we successfully resovled a path lookup.
 *
 * Resolve the path name, walk through the path, and finally, find its inode.
 * Implemented using iterative approach.
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode)
{
	struct dirent de = {0};
	char *path_dup, *path_walker, *to_free;

	if (!strcmp(path, "/"))
		return readi(0, inode);

	path_dup = strdup(path);
	if (!path_dup)
		return -ENOMEM;
	to_free = path_dup;

	while ((path_walker = strsep(&path_dup, "/")) != NULL) {
		/*
		 * The logic in this if statement is only explainable
		 * by the things that strsep does. It is possible that
		 * if our path ends in "/" then it returns "\0" and we should
		 * never send a blank string to dir find.
		 */
		if (*path_walker &&
		    dir_find(de.ino, path_walker, strlen(path_walker), &de) < 0) {
			free(to_free);
			return -ENOENT;
		}
	}
	free(to_free);
	return readi(de.ino, inode);
}


/*
 * Initializes the first entries of an dirent entry block.
 * Creates "." and ".." files and links them to their respective inodes.
 */
static void init_dir(struct dirent *entry_block, int self_ino, int parent_ino)
{
	entry_block->ino = self_ino;
	entry_block->valid = 1;
	strcpy(entry_block->name, ".");
	entry_block++;
	entry_block->ino = parent_ino;
	entry_block->valid = 1;
	strcpy(entry_block->name, "..");
}

/*
 * initalizes an inode by assigning it an open_inode, grabbing an
 * open data block, and assigning it a type.
 */
static void init_inode(struct inode *new, int open_inode, int type)
{
	int i;

	new->ino = open_inode;
	new->valid = 1;
	new->link = type == TYPE_REG ? 1 : 2;
	new->direct_ptr[0] = get_avail_blkno();
	for (i = 1; i < ARRAY_SIZE(new->direct_ptr); i++)
		new->direct_ptr[i] = 0;
	for (i = 0; i < ARRAY_SIZE(new->indirect_ptr); i++)
		new->indirect_ptr[i] = 0;
	new->type = type;
}


/*
 * Make file system
 * tfs_mkfs - Intializes the disk
 *
 * 1. Loads in superblock default values
 * 2. Sets up inode & data block bitmaps
 * 3. Creates the root directory and it's first entries ("." and "..")
 * 4. Writes everything to disk
 */
int tfs_mkfs(void)
{
	struct inode *iroot;
	struct stat *stat_root;
	struct dirent *dir_root;
	int i;

	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path);

	superblock = malloc(BLOCK_SIZE);
	if (!superblock) {
		tfs_log("%s: Error initializing superblock", __func__);
		return -ENOMEM;
	}
	*superblock = __superblock_defaults;

	/* write superblock information to the first block of the disk */
	bio_write(0, superblock);

	/* Inode and data block bitmaps both take up one block. */
	inode_map = calloc(1, BLOCK_SIZE);
	block_map = calloc(1, BLOCK_SIZE);
	if (!inode_map || !block_map) {
		tfs_log("%s: Error initializing superblock", __func__);
		return -ENOMEM;
	}

	// update bitmap information for root directory
	set_bitmap(inode_map, 0);
	set_bitmap(block_map, 0);
	bio_write(superblock->i_bitmap_blk, inode_map);
	bio_write(superblock->d_bitmap_blk, block_map);

	/* Create first instances of inode entries and dir entries */
	iroot = malloc(BLOCK_SIZE);
	dir_root = malloc(BLOCK_SIZE);
	if (!iroot || !dir_root) {
		tfs_log("%s: Error initializing root inode and root entries",
								__func__);
		return -ENOMEM;
	}

	/*
	 * Update inode for root directory
	 * We cannot use init_inode() here because we have already
	 * set the block bitmap above. We do not want to grab ANOTHER
	 * free block. So we do the job of init_inode() but manually.
	 */
	iroot->ino = 0;
	iroot->valid = 1;
	iroot->size = sizeof(struct dirent) * 2;;
	iroot->type = TYPE_DIR;
	iroot->link = 2;
	iroot->direct_ptr[0] = superblock->d_start_blk;
	for (i = 1; i < ARRAY_SIZE(iroot->direct_ptr); i++)
		iroot->direct_ptr[i] = 0;
	for (i = 0; i < ARRAY_SIZE(iroot->indirect_ptr); i++)
		iroot->indirect_ptr[i] = 0;
	stat_root = &iroot->vstat;
	stat_root->st_mode = S_IFDIR | 0755;
	stat_root->st_nlink = 2;
	stat_root->st_blocks = 1;
	stat_root->st_blksize = BLOCK_SIZE;

	/* Setup root directory */
	init_dir(dir_root, 0, 0);

	/* Write everything to disk */
	bio_write(superblock->i_start_blk, iroot);
	bio_write(superblock->d_start_blk, dir_root);
	free(iroot);
	free(dir_root);
	return 0;
}


/*
 * FUSE file operations
 */
static void *tfs_init(struct fuse_conn_info *conn)
{
	// Step 1a: If disk file is not found, call mkfs
	if (dev_open(diskfile_path) < 0) {
		tfs_mkfs();
		return NULL;
	}

	// Step 1b: If disk file is found, just initialize in-memory data structures
	// and read superblock from disk
	superblock = malloc(BLOCK_SIZE);
	inode_map = malloc(BLOCK_SIZE);
	block_map = malloc(BLOCK_SIZE);
	if (!superblock || !inode_map || !block_map)
		tfs_log("%s: Error initializing in-memory structures.", __func__);

	if (!bio_read(0, superblock) ||
	    !bio_read(superblock->i_bitmap_blk, inode_map) ||
	    !bio_read(superblock->d_bitmap_blk, block_map))
		tfs_log("%s: Error reading block structures from disk.", __func__);
	return NULL;
}

/*
 * tfs_destroy - unwind & deallocate data structures and close disk
 * userdata: unused
 */
static void tfs_destroy(void *userdata)
{
	// Step 1: De-allocate in-memory data structures
	if (superblock)
		free(superblock);
	if (inode_map)
		free(inode_map);
	if (block_map)
		free(block_map);

	// Step 2: Close diskfile
	dev_close();
}

/*
 * tfs_getattr - fuse getattr
 * path: path of file to retrieve attributes
 */
static int tfs_getattr(const char *path, struct stat *stbuf)
{
	struct inode node;

	// Step 1: call get_node_by_path() to get inode from path
	if (get_node_by_path(path, 0, &node))
		return -ENOENT;

	// Step 2: fill attribute of file into stbuf from inode
	*stbuf = node.vstat;
	return 0;
}

/*
 * tfs_opendir - fuse opendir
 * path: path of the directory to open
 */
static int tfs_opendir(const char *path, struct fuse_file_info *fi)
{
	// Step 1: Call get_node_by_path() to get inode from path
	struct inode dir_node;

	// Step 2: If not find, return -1
	return get_node_by_path(path, 0, &dir_node) ? -1 : 0;
}

/*
 * tfs_readdir - fuse readdir
 * path: path of the directory to read
 * buffer: buffer to load our directory entries into
 * filler: fuse function that loads our entries into the buffer
 * offset: unused, stays at 0 so filler() knows to manage the offsets
 * 		into the directory structure itself.
 */
static int tfs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
			off_t offset, struct fuse_file_info *fi)
{
	struct inode dir_node;
	struct dirent *entries;
	int i;

	// Step 1: Call get_node_by_path() to get inode from path
	if (get_node_by_path(path, 0, &dir_node) < 0)
		return -ENOENT;

	entries = malloc(BLOCK_SIZE);
	if (!entries)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(dir_node.direct_ptr); i++) {
		struct dirent *entry_parser = entries;
		int entry_index;

		if (!dir_node.direct_ptr[i])
			break;
		if (!bio_read(dir_node.direct_ptr[i], entries))
			break;

		/*
		 * Step 2: Read directory entries from its data blocks,
		 * and copy them to filler
		 */
		for (entry_index = 0; entry_index < DIRENTS_IN_BLOCK;
						entry_index++, entry_parser++) {
			if (entry_parser->valid) {
				struct inode to_read;
				readi(entry_parser->ino, &to_read);
				filler(buffer, entry_parser->name, &to_read.vstat, 0);
			}
		}
	}
	free(entries);
	return 0;
}

/*
 * tfs_mkdir - fuse mkdir
 * path: path of the directory to make
 * mode: mode/permissions of the directory
 */
static int tfs_mkdir(const char *path, mode_t mode)
{
	struct inode pdir_node, new_dir_node;
	struct dirent *new_dir;
	struct stat *new_dir_stat = &new_dir_node.vstat;
	time_t create_time;
	char *target, *parent, *dirc, *basec;
	int open_inode, err;

	dirc = strdup(path);
	basec = strdup(path);
	new_dir = calloc(1, BLOCK_SIZE);
	if (!basec || !dirc || !new_dir)
		return -ENOMEM;

	/*
	 * Step 1: Use dirname() and basename() to separate parent directory
	 * path and target directory name
	 */
	target = basename(basec);
	parent = dirname(dirc);

	/* Ensure that the target name can fit in an entry */
	if (strlen(target) >= sizeof_field(struct dirent, name)) {
		err = -ENAMETOOLONG;
		goto out;
	}

	// Step 2: Call get_node_by_path() to get inode of parent directory
	err = get_node_by_path(parent, 0, &pdir_node);
	if (err)
		goto out;

	// Step 3: Call get_avail_ino() to get an available inode number
	open_inode = get_avail_ino();
	if (open_inode < 0) {
		err = -ENOSPC;
		goto out;
	}

	/*
	 * Step 4: Call dir_add() to add directory entry of target
	 * directory to parent directory
	 */
	err = dir_add(pdir_node, open_inode, target, strlen(target));
	if (err)
		goto out;

	// Step 5: Update inode for target directory
	init_inode(&new_dir_node, open_inode, TYPE_DIR);
	new_dir_node.size = sizeof(struct dirent) * 2;
	new_dir_stat->st_mode = S_IFDIR | mode;
	new_dir_stat->st_nlink = 2;
	new_dir_stat->st_ino = open_inode;
	new_dir_stat->st_blocks = 1;
	new_dir_stat->st_blksize = BLOCK_SIZE;
	new_dir_stat->st_size = new_dir_node.size;
	time(&create_time);
	new_dir_stat->st_atime = create_time;
	new_dir_stat->st_mtime = create_time;
	new_dir_stat->st_uid = getuid();
	new_dir_stat->st_gid = getgid();

	/* Set up initial directory entries */
	init_dir(new_dir, open_inode, pdir_node.ino);

	// Step 6: Call writei() to write inode to disk
	writei(open_inode, &new_dir_node);

	/* Write the inital entries to disk */
	bio_write(new_dir_node.direct_ptr[0], new_dir);
out:
	free(dirc);
	free(basec);
	free(new_dir);
	return err;
}

/*
 * release_data_blocks - releases data blocks inode points to
 * target: target inode to release data blocks
 */
static void release_data_blocks(struct inode *target)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(target->direct_ptr) &&
						target->direct_ptr[i]; i++) {
		unset_bitmap(block_map, target->direct_ptr[i] -
							superblock->d_start_blk);
		target->direct_ptr[i] = 0;

	}
	if (target->type == TYPE_REG) {
		int *ptr_buffer;

		/*
		 * If the inode is a file, we need to check for and release
		 * indirect pointers.
		 */
		ptr_buffer = malloc(BLOCK_SIZE);
		for (i = 0; i < ARRAY_SIZE(target->indirect_ptr) &&
						target->indirect_ptr[i]; i++) {
			int j;
			bio_read(target->indirect_ptr[i], ptr_buffer);
			for (j = 0; j < (BLOCK_SIZE / sizeof(int)); j++) {
				if (!ptr_buffer[j])
					break;
				unset_bitmap(block_map, ptr_buffer[j] -
							superblock->d_start_blk);
			}
			unset_bitmap(block_map, target->indirect_ptr[i] -
						superblock->d_start_blk);

		}
		free(ptr_buffer);
	}
	bio_write(superblock->d_bitmap_blk, block_map);
}

/*
 * tfs_rmdir - fuse rmdir
 * path: path to directory to remove
 */
static int tfs_rmdir(const char *path)
{
	struct inode target_inode, parent_inode;
	char *target, *parent, *dirc, *basec;
	int err;

	dirc = strdup(path);
	basec = strdup(path);
	if (!dirc || !basec)
		return -ENOMEM;
	/*
	 * Step 1: Use dirname() and basename() to separate parent directory
	 * path and target directory name.
	 */
	target = basename(basec);
	parent = dirname(dirc);

	// Step 2: Call get_node_by_path() to get inode of target directory
	err = get_node_by_path(path, 0, &target_inode);
	if (err)
		goto out;

	/* Ensure that the directory is empty before we try to delete it */
	if (target_inode.size != sizeof(struct dirent) * 2) {
		err = -ENOTEMPTY;
		goto out;
	}

	target_inode.valid = 0;

	// Step 3: Clear data block bitmap of target directory
	release_data_blocks(&target_inode);

	// Step 4: Clear inode bitmap and its data block
	unset_bitmap(inode_map, target_inode.ino);
	bio_write(superblock->i_bitmap_blk, inode_map);
	writei(target_inode.ino, &target_inode);

	// Step 5: Call get_node_by_path() to get inode of parent directory
	err = get_node_by_path(parent, 0, &parent_inode);
	if (err)
		goto out;

	/*
	 * Step 6: Call dir_remove() to remove directory entry of target
	 * directory in its parent directory
	 */
	err = dir_remove(parent_inode, target, strlen(target));
	if (err)
		tfs_log("%s: failed remove %s from %s.",
					__func__, target, parent);
out:
	free(dirc);
	free(basec);
	return err;
}

/*
 * tfs_create - fuse create
 * path: path of the file to create
 * mode: mode/permissions of file
 */
static int tfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	struct inode p, target_inode;
	struct stat *target_stat = &target_inode.vstat;
	char *directory, *target, *dirc, *basec;
	time_t create_time;
	int open_inode, err;

	if (!path || !mode)
		return -1;

	basec = strdup(path);
	dirc = strdup(path);
	if (!basec || !dirc)
		return -ENOMEM;
	/*
	 * Step 1: Use dirname() and basename() to separate parent directory
	 * path and target file name
	 */
	directory = dirname(dirc);
	target = basename(basec);

	/* Ensure that the target name can fit in an entry */
	if (strlen(target) >= sizeof_field(struct dirent, name)) {
		err = -ENAMETOOLONG;
		goto out;
	}

	// Step 2: Call get_node_by_path() to get inode of parent directory
	err = get_node_by_path(directory, 0, &p);
	if (err)
		goto out;

	// Step 3: Call get_avail_ino() to get an available inode number
	open_inode = get_avail_ino();

	/*
	 * Step 4: Call dir_add() to add directory entry of target file
	 * to parent directory
	 */
	err = dir_add(p, open_inode, target, strlen(target));
	if (err) {
		tfs_log("%s: Failed to add %s to %s",
				__func__, target, directory);
		goto out;
	}

	// Step 5: Update inode for target file
	init_inode(&target_inode, open_inode, TYPE_REG);
	target_inode.size = 0;
	target_stat->st_mode = S_IFREG | mode;
	target_stat->st_nlink = 1;
	target_stat->st_ino = open_inode;
	target_stat->st_size = 0;
	target_stat->st_blocks = 1;
	time(&create_time);
	target_stat->st_atime = create_time;
	target_stat->st_mtime = create_time;
	target_stat->st_gid = getgid();
	target_stat->st_uid = getuid();

	// Step 6: Call writei() to write inode to disk
	writei(open_inode, &target_inode);
out:
	free(dirc);
	free(basec);
	return err;
}

/*
 * tfs_open - fuse open
 * path: path to the file to open
 */
static int tfs_open(const char *path, struct fuse_file_info *fi)
{
	struct inode file_node;

	// Step 1: Call get_node_by_path() to get inode from path
	// Step 2: If not find, return -1
	return get_node_by_path(path, 0, &file_node) ? -1 : 0;
}

/*
 * ptr_to_indirect - converts block pointer to indirect pointers
 * block_ptr: pointer index to become index of indirect pointer
 * iptr_index: index of where the direct pointer lies in the data block
 */
static void ptr_to_indirect(int *block_ptr, int *iptr_index, struct inode *i)
{
	int ptr = *block_ptr, index = *iptr_index;

	/*
	 * ptr will keep track of which index to use of the indirect
	 * pointers.
	 * index will keep track of the index of the ptr WITHIN the
	 * data block that the indirect pointers point to.
	 */
	ptr -= ARRAY_SIZE(i->direct_ptr);
	index = ptr % (BLOCK_SIZE / sizeof(int));
	ptr /= BLOCK_SIZE / sizeof(int);
	*block_ptr = ptr;
	*iptr_index = index;
}

/*
 * __tfs_read_get_block - internal function for tfs_read to get next data block
 * read_block_ptr: the index of block that we should retrieve
 * inode: inode of the file we are reading from
 */
static int __tfs_read_get_block(int read_block_ptr, struct inode *inode)
{
	int block_ptr;

	if (read_block_ptr >= ARRAY_SIZE(inode->direct_ptr)) {
		int iptr_index, *ptr_buffer;

		ptr_to_indirect(&read_block_ptr, &iptr_index, inode);
		ptr_buffer = malloc(BLOCK_SIZE);
		bio_read(inode->indirect_ptr[read_block_ptr], ptr_buffer);
		block_ptr = ptr_buffer[iptr_index];
		free(ptr_buffer);
	} else {
		block_ptr = inode->direct_ptr[read_block_ptr];
	}
	return block_ptr;
}

/*
 * tfs_read - fuse read
 * path: path to the file to unlock
 * buffer: the buffer to load the read bytes into
 * size: number of bytes to read
 * offset: index of where to start reading from
 */
static int tfs_read(const char *path, char *buffer, size_t size,
					off_t offset, struct fuse_file_info *fi)
{
	struct inode file_node;
	char *block_buffer;
	int bytes_read, bytes_to_end;

	// Step 1: You could call get_node_by_path() to get inode from path
	if (get_node_by_path(path, 0, &file_node) < 0)
		return -ENOENT;

	block_buffer = malloc(BLOCK_SIZE);
	if (!block_buffer)
		return -ENOMEM;

	/* bytes_to_end = file_node.vstat.st_blocks * BLOCK_SIZE - offset; */
	bytes_to_end = file_node.vstat.st_size - offset;

	// Step 2: Based on size and offset, read its data blocks from disk
	for (bytes_read = 0; bytes_read < size && bytes_read < bytes_to_end;) {
		int read_block, counter = 0, rblock_ptr = offset / BLOCK_SIZE;
		char *reader = block_buffer;

		read_block = __tfs_read_get_block(rblock_ptr, &file_node);

		if (!bio_read(read_block, block_buffer))
			break;

		/*
		 * Step 3: copy the correct amount of data from offset to buffer
		 * This loop actually reads from the file.
		 * We need to keep track that we are:
		 * 	1. Not reading more than requested
		 * 	2. Not reading past the end of the file
		 * 	3. Not reading more than BLOCK_SIZE (past the buffer)
		 */
		while (bytes_read < size && bytes_read < bytes_to_end &&
							counter < BLOCK_SIZE) {
			*buffer++ = *reader++;
			counter++;
			bytes_read++;
			offset++;
		}
	}
	free(block_buffer);
	return bytes_read;
}

/*
 * __tfs_write_get_block - internal function to tfs_write to get next data block
 * write_block_ptr: number of the next data block to get
 * inode: inode of the file we are writing to
 */
static int __tfs_write_get_block(int write_block_ptr, struct inode *inode)
{
	int block_ptr;

	if (write_block_ptr >= ARRAY_SIZE(inode->direct_ptr)) {
		int iptr_index, *iptr, *ptr_buffer;

		/*
		 * The block we will write to is only accessible through
		 * indirect pointers
		 */
		ptr_to_indirect(&write_block_ptr, &iptr_index, inode);
		ptr_buffer = malloc(BLOCK_SIZE);
		if (!ptr_buffer)
			return -ENOMEM;

		if (!inode->indirect_ptr[write_block_ptr]) {
			/*
			 * If we need to allocate a new data block of pointers
			 * it should be zero'd because these are _new_ pointers.
			 */
			inode->indirect_ptr[write_block_ptr] = get_avail_blkno();
			memset(ptr_buffer, 0, BLOCK_SIZE);
		} else {
			bio_read(inode->indirect_ptr[write_block_ptr], ptr_buffer);
		}

		iptr = &ptr_buffer[iptr_index];
		if (!*iptr) {
			*iptr = get_avail_blkno();
			inode->vstat.st_blocks++;
			bio_write(inode->indirect_ptr[write_block_ptr], ptr_buffer);
		}
		block_ptr = *iptr;
		free(ptr_buffer);
	} else {
		/* The block we will write to is in our direct pointers */
		if (!inode->direct_ptr[write_block_ptr]) {
			inode->direct_ptr[write_block_ptr] = get_avail_blkno();
			inode->vstat.st_blocks++;
		}
		block_ptr = inode->direct_ptr[write_block_ptr];
	}
	return block_ptr;
}

/*
 * tfs_write -	fuse write
 * path:	the path of the file to write to
 * buffer:	the contents we use to write to the file
 * size:	how many bytes to write
 * offset:	byte offset from beginning of file
 * fi:		unused
 */
static int tfs_write(const char *path, const char *buffer, size_t size,
				off_t offset, struct fuse_file_info *fi)
{
	struct inode file_inode;
	char *write_buffer;
	int i, max, bytes_written = 0;

	// Step 1: You could call get_node_by_path() to get inode from path
	if (get_node_by_path(path, 0, &file_inode) < 0)
		return -ENOENT;

	write_buffer = malloc(BLOCK_SIZE);
	if (!write_buffer)
		return -ENOMEM;

	max = offset + size;
	// Step 2: Based on size and offset, read its data blocks from disk
	for (i = offset; i < max; i++) {
		int write_block_ptr, write_block, counter;
		char *writer = write_buffer;

		/*
		 * The offset / the size of our blocks lets us know which
		 * block ptr we need to access to start our writes.
		 */
		write_block_ptr = i / BLOCK_SIZE;
		write_block = __tfs_write_get_block(write_block_ptr, &file_inode);
		bio_read(write_block, write_buffer);

		/*
		 * Step 3: Write the correct amount of data from offset to disk
		 * This loop actually writes the data.
		 * We need to keep track that we are:
		 * 	1. Not writing more than the user asked.
		 * 	2. Not writing more than BLOCK_SIZE.
		 * 		a. If this condition is hit, we cycle back and
		 * 			find the next block to write to.
		 */
		for (counter = 0; i < max && counter < BLOCK_SIZE; i++, counter++) {
			*writer++ = *buffer++;
			bytes_written++;
		}
		bio_write(write_block, write_buffer);
	}
	free(write_buffer);

	/* If we allocated more space for the file, we need to update the size */
	if (i > file_inode.size) {
		file_inode.size = i - 1;
		file_inode.vstat.st_size = i - 1;
	}

	/* Update modified time if we wrote anything */
	if (bytes_written)
		time(&file_inode.vstat.st_mtime);

	// Step 4: Update the inode info and write it to disk
	writei(file_inode.ino, &file_inode);
	return bytes_written;
}

/*
 * tfs_unlink - fuse unlink
 * path: the path to the file to unlink
 */
static int tfs_unlink(const char *path)
{
	struct inode target_node, target_pdir;
	char *parent_dir, *target, *dirc, *basec;
	int err;

	/*
	 * Step 1: Use dirname() and basename() to separate parent directory
	 * path and target file name
	 */
	basec = strdup(path);
	dirc = strdup(path);
	if (!basec || !dirc)
		return -ENOMEM;

	parent_dir = dirname(dirc);
	target = basename(basec);

	// Step 2: Call get_node_by_path() to get inode of target file
	err = get_node_by_path(path, 0, &target_node);
	if (err)
		goto out;

	// Step 3: Clear data block bitmap of target file
	release_data_blocks(&target_node);

	// Step 4: Clear inode bitmap and its data block
	target_node.valid = 0;
	unset_bitmap(inode_map, target_node.ino);
	bio_write(superblock->i_bitmap_blk, inode_map);
	writei(target_node.ino, &target_node);

	// Step 5: Call get_node_by_path() to get inode of parent directory
	err = get_node_by_path(parent_dir, 0, &target_pdir);
	if (err)
		goto out;

	/*
	 * Step 6: Call dir_remove() to remove directory entry of target
	 * file in its parent directory
	 */
	err = dir_remove(target_pdir, target, strlen(target));
	if (err)
		tfs_log("%s: Error removing %s from %s",
				__func__, target, parent_dir);
out:
	free(basec);
	free(dirc);
	return err;
}

/*
 * All unimplemented functions are put here to make everything that is filled
 * in look less congested.
 */
static int tfs_truncate(const char *path, off_t size)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int tfs_release(const char *path, struct fuse_file_info *fi)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int tfs_flush(const char * path, struct fuse_file_info * fi)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int tfs_utimens(const char *path, const struct timespec tv[2])
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int tfs_releasedir(const char *path, struct fuse_file_info *fi)
{
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static struct fuse_operations tfs_ope = {
	.init		= tfs_init,
	.destroy	= tfs_destroy,

	.getattr	= tfs_getattr,
	.readdir	= tfs_readdir,
	.opendir	= tfs_opendir,
	.releasedir	= tfs_releasedir,
	.mkdir		= tfs_mkdir,
	.rmdir		= tfs_rmdir,

	.create		= tfs_create,
	.open		= tfs_open,
	.read 		= tfs_read,
	.write		= tfs_write,
	.unlink		= tfs_unlink,

	.truncate	= tfs_truncate,
	.flush		= tfs_flush,
	.utimens	= tfs_utimens,
	.release	= tfs_release
};

int main(int argc, char *argv[])
{
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &tfs_ope, NULL);

	return fuse_stat;
}
