#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fuse.h>

#include "sfs.h"

FILE *flatFile = NULL;
struct SuperBlock *superblock = NULL;
char *bitmap = NULL;
INode *curNode = NULL;

/***********************************************************************
 * 
 * Low level IO functions
 * 
 ***********************************************************************/

/**
 * Reads the block specified by id into the buffer. Buffer must be at least
 * superblock->blockSize in length.
 */
void readBlock(BlockID id, void *buffer) {
	fseek(flatFile, id*superblock->blockSize, SEEK_SET);
	fread(buffer, superblock->blockSize, 1, flatFile);
}

/**
 * Writes data from buffer into the block specified by id. Buffer must be
 * at least superblock->blockSize in length.
 */
void writeBlock(BlockID id, void *buffer) {
	fseek(flatFile, id*superblock->blockSize, SEEK_SET);
	fwrite(buffer, superblock->blockSize, 1, flatFile);
	fflush(flatFile);
}

/**
 * Reads the INode specified by id into the global curNode pointer. This is
 * done because only 1 INode needs to be manipulated at a time, and we use INodes 
 * a lot, so now we don't have to continue to malloc and free INodes
 */
void readINode(INodeID id) {
	fseek(flatFile, id*sizeof(INode) + superblock->firstINodeBlock*superblock->blockSize, SEEK_SET);
	fread(curNode, sizeof(INode), 1, flatFile);
}

/**
 * Writes curNode back to disk, into the INode specified by id.
 */
void writeINode(INodeID id) {
	fseek(flatFile, id*sizeof(INode) + superblock->firstINodeBlock*superblock->blockSize, SEEK_SET);
	fwrite(curNode, sizeof(INode), 1, flatFile);
	fflush(flatFile);
}

/***********************************************************************
 * 
 * Allocation methods
 * 
 ***********************************************************************/

void markBlockUsed(BlockID id) {
	bitmap[id/8] |= 1 << (id % 8);
	writeBlock(superblock->bitmapBlock, bitmap);
	superblock->numFreeBlocks--;
	writeBlock(0, superblock);
}

void markBlockFree(BlockID id) {
	// don't allow anyone to mark INodes or superblock as unused
	if (id < superblock->firstDataBlock) return;
	bitmap[id/8] &= bitmap[id/8] & ~(1 << (id % 8));
	writeBlock(superblock->bitmapBlock, bitmap);
	superblock->numFreeBlocks++;
	writeBlock(0, superblock);
}

void markINodeUsed(INodeID id) {
	readINode(id);
	curNode->flags |= INODE_IN_USE;
	writeINode(id);
	superblock->numFreeINodes--;
	writeBlock(0, superblock);
}

void markINodeFree(INodeID id) {
	readINode(id);
	curNode->flags &= ~INODE_IN_USE;
	writeINode(id);
	superblock->numFreeINodes++;
	writeBlock(0, superblock);
}

/**
 * Finds the next unused INode and allocates it, then returns the ID. curNode
 * will contain the INode data upon exit.
 */
INodeID allocateNextINode() {
	int i;
	
	for (i=0; i<superblock->numINodes; i++) {
		readINode(i);
		if (isFree(curNode)) {
			markINodeUsed(i);
			break;
		}
	}
	
	return i;
}

/**
 * Finds the next free data block on disk, and marks it as used in the bitmap.
 * Then returns the block ID of the newly allocated block.
 */
BlockID allocateNextBlock() {
	int i;
	
	for (i=0; i<superblock->numBlocks; i++) {
		char b = bitmap[i / 8];
		if ((b & (1 << (i % 8))) == 0) {
			markBlockUsed(i);
			break;
		}
	}
	
	return i;
}

/***********************************************************************
 * 
 * File lookup/path traversal functions
 * 
 ***********************************************************************/

int indexOf(char *str, char val) {
	int i = 0;
	for (i=0; i<strlen(str); i++) {
		if (str[i] == val) return i;
	}
	return -1;
}

int lastIndexOf(char *str, char val) {
	int i;
	for (i=strlen(str)-1; i>=0; i--) {
		if (str[i] == val) return i;
	}
	return -1;
}

# define min(x, y) ((x < y) ? x : y)
# define max(x, y) ((x > y) ? x : y)

/**
 * Recursive function call to find file. Starting at the root directory, 
 * it goes through each directory in a specified path, to find the INode of
 * a specific file or directory. It does not care what the last INode represents,
 * only that it exists.
 * 
 * Returns the INodeID of the file specified by path, or -1 if it, or a parent
 * of it, does not exist.
 */
INodeID findFileInternal(INodeID dir, char *path);

