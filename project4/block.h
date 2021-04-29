/*
 *  Copyright (C) 2021 CS416 Rutgers CS
 *	Tiny File System
 *	File:	block.h
 *
 */

#ifndef _BLOCK_H_
#define _BLOCK_H_

#define BLOCK_SIZE 4096

extern void dev_init(const char* diskfile_path);
extern int dev_open(const char* diskfile_path);
extern void dev_close(void);
extern int bio_read(const int block_num, void *buf);
extern int bio_write(const int block_num, const void *buf);

#endif
