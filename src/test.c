#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <fuse.h>

#include "sfs.h"

FILE *flatFile = NULL;
struct SuperBlock *superblock;

void readBlock(BlockID id, void *buffer) {
	fseek(flatFile, id*BLOCK_SIZE, SEEK_SET);
	fread(buffer, BLOCK_SIZE, 1, flatFile);
}

void writeBlock(BlockID id, void *buffer) {
	fseek(flatFile, id*BLOCK_SIZE, SEEK_SET);
	fwrite(buffer, BLOCK_SIZE, 1, flatFile);
	fflush(flatFile);
}

void markBlockUsed(BlockID id) {
	char *bitmap = malloc(BLOCK_SIZE);
	readBlock(superblock->bitmapBlock, bitmap);
	bitmap[id/8] |= 1 << (id % 8);
	writeBlock(superblock->bitmapBlock, bitmap);
	free(bitmap);
}

void markBlockFree(BlockID id) {
	char *bitmap = malloc(BLOCK_SIZE);
	readBlock(superblock->bitmapBlock, bitmap);
	bitmap[id/8] &= bitmap[id/8] & ~(1 << (id % 8));
	writeBlock(superblock->bitmapBlock, bitmap);
}


void *sfs_init(struct fuse_conn_info *conn)
{
	int i, nothing = 0;
	flatFile = fopen("flatfile.bin", "r+");
	
	if (flatFile == NULL) {
		// if we can't open for updating, we open for writing to create it
		flatFile = fopen("flatfile.bin", "w");
		// write last byte to set file size
		fseek(flatFile, TOTAL_SIZE - 1, SEEK_SET);
		fwrite((void *) &nothing, 1, 1, flatFile);
		fclose(flatFile);
		// reopen for updating
		flatFile = fopen("flatfile.bin", "r+");
	}
	// read superblock
	superblock = malloc(BLOCK_SIZE);
	readBlock(0, superblock);
	
	if (!validSuperBlock(superblock)) {
		// if superblock is not valid, we need to initialize the disk fully
		superblock->blockSize = BLOCK_SIZE;
		superblock->numBlocks = NUM_DATA_BLOCKS;
		superblock->numINodes = NUM_INODES;
		superblock->numFreeBlocks = NUM_DATA_BLOCKS;
		superblock->numFreeINodes = NUM_INODES;
		superblock->firstINode = 1;
		superblock->firstDataBlock = 1 + NUM_INODE_BLOCKS;
		superblock->bitmapBlock = superblock->firstDataBlock;
		setValidSuperBlock(superblock);
		writeBlock(0, superblock);
		// mark first 752 blocks as used (superblock + 750 inode blocks + bitmap block)
		for (i=0; i < 2+NUM_INODE_BLOCKS; i++) {
			markBlockUsed(i);
		}
	}
	
	fclose(flatFile);
	return (void *) 0;
}

int main(int argc, char *argv[]) {
	sfs_init((struct fuse_conn_info *) 0);
	printf("%ld\n", sizeof(struct SuperBlock));
}
