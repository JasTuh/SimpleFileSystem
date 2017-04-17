#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <fuse.h>

#include "sfs.h"

FILE *flatFile = NULL;
struct SuperBlock *superblock = NULL;
char *bitmap = NULL;

void readBlock(BlockID id, void *buffer) {
	fseek(flatFile, id*superblock->blockSize, SEEK_SET);
	fread(buffer, superblock->blockSize, 1, flatFile);
}

void writeBlock(BlockID id, void *buffer) {
	fseek(flatFile, id*superblock->blockSize, SEEK_SET);
	fwrite(buffer, superblock->blockSize, 1, flatFile);
	fflush(flatFile);
}

void markBlockUsed(BlockID id) {
	bitmap[id/8] |= 1 << (id % 8);
	writeBlock(superblock->bitmapBlock, bitmap);
	superblock->numFreeBlocks--;
	writeBlock(0, superblock);
}

void markBlockFree(BlockID id) {
	bitmap[id/8] &= bitmap[id/8] & ~(1 << (id % 8));
	writeBlock(superblock->bitmapBlock, bitmap);
	superblock->numFreeBlocks++;
	writeBlock(0, superblock);
}

/**
 * This isn't finished or tested
 * 
 * Gets the block ID that contains the specific offset of the file. 
 * If the offset is larger than the file, returns -1
 */
BlockID getBlockFromOffset(INode *node, int offset) {
	if (offset < node->size) {
		return -1;
	}
	
	// lists space contained by each level of indirection
	int sizes[3];
	sizes[0] = 12 * superblock->blockSize;
	sizes[1] = (superblock->blockSize / sizeof(BlockID)) * superblock->blockSize;
	sizes[2] = (superblock->blockSize / sizeof(BlockID)) * (superblock->blockSize / sizeof(BlockID)) * superblock->blockSize;
	
	if (offset < sizes[0]) {
		// inside of direct blocks
		return node->direct[offset / superblock->blockSize];
	} 
	
	offset -= sizes[0];
	
	if (offset < sizes[1]) {
		// inside of the single level indirection block
		// read indirection block
		BlockID *indirect = malloc(superblock->blockSize);
		readBlock(node->singleIndirect, indirect);
		// divide by blockSize to get indirection offset
		offset /= superblock->blockSize;
		BlockID id = indirect[offset];
		free(indirect);
		return id;
	}
	// otherwise, the block is inside the double indirection block
	offset -= sizes[1];
	
	BlockID *indirect = malloc(superblock->blockSize);
	offset /= sizes[1]; 	// divide by the 
	readBlock(node->singleIndirect, indirect);
	offset /= superblock->blockSize;
	return indirect[offset];
}


void *sfs_init(struct fuse_conn_info *conn) {
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
	superblock = calloc(BLOCK_SIZE, 1);
	bitmap = calloc(BLOCK_SIZE, 1);
	readBlock(0, superblock);
	
	if (!validSuperBlock(superblock)) {
		// if superblock is not valid, we need to initialize the disk fully
		superblock->blockSize = BLOCK_SIZE;
		superblock->numBlocks = TOTAL_BLOCKS;
		
		superblock->numINodeBlocks = (superblock->numBlocks - 1)  / ((float) superblock->blockSize / sizeof(INode) + 1);
		superblock->numINodes = superblock->numINodeBlocks * superblock->blockSize / sizeof(INode);
		superblock->numFreeINodes = superblock->numINodes;
		superblock->numFreeBlocks = superblock->numBlocks;
		superblock->firstINodeBlock = 1;
		superblock->firstDataBlock = 1 + superblock->numINodeBlocks;
		superblock->bitmapBlock = superblock->firstDataBlock;
		setValidSuperBlock(superblock);
		writeBlock(0, superblock);
		// mark first 752 blocks as used (superblock + 750 inode blocks + bitmap block)
		for (i=0; i < 2+superblock->numINodeBlocks; i++) {
			markBlockUsed(i);
		}
	} 
	
	readBlock(superblock->bitmapBlock, bitmap);
	
	return (void *) 0;
}

void sfs_destroy(void *userdata) {
	fclose(flatFile);
	flatFile = NULL;
	free(superblock);
	superblock = NULL;
	free(bitmap);
	bitmap = NULL;
}

int main(int argc, char *argv[]) {
	sfs_init((struct fuse_conn_info *) 0);
	sfs_destroy(NULL);
	printf("%ld\n", sizeof(INode));
}