INodeID findFileInternal(INodeID dir, char *path) {
	//printf("\nLOOKING FOR: %s IN %d\n", path, dir);
	if (strlen(path) == 0) {
		// if there is no length, return current directory
		//printf("RETURNING %d\n", dir);
		return dir;
	}
	
	int br = indexOf(path, '/');
	char *nextPath = path;
	
	if (br == -1) {
		// on the last file!
		nextPath = nextPath + strlen(nextPath);
	} else {
		// still in a directory, so we will need to search deeper
		nextPath[br] = 0;
		nextPath = nextPath + br + 1;
	}
	
	BlockID blk = 0;
	int i, count, remaining, entriesPerBlock;
	FileEntry *ptr;
	FileEntry *entries = malloc(superblock->blockSize);
	
	readINode(dir);
	remaining = curNode->childCount;
	entriesPerBlock = superblock->blockSize / sizeof(FileEntry);
	
	// each iteration will read 1 block of data
	while (remaining > 0) {
		// read next block
		readBlock(curNode->direct[blk++], entries);
		// read the remaining number of entries, or the whole blocks worth of entities
		count = min(remaining, entriesPerBlock);
		remaining -= count;
		
		for (i=0; i<count; i++) {
			// iterate through each entry
			ptr = &(entries[i]);
			//printf("CMP \n\t'%d' - '%s'\n\t'%d' - '%s'\n", strlen(ptr->value), ptr->value, strlen(path), path);
			if (strcmp(ptr->value, path) == 0) {
				//printf("GOT MATCH\n"); 
				// found a match! Search further down the path
				INodeID id = findFileInternal(ptr->id, nextPath);
				free(entries);
				return id;
			}
		}
	}
	// if we're here, we didn't find a match
	//printf("NO FIND\n");
	free(entries);
	return -1;
}


/**
 * Actual function call, that uses findFileInternal
 */
INodeID findFile(char *path) {
	char *newPath = malloc(strlen(path)+1);
	strcpy(newPath, path);
	char *ptr = newPath;
	
	//printf("\nUNMOD: %s\n", ptr);
	
	if (ptr[0] != '/') {
		// need absolute path
		free(newPath);
		return -1;
	} else {
		// remove leading '/'
		ptr += 1;
	}
	
	if (ptr[strlen(ptr)-1] == '/') {
		// remove trailing '/', meaning this is a directory
		ptr[strlen(ptr)-1] = 0;
	}
	
	//printf("\nMODIF: %s\n", ptr);
	INodeID id = findFileInternal(0, ptr);
	free(newPath);
	return id;
}

/**
 * Adds the given child to the specified parent directory INode. Returns the 
 * index the child was added at in the directory data blocks, or -1 if no
 * space is left to add the child.
 */
int addFileEntry(INodeID dir, INodeID child, char *fname) {
	int childrenPerBlock = superblock->blockSize / sizeof(FileEntry);
	int maxChildren = childrenPerBlock * 12;
	readINode(dir);
	
	if (curNode->childCount == maxChildren) {
		return -1;
	}
	// blk = which direct block this child will be in
	// index = which FileEntry the child is in the block
	int blk = curNode->childCount / childrenPerBlock;
	int index = curNode->childCount % childrenPerBlock;
	
	if (index == 0) {
		// if we are on a new block boundary, allocate a new block
		curNode->direct[blk] = allocateNextBlock();
		curNode->size += superblock->blockSize;
	}
	
	FileEntry *block = malloc(superblock->blockSize);
	readBlock(curNode->direct[blk], block);
	FileEntry *entry = &(block[index]);
	// copy in name and ID
	strcpy(entry->value, fname);
	entry->id = child;
	// write directory block back
	writeBlock(curNode->direct[blk], block);
	curNode->childCount++;
	// write INode back
	writeINode(dir);
	free(block);
	return curNode->childCount - 1;
}

INodeID allocateDir() {
	INodeID id = allocateNextINode();
	if (id == superblock->numINodes) {
		printf("Out of useable INodes!");
		// throw some error
		return id;
	}
	
	curNode->flags |= INODE_DIR;
	curNode->size = 0;	// set size to 0
	curNode->childCount = 0; // no children in directory
	writeINode(id);
	return id;
}

INodeID allocateFile() {
	// for now, just allocates root directory
	INodeID id = allocateNextINode();
	if (id == superblock->numINodes) {
		printf("Out of useable INodes!");
		// throw some error
		return id;
	}
	
	curNode->flags |= INODE_FILE;
	curNode->size = 0;	// set size to 0
	curNode->childCount = 0;
	writeINode(id);
	return id;
}


/**
 * This isn't tested
 * 
 * Gets the block ID that contains the specific offset of the file. 
 * If the offset is larger than the file, returns -1
 */
BlockID getBlockFromOffset(INode *node, int offset) {
	if (offset < node->size) {
		return -1;
	}
	
	int sizes[3];
	int id, index;
	int IDsPerBlock = superblock->blockSize / sizeof(BlockID);
	BlockID *indirect;
	// lists space (bytes) contained by each level of indirection
	
	sizes[0] = 12 * superblock->blockSize;
	sizes[1] = IDsPerBlock * superblock->blockSize;
	sizes[2] = IDsPerBlock * IDsPerBlock * superblock->blockSize;
	
	if (offset < sizes[0]) {
		// divide by blocksize to get which blockID contains the offset
		return node->direct[offset / superblock->blockSize];
	} else if ((offset -= sizes[0]) < sizes[1]) {
		// inside of the single level indirection block
		// read indirection block
		indirect = malloc(superblock->blockSize);
		readBlock(node->singleIndirect, indirect);
	} else {
		// otherwise, the block is inside the double indirection block
		offset -= sizes[1];
		indirect = malloc(superblock->blockSize);
		// divide by how much space each first-level indirection ID takes up
		index = offset / (IDsPerBlock * superblock->blockSize);
		readBlock(node->doubleIndirect, indirect);
		id = indirect[index];
		readBlock(id, indirect);
	}
	
	index = offset / superblock->blockSize;
	id = indirect[index];
	free(indirect);
	return id;
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
	curNode = calloc(sizeof(INode), 1);
	superblock = calloc(BLOCK_SIZE, 1);
	bitmap = calloc(BLOCK_SIZE, 1);
	fread(superblock, BLOCK_SIZE, 1, flatFile);
	
	if (!validSuperBlock(superblock)) {
		// if superblock is not valid, we need to initialize the disk fully
		superblock->blockSize = BLOCK_SIZE;
		superblock->numBlocks = TOTAL_BLOCKS;
		// calculate INode blocks required to address each remaining block as an individual file
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
	
	allocateDir(); 	// allocate root directory
	
	conn->async_read = 0;
	conn->max_write = superblock->blockSize;
	conn->want = FUSE_CAP_EXPORT_SUPPORT | FUSE_CAP_BIG_WRITES;
	
	return (void *) 0;
}

/**
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 *
 * Introduced in version 2.5
 */
int sfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	char *newPath = malloc(strlen(path) + 1);
	strcpy(newPath, path);
	
	INodeID id = findFile(newPath);
	
    if (id == (INodeID) -1) {
		// need to allocate file. But first, we must find the parent path
		int lastIndex = lastIndexOf(newPath, '/');
		// record character we write over to terminate the parent path
		char oldChar = newPath[lastIndex+1];
		newPath[lastIndex+1] = 0;	// terminate parent path
		// make sure parent exists
		INodeID parent = findFile(newPath);
		if (parent == -1) {
			// set some kind of error that the path doesn't exist
			return -1;
		}
		// place old char back
		newPath[lastIndex+1] = oldChar;
		id = allocateFile();
		// add file entry, and get the address of the start of the actual file name
		addFileEntry(parent, id, &(newPath[lastIndex+1]));
	}
	free(newPath);
	return id;
}

/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
int sfs_getattr(const char *path, struct stat *statbuf)
{
	char *newPath = malloc(strlen(path) + 1);
	strcpy(newPath, path);
    INodeID id = findFile(newPath);
    if (id == -1) {
	}
	return 0;
}

/** Create a directory */
int sfs_mkdir(const char *path, mode_t mode)
{
    char *newPath = malloc(strlen(path) + 1);
	strcpy(newPath, path);
	
	INodeID id = findFile(newPath);
	
    if (id == (INodeID) -1) {
		// need to allocate file. But first, we must find the parent path
		int lastIndex = lastIndexOf(newPath, '/');
		// record character we write over to terminate the parent path
		char oldChar = newPath[lastIndex+1];
		newPath[lastIndex+1] = 0;	// terminate parent path
		// make sure parent exists
		INodeID parent = findFile(newPath);
		if (parent == -1) {
			// set some kind of error that the path doesn't exist
			return -1;
		}
		// place old char back
		newPath[lastIndex+1] = oldChar;
		id = allocateDir();
		// add file entry, and get the address of the start of the actual file name
		addFileEntry(parent, id, &(newPath[lastIndex+1]));
	}
	free(newPath);
	return id;
}

void sfs_destroy(void *userdata) {
	fclose(flatFile);
	flatFile = NULL;
	free(superblock);
	superblock = NULL;
	free(bitmap);
	bitmap = NULL;
	free(curNode);
	curNode = NULL;
}

int main(int argc, char *argv[]) {
	struct fuse_conn_info thing;
	sfs_init(&thing);
	printf("%d\n", superblock->numINodeBlocks);
	
	sfs_mkdir("/var", 0);
	sfs_create("/var/thing.txt", 0, NULL);
	sfs_mkdir("/var/lib", 0);
	sfs_create("/var/lib/test.txt", 0, NULL);
	
	printf("\nID: %d\n\n", findFile("/var/lib/test.txt"));
	
	char cwd[1024];
	if (getcwd(cwd, sizeof(cwd)) != NULL)
	   fprintf(stdout, "Current working dir: %s\n", cwd);
	else
	   perror("getcwd() error");
	sfs_destroy(NULL);
	return 0;
   
}
