/*
  Simple File System

  This code is derived from function prototypes found /usr/include/fuse/fuse.h
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  His code is licensed under the LGPLv2.

*/

#include "params.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

#include "log.h"
#include "sfs.h"

/***********************************************************************
 * 
 * Globals & methods to save and load said globals
 * 
 ***********************************************************************/

FILE *flatFile = NULL;
struct SuperBlock *superblock = NULL;
char *bitmap = NULL;
FileHandle *handles;

void loadGlobals() {
	struct sfs_state *data = SFS_DATA;
	flatFile = data->flatFile;
	superblock = data->superblock;
	bitmap = data->bitmap;
}

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
	if (handles != NULL) log_msg("\nREADING BLK %d OFF %d\n", id, id*superblock->blockSize);
	fseek(flatFile, id*superblock->blockSize, SEEK_SET);
	fread(buffer, superblock->blockSize, 1, flatFile);
}

/**
 * Writes data from buffer into the block specified by id. Buffer must be
 * at least superblock->blockSize in length.
 */
void writeBlock(BlockID id, void *buffer) {
	if (handles != NULL) log_msg("\nWRITING BLK %d OFF %d\n", id, id*superblock->blockSize);
	fseek(flatFile, id*superblock->blockSize, SEEK_SET);
	fwrite(buffer, superblock->blockSize, 1, flatFile);
	fflush(flatFile);
}

/**
 * Reads the INode specified by id into the buffer curNode.
 */
void readINode(INodeID id, INode *curNode) {
	if (handles != NULL) log_msg("\nREADING INODE %d FL: %d\n", id);
	fseek(flatFile, id*sizeof(INode) + superblock->firstINodeBlock*superblock->blockSize, SEEK_SET);
	fread(curNode, sizeof(INode), 1, flatFile);
}

/**
 * Writes curNode back to disk, into the INode specified by id.
 */
void writeINode(INodeID id, INode *curNode) {
	if (handles != NULL) log_msg("\nWRITING INODE %d FL: %d\n", id, curNode->flags);
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
	INode curNode;
	readINode(id, &curNode);
	curNode.flags = INODE_IN_USE;
	writeINode(id, &curNode);
	superblock->numFreeINodes--;
	writeBlock(0, superblock);
}

void markINodeFree(INodeID id) {
	INode curNode;
	readINode(id, &curNode);
	curNode.flags &= ~INODE_IN_USE;
	writeINode(id, &curNode);
	superblock->numFreeINodes++;
	writeBlock(0, superblock);
}

/**
 * Finds the next unused INode and allocates it, then returns the ID. curNode
 * will contain the INode data upon exit.
 */
INodeID allocateNextINode() {
	int i;
	INode curNode;
	for (i=0; i<superblock->numINodes; i++) {
		readINode(i, &curNode);
		if (isFree((&curNode))) {
			markINodeUsed(i);
			return i;
		}
	}
	
	return -1;
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
			return i;
		}
	}
	
	return -1;
}

int allocateNextHandle() {
	int i;
	for (i=0; i<NUM_OPEN_FILES; i++) {
		if (handles[i].inUse == false) {
			// if the handle is free, allocate it
			handles[i].inUse = true;
			return i;
		}
	}
	
	errno = ENFILE;
	return -1;
}

void freeHandle(int fd) {
	handles[fd].inUse = false;
}

/***********************************************************************
 * 
 * FileEntry methods
 * 
 ***********************************************************************/
 
# define min(x, y) ((x < y) ? x : y)
# define max(x, y) ((x > y) ? x : y)

/**
 * finds the file/directory specified by fname in dir. The BlockID pointer points
 * to a location to store the block in which the entry is located, and the index 
 * pointer points to a location to store the FileEntry's index in that block.
 * 
 * On success, returns the INodeID of the file/dir specified by fname, 
 * or -1 with errno set if an error occurs
 */
INodeID findFileEntry(INodeID dir, const char *fname, BlockID *block, int *index) {
	BlockID blk = 0;
	int i, count, remaining, entriesPerBlock;
	FileEntry *ptr;
	FileEntry *entries;
	INode curNode;
	
	readINode(dir, &curNode);
	
	if (!isDir((&curNode))) {
		// ensure this is a directory
		errno = ENOTDIR;
		return -1;
	}

	entries = malloc(superblock->blockSize);
	remaining = curNode.childCount;
	entriesPerBlock = superblock->blockSize / sizeof(FileEntry);
	
	// each iteration will read 1 block of data
	while (remaining > 0) {
		// read next block
		readBlock(curNode.blocks[blk++], entries);
		// read the remaining number of entries, or the whole blocks worth of entities
		count = min(remaining, entriesPerBlock);
		remaining -= count;
		
		for (i=0; i<count; i++) {
			// iterate through each entry
			ptr = &(entries[i]);
			if (strcmp(ptr->value, fname) == 0) {
				// found a match! 
				int id = ptr->id;
				free(entries);
				*block = curNode.blocks[blk-1];
				*index = i;
				return id;
			}
		}
	}
	
	free(entries);
	errno = ENOENT;
	return -1;
}

/**
 * Adds the given child to the specified parent directory INode. Returns the 
 * index the child was added at in the directory data blocks, or -1 if no
 * space is left to add the child.
 */
int addFileEntry(INodeID dir, INodeID child, const char *fname) {
	int childrenPerBlock = superblock->blockSize / sizeof(FileEntry);
	int maxChildren = childrenPerBlock * 14;
	INode curNode;
	
	readINode(dir, &curNode);
	
	if (curNode.childCount == maxChildren) {
		errno = ENOSPC;
		return -1;
	}
	// blk = which direct block this child will be in
	// index = which FileEntry the child is in the block
	int blk = curNode.childCount / childrenPerBlock;
	int index = curNode.childCount % childrenPerBlock;
	
	if (curNode.blocks[blk] == 0) {
		// if we are in an unallocated block
		curNode.blocks[blk] = allocateNextBlock();
		if (curNode.blocks[blk] == (BlockID) -1) return -1;
		curNode.size += superblock->blockSize;
	}
	
	FileEntry *block = malloc(superblock->blockSize);
	readBlock(curNode.blocks[blk], block);
	FileEntry *entry = &(block[index]);
	// copy in name and ID
	strcpy(entry->value, fname);
	entry->id = child;
	// write directory block back
	writeBlock(curNode.blocks[blk], block);
	curNode.childCount++;
	// write INode back
	writeINode(dir, &curNode);
	free(block);
	return curNode.childCount - 1;
}

/**
 * Removes file specified by fname from the directory listing of dir, by
 * taking the last FileEntry element and putting it in place of the old entry
 */
void removeFileEntry(INodeID dir, const char *fname) {
	int childrenPerBlock = superblock->blockSize / sizeof(FileEntry);
	// get block and index of fname
	int index;
	BlockID block;
	INode curNode;
	INodeID fileID = findFileEntry(dir, fname, &block, &index);
	// read dir to get child count
	readINode(dir, &curNode);
	// check if it's the last element
	if ((block * childrenPerBlock + index) != (curNode.childCount - 1)) {
		// if we aren't deleting the last element, we have to copy the last element
		// into the FileEntry of fname
		FileEntry lastEntry;
		FileEntry *entries = malloc(superblock->blockSize);
		BlockID lastBlk = (curNode.childCount - 1) / childrenPerBlock;
		int lastIndex = (curNode.childCount - 1) % childrenPerBlock;
		readBlock(curNode.blocks[lastBlk], entries);
		// copy last entry
		memcpy(&lastEntry, &(entries[lastIndex]), sizeof(FileEntry));
		// read block containing fname
		readBlock(block, entries);
		// copy entry into proper index
		memcpy(&(entries[index]), &lastEntry, sizeof(FileEntry));
		writeBlock(block, entries);
		free(entries);
	}
	
	curNode.childCount--;
	writeINode(dir, &curNode);
	return;
}


/***********************************************************************
 * 
 * Directory navigation methods
 * 
 ***********************************************************************/

int indexOf(const char *str, char val) {
	int i = 0;
	for (i=0; i<strlen(str); i++) {
		if (str[i] == val) return i;
	}
	return -1;
}

int lastIndexOf(const char *str, char val) {
	int i;
	for (i=strlen(str)-1; i>=0; i--) {
		if (str[i] == val) return i;
	}
	return -1;
}

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
	//log_msg("\nLOOKING FOR: %s IN %d\n", path, dir);
	if (strlen(path) == 0) {
		// if there is no length, return current directory
		return dir;
	}
	
	int br = indexOf(path, '/');
	char *nextPath = path;
	
	if (br == -1) {
		// on the last file!
		nextPath = nextPath + strlen(nextPath);
	} else {
		// still in a directory, so we will need to search deeper
		// set the '/' to \0 to end the string of the subdir to move to
		nextPath[br] = 0;
		// move nextPath up to exclude this subdir
		nextPath = nextPath + br + 1;
	}
	
	if (strlen(path) > 123) {
		// ensure path length is okay
		errno = ENAMETOOLONG;
		return -1;
	}
	
	BlockID blk;
	INodeID id;
	int index;
	//log_msg("CUR: %s NEXT: %s\n\n", path, nextPath);
	id = findFileEntry(dir, path, &blk, &index);
	if (id == (INodeID) -1) return -1;
	
	return findFileInternal(id, nextPath);
}


/**
 * Actual function call, that uses findFileInternal
 */
INodeID findFile(const char *path) {
	// newPath always points to the start of the malloc()'ed space. PTR may change.
	char *newPath = malloc(strlen(path)+1);
	strcpy(newPath, path);
	char *ptr = newPath;
	
	if (ptr[0] != '/') {
		// need absolute path
		free(newPath);
		errno = EIO;
		return -1;
	} else {
		// remove leading '/'
		ptr += 1;
	}
	
	if (ptr[strlen(ptr)-1] == '/') {
		// remove trailing '/', as we do not care what the endpoint is
		ptr[strlen(ptr)-1] = 0;
	}
	
	INodeID id = findFileInternal(0, ptr);
	free(newPath);
	return id;
}

INodeID findParent(const char *path) {
	char *newPath = malloc(strlen(path) + 1);
	strcpy(newPath, path);
	// 
	int lastIndex = lastIndexOf(newPath, '/');
	newPath[lastIndex+1] = 0;	// terminate parent path
	// make sure parent exists
	INodeID parent = findFile(newPath);
	free(newPath);
	return parent;
}

/***********************************************************************
 * 
 * File allocation methods
 * 
 ***********************************************************************/

INodeID allocateFile(bool isDir) {
	INode curNode;
	INodeID id = allocateNextINode();
	if (id == (INodeID) -1) {
		errno = ENOSPC;
		return -1;
	}
	
	BlockID blk = allocateNextBlock();
	if (blk == (BlockID) -1) {
		markINodeFree(id);
		errno = ENOSPC;
		return -1;
	}
	
	readINode(id, &curNode);
	int i;
	
	for (i=0; i<14; i++) {
		curNode.blocks[i] = 0;
	}
	
	curNode.flags |= (isDir) ? INODE_DIR : INODE_FILE;
	curNode.size = (isDir) ? superblock->blockSize : 0;	// set size to 0
	curNode.childCount = 0; 				// no children in directory
	curNode.lastAccess = time(NULL);
	curNode.lastChange = curNode.lastAccess;
	curNode.lastModify = curNode.lastAccess;
	curNode.blocks[0] = blk;
	writeINode(id, &curNode);
	return id;
}

/**
 * This isn't tested
 * 
 * Gets the block ID that contains the specific offset of the file. 
 * If the offset is larger than the file, returns -1
 */
BlockID getBlockFromOffset(INode *node, int offset) {
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
        log_msg("\n trying to get %d which is %d\n", offset/superblock->blockSize, 
                node->blocks[offset / superblock->blockSize]);
		return node->blocks[offset / superblock->blockSize];
	} else if ((offset -= sizes[0]) < sizes[1]) {
		// inside of the single level indirection block
		// read indirection block
		if (node->blocks[12] == 0) return 0;
		indirect = malloc(superblock->blockSize);
		readBlock(node->blocks[12], indirect);
	} else {
		// otherwise, the block is inside the double indirection block
		offset -= sizes[1];
		if (node->blocks[13] == 0) return 0;
		indirect = malloc(superblock->blockSize);
		// divide by how much space each first-level indirection ID takes up
		index = offset / (IDsPerBlock * superblock->blockSize);
		readBlock(node->blocks[13], indirect);
		id = indirect[index];
		if (id == 0) return 0;
		readBlock(id, indirect);
	}
	
	index = (offset / superblock->blockSize) % (IDsPerBlock); 
	id = indirect[index];
	if (id == 0) {
		log_msg("\n returning 0 and index = %d \n", index);
	}
	free(indirect);
	return id;
}

/***********************************************************************
 * 
 * SFS Methods
 * 
 ***********************************************************************/

void *sfs_init(struct fuse_conn_info *conn) {
	conn->async_read = 0;
	conn->max_write = superblock->blockSize;
	conn->want = FUSE_CAP_EXPORT_SUPPORT;
	
	log_msg("\nsfs_init()\n");
    log_conn(conn);
    log_fuse_context(fuse_get_context());
    
	return SFS_DATA;
}

/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
int sfs_getattr(const char *path, struct stat *statbuf)
{
	log_msg("\nsfs_getattr(path=\"%s\", statbuf=0x%08x)\n",
	  path, statbuf);
	  
	loadGlobals();
    INodeID id = findFile(path);
    INode curNode;
    if (id == -1) return -errno;
	
	readINode(id, &curNode);
	
	statbuf->st_mode = ((isDir((&curNode))) ? S_IFDIR : S_IFREG) | S_IRWXU | S_IRWXG | S_IRWXO;
	statbuf->st_nlink = 1;
	statbuf->st_ino = id;
	statbuf->st_uid = 0;
	statbuf->st_gid = 0;
	statbuf->st_size = curNode.size;
	statbuf->st_atime = curNode.lastAccess;
	statbuf->st_mtime = curNode.lastModify;
	statbuf->st_ctime = curNode.lastChange;
	statbuf->st_blksize = superblock->blockSize;
	statbuf->st_blocks = (curNode.size / 512);
	return 0;
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 * Changed in version 2.2
 */
int sfs_open(const char *path, struct fuse_file_info *fi)
{
    log_msg("\nsfs_open(path\"%s\", fi=0x%08x)\n",
	    path, fi);

    INodeID id = findFile(path);
    if (id == (INodeID) -1) return -errno;
    int handle = allocateNextHandle();
    if (handle == -1) return -errno;
    
    handles[handle].id = id;
    handles[handle].flags = fi->flags;
    handles[handle].index = 0;
    
    fi->fh = handle;
    
    return 0;
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
	log_msg("\nsfs_create(path=\"%s\", mode=0%03o, fi=0x%08x)\n",
	    path, mode, fi);
	
	loadGlobals();
	INodeID id = findFile(path);
	
    if (id == (INodeID) -1) {
		// need to allocate file. But first, we must find the parent path
		INodeID parent = findParent(path);
		INode curNode;
		if (parent == (INodeID) -1) return -errno;
		// update parent timestamps
		readINode(parent, &curNode);
		curNode.lastAccess = time(NULL);
		curNode.lastChange = curNode.lastAccess;
		curNode.lastModify = curNode.lastAccess;
		writeINode(parent, &curNode);
		
		id = allocateFile(false);
		if (id == (INodeID) -1) return -errno;
		// add file entry, and get the address of the start of the actual file name
		int lastIndex = lastIndexOf(path, '/');
		int val = addFileEntry(parent, id, &(path[lastIndex+1]));
		if (val == -1) return -errno;
	}
	return sfs_open(path, fi);
}

/** Create a directory */
int sfs_mkdir(const char *path, mode_t mode)
{
	log_msg("\nsfs_mkdir(path=\"%s\", mode=0%3o)\n",
	    path, mode);
	    
	loadGlobals();
	// directory cannot exist
	INode curNode;
	INodeID id = findFile(path);
	if (id != (INodeID) -1) return -EEXIST;
	// need to allocate file. But first, we must find the parent path
	INodeID parent = findParent(path);
	if (parent == (INodeID) -1) return -errno;
	// update parent timestamps
	readINode(parent, &curNode);
	curNode.lastAccess = time(NULL);
	curNode.lastChange = curNode.lastAccess;
	curNode.lastModify = curNode.lastAccess;
	writeINode(parent, &curNode);
	log_msg("in mkdir about to allocateFile\n");
	id = allocateFile(true);
	log_msg("in mkdir allocated File\n");
	if (id == (INodeID) -1) return -errno;
	// add file entry, and get the address of the start of the actual file name
	int lastIndex = lastIndexOf(path, '/');
	int val = addFileEntry(parent, id, &(path[lastIndex+1]));
	log_msg("in mkdir added file entry %d\n", val);
	if (val == -1) return -errno;
	
	return 0;
}

void sfs_destroy(void *userdata) {
	log_msg("\nsfs_destroy(userdata=0x%08x)\n", userdata);
	loadGlobals();
	fclose(SFS_DATA->logfile);
	fclose(flatFile);
	free(superblock);
	free(bitmap);
	free(handles);
	free(fuse_get_context()->private_data);
}

/** Remove a file */
int sfs_unlink(const char *path)
{
    int retstat = 0;
    log_msg("\nsfs_unlink(path\"%s\"\n",
	    path);

    INodeID id = findFile(path);
    if (id == -1) return -errno;
    
    int i, j;
    int idsPerBlock = superblock->blockSize / sizeof(BlockID);
    BlockID *indirect1, *indirect2;
    INode curNode;
    
    readINode(id, &curNode);
    
    if (curNode.blocks[13] != 0) {
		indirect1 = malloc(superblock->blockSize);
		indirect2 = malloc(superblock->blockSize);
		readBlock(curNode.blocks[13], indirect1);
		for (i=0; i<idsPerBlock; i++) {
			if (indirect1[i] == 0) break;
			readBlock(indirect1[i], indirect2);
			for (j=0; j<idsPerBlock; j++) {
				if (indirect2[j] == 0) break;
				markBlockFree(indirect2[j]);
			}
			markBlockFree(indirect1[i]);
		}
		markBlockFree(curNode.blocks[13]);
		free(indirect1);
		free(indirect2);
	}
	
	if (curNode.blocks[12] != 0) {
		indirect1 = malloc(superblock->blockSize);
		readBlock(curNode.blocks[12], indirect1);
		for (i=0; i<idsPerBlock; i++) {
			if (indirect1[i] == 0) break;
			markBlockFree(indirect1[i]);
		}
		markBlockFree(curNode.blocks[12]);
		free(indirect1);
	}
	
	for (i=0; i<12; i++) {
		if (curNode.blocks[i] == 0) break;
		markBlockFree(curNode.blocks[i]);
	}
	readINode(id, &curNode);
	memset(&curNode, 0, sizeof(INode));
	writeINode(id, &curNode);
	// mark INode as free
	markINodeFree(id);
	// get ending file name to remove it from parent directory
	char *name, *copy;
	name = copy = malloc(strlen(path) + 1);
	strcpy(copy, path);
	i = lastIndexOf(name, '/');
	if (i == strlen(name) - 1) {
		// remove ending slash
		name[i] = 0;
		i = lastIndexOf(name, '/');
	}
	
	name += i + 1;
    // remove entry from parent directory, by taking the last element
    // of the parent directory and placing it in place of the entry being removed
	INodeID parent = findParent(path);
	removeFileEntry(parent, name);
	free(copy);
    return retstat;
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 *
 * Changed in version 2.2
 */
int sfs_release(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_release(path=\"%s\", fi=0x%08x)\n",
	  path, fi);
    freeHandle(fi->fh);
    return retstat;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
int sfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	INode curNode;
    int id = handles[fi->fh].id, relOffset = 0, remaining = size;
    int blockSize = superblock->blockSize;
    readINode(id, &curNode);
    curNode.lastAccess = time(NULL);
    writeINode(id, &curNode);
    int curFileSize = curNode.size; 
    if (offset > curFileSize) {
         return 0;
    }
    int difference = 0;
    if (size + offset > curFileSize){
	log_msg("FIXED SIZE size before = %d FILE SIZE = %d\n", size, curFileSize);
        difference = size + offset - curFileSize;	
	log_msg("FIXED SIZE difference = %d \n", difference);
	size = curFileSize - offset;
	remaining = size;
	log_msg("FIXED SIZE size - %d\n", size);
    }
    log_msg("\nsfs_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
	    path, buf, size, offset, fi);
    if (size == 0) {
        log_msg("\n size = 0 returning 0 \n");
        return 0;
    }
    
    char * blockBuf = malloc(superblock->blockSize); 
    BlockID blockToRead = getBlockFromOffset(&curNode, offset);
    log_msg("\nAbout to read block %d\n",blockToRead);
    readBlock(blockToRead, blockBuf);
    int bytesToRead = min(blockSize-(offset%blockSize), remaining);
    memcpy(buf, blockBuf + (offset % blockSize), bytesToRead);
    remaining -= bytesToRead;
    relOffset += bytesToRead;
    while (remaining != 0) {
		blockToRead = getBlockFromOffset(&curNode, offset+relOffset);
		bytesToRead = min(blockSize, remaining);
		readBlock(blockToRead, blockBuf);
		memcpy(buf + (size-remaining), blockBuf, bytesToRead);
		relOffset += bytesToRead;
		remaining -= bytesToRead;
    } 
    free(blockBuf); 
    if (difference != 0){
       memset(buf + size, 0, difference);
    }
    log_msg("\nAbout to return %d", size);
    return size;
}

/**
 * NEEDS TO BE TESTED UNSURE HOW TO TEST IT
 * AssignNextBlock should take in an iNode and put in another 
 * block of data into its corresponding field. It then returns
 * the blockID of the new block.
 */
BlockID assignNextBlock(INodeID id, INode * toAssign) {
    int i = 0, j = 0;
    int idsPerBlock = superblock->blockSize / sizeof(BlockID);
    BlockID blk = allocateNextBlock(); // block we'll be assigning
    if (blk == (BlockID) -1) return -1;
    
    INode curNode;
    readINode(id, &curNode);
    
    //check if one of the direct blocks is free
    for (i = 0; i <= 11; i++){
        if (curNode.blocks[i] == 0) {
            curNode.blocks[i] = blk;
            toAssign->blocks[i] = blk;
            log_msg("\n giving inode %d blk %d in spot %d \n", id, blk, i);
            writeINode(id, &curNode);
            return blk;
		} 
    }
    
    //allocate a first level indirection if neccessary
    if (curNode.blocks[12] == 0) {
        curNode.blocks[12] = blk;
        toAssign->blocks[12] = blk;
    	blk = allocateNextBlock();
    	if (blk == (BlockID) -1) {
			// free first indirection block 
			markBlockFree(curNode.blocks[12]);
			return -1;
		} else {
			// write indirection right here & write 0's to the rest of the block
			BlockID *block = calloc(superblock->blockSize, 1);
			block[0] = blk;
			writeBlock(curNode.blocks[12], block);	// write block back
			writeINode(id, &curNode);
			free(block);
			return blk;
		}
    }
    
    BlockID *indirect1 = malloc(superblock->blockSize);
    //look for first free spot in first level indirection
    readBlock(curNode.blocks[12], indirect1); 
    for (i=0; i<idsPerBlock; i++) {
        if (indirect1[i] == 0) {
			// write to this slot
			indirect1[i] = blk;
			writeBlock(curNode.blocks[12], indirect1);
			free(indirect1);
			return blk;
		}
    }
    // at this point, indirect1 is still allocated
    //allocate a second level indirection if neccessary
    if (curNode.blocks[13] == 0){
        curNode.blocks[13] = blk;
        toAssign->blocks[13] = blk;
        blk = allocateNextBlock();
        if (blk == (BlockID) -1) {
			// free blocks that may have been allocated
			markBlockFree(curNode.blocks[13]);
			free(indirect1);
			return -1;
		}
		// clear block
		memset(indirect1, 0, superblock->blockSize);
        writeBlock(curNode.blocks[13], indirect1);	// write 0's to block
        writeINode(id, &curNode);
    }
    
    BlockID *indirect2 = malloc(superblock->blockSize);
    // read first indirection block
    readBlock(curNode.blocks[13], indirect1); 
    for (i=0; i<idsPerBlock; i++) {
		if (indirect1[i] == 0) {
			// need to allocate an indirection block
			indirect1[i] = blk;
			blk = allocateNextBlock();
			if (blk == (BlockID) -1) {
				markBlockFree(indirect1[i]);
				free(indirect1);
				free(indirect2);
				return -1;
			}
			writeBlock(curNode.blocks[13], indirect1);
			memset(indirect2, 0, superblock->blockSize);
			writeBlock(indirect1[i], indirect2);	// write 0's to block
		}
		
		readBlock(indirect1[i], indirect2);
		for (j=0; j<idsPerBlock; j++) {
			if (indirect2[j] == 0) {
				indirect2[j] = blk;
				writeBlock(indirect1[i], indirect2);
				free(indirect1);
				free(indirect2);
				return blk;
			}
		}
    }
    free(indirect1);
	free(indirect2);
    //well shit thats a big file
    return -1;
}
/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
int sfs_write(const char *path, const char *buf, size_t size, off_t offset,
	     struct fuse_file_info *fi)
{
    log_msg("\nsfs_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n", path, buf, size, offset, fi);
    
    int id = handles[fi->fh].id, written = 0;
    INode curNode;
    readINode(id, &curNode);
    log_msg("\ninode %d block 0 = %d block 1 = %d\n", id, curNode.blocks[0],
            curNode.blocks[1]);
    BlockID firstHalf = getBlockFromOffset(&curNode, offset);
    if (firstHalf == 0) {
        firstHalf = assignNextBlock(id, &curNode);
        if (firstHalf == (BlockID) -1) return -errno;	// ran out of space
		log_msg("\nassigning new block %d\n", firstHalf);
    }
    log_msg("\nAFTER inode %d block 0 = %d block 1 = %d\n", id, curNode.blocks[0],
            curNode.blocks[1]);
    char * blockBuf = malloc(superblock->blockSize);
    readBlock(firstHalf, blockBuf);
    int blocksize = superblock->blockSize;
    int toWrite = min(blocksize - (offset % blocksize), (int) size);
    memcpy(blockBuf+(offset % blocksize), buf, toWrite);
    writeBlock(firstHalf, blockBuf);
    log_msg("\nWrote %d to block %d", toWrite, firstHalf);
    written += toWrite;
    while (written != size) {
        toWrite = min(blocksize, size - written);    
        //check if INode exists in the next space (it shouldn't but worth checking)
        BlockID blockToWrite = getBlockFromOffset(&curNode, offset+written);
		if (blockToWrite == 0) {
			blockToWrite = assignNextBlock(id, &curNode);
			if (blockToWrite == (BlockID) -1) {
				// ran out of space
				free(blockBuf);
				return -errno;
			}
		}
    	log_msg("\nAbout to write %d to block %d", toWrite, blockToWrite);
    	readBlock(blockToWrite, blockBuf);
        memcpy(blockBuf, buf+written, toWrite);
        writeBlock(blockToWrite, blockBuf);
		written += toWrite;
        //if so start writting to that
        //else assignanewblock to the inode and write to that
	//increase written by toWrite
    }
    log_msg("\nAbout to return %d", written);
    curNode.size += written;
    curNode.lastAccess = time(NULL);
	curNode.lastChange = curNode.lastAccess;
	curNode.lastModify = curNode.lastAccess;
    writeINode(id, &curNode);
    return written;
}

/** Remove a directory */
int sfs_rmdir(const char *path)
{
    int i, id;
    log_msg("sfs_rmdir(path=\"%s\")\n",
	    path);
    // ensure file exists
    id = findFile(path);
    if (id == -1) return -errno;
    INode curNode;
    readINode(id, &curNode);
    // directory needs to be empty
    if (curNode.childCount > 0) return -ENOTEMPTY;
    // free all data blocks connected to INode
    for (i=0; i<curNode.size / superblock->blockSize; i++) {
		markBlockFree(curNode.blocks[i]);
	}
	// mark INode as free
	markINodeFree(id);
	// get ending file name to remove it from parent directory
	char *name, *copy;
	name = copy = malloc(strlen(path) + 1);
	strcpy(copy, path);
	i = lastIndexOf(name, '/');
	if (i == strlen(name) - 1) {
		// remove ending slash
		name[i] = 0;
		i = lastIndexOf(name, '/');
	}
	
	name += i + 1;
    // remove entry from parent directory, by taking the last element
    // of the parent directory and placing it in place of the entry being removed
	INodeID parent = findParent(path);
	removeFileEntry(parent, name);
	free(copy);
    return 0;
}


/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 *
 * Introduced in version 2.3
 */
int sfs_opendir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_opendir(path=\"%s\", fi=0x%08x)\n",
	  path, fi);
    
    
    return retstat;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 * Introduced in version 2.3
 */
int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
	       struct fuse_file_info *fi)
{
	log_msg("\nsfs_readdir()\n");
	loadGlobals();
    BlockID blk = 0;
	int i, count, remaining, entriesPerBlock;
	FileEntry *ptr;
	FileEntry *entries = malloc(superblock->blockSize);

	i = findFile(path);
	if (i == -1) return -errno;
	
	INode curNode;
	readINode(i, &curNode);
	remaining = curNode.childCount;
	entriesPerBlock = superblock->blockSize / sizeof(FileEntry);
	
	// each iteration will read 1 block of data
	while (remaining > 0) {
		// read next block
		readBlock(curNode.blocks[blk++], entries);
		// read the remaining number of entries, or the whole blocks worth of entities
		count = min(remaining, entriesPerBlock);
		remaining -= count;
		
		for (i=0; i<count; i++) {
			// iterate through each entry
			ptr = &(entries[i]);
			if (filler(buf, ptr->value, NULL, 0) != 0) {
				free(entries);
				return -ENOMEM;
			}
		}
	}
	free(entries);
    return 0;
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int sfs_releasedir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_releasedir()\n");
    
    return retstat;
}

struct fuse_operations sfs_oper = {
  .init = sfs_init,
  .destroy = sfs_destroy,

  .getattr = sfs_getattr,
  .create = sfs_create,
  .unlink = sfs_unlink,
  .open = sfs_open,
  .release = sfs_release,
  .read = sfs_read,
  .write = sfs_write,

  .rmdir = sfs_rmdir,
  .mkdir = sfs_mkdir,

  .opendir = sfs_opendir,
  .readdir = sfs_readdir,
  .releasedir = sfs_releasedir
};

void sfs_usage()
{
    fprintf(stderr, "usage:  sfs [FUSE and mount options] diskFile mountPoint\n");
    abort();
}

int main(int argc, char *argv[])
{
    int fuse_stat;
    struct sfs_state *sfs_data;
    
    // sanity checking on the command line
    if ((argc < 3) || (argv[argc-2][0] == '-') || (argv[argc-1][0] == '-'))
	sfs_usage();

    sfs_data = malloc(sizeof(struct sfs_state));
    if (sfs_data == NULL) {
		perror("main calloc");
		abort();
    }

    // Pull the diskfile and save it in internal data
    sfs_data->diskfile = argv[argc-2];
    argv[argc-2] = argv[argc-1];
    argv[argc-1] = NULL;
    argc--;
    
    sfs_data->logfile = log_open();
    //******************************************************************/
	int i, nothing = 0;
	flatFile = fopen(sfs_data->diskfile, "r+");
	
	if (flatFile == NULL) {
		// if we can't open for updating, we open for writing to create it
		flatFile = fopen(sfs_data->diskfile, "w");
		// write last byte to set file size
		fseek(flatFile, TOTAL_SIZE - 1, SEEK_SET);
		fwrite((void *) &nothing, 1, 1, flatFile);
		fclose(flatFile);
		// reopen for updating
		flatFile = fopen(sfs_data->diskfile, "r+");
	}
	
	fseek(flatFile, 0, SEEK_END);
	if (ftell(flatFile) < TOTAL_SIZE) {
		fseek(flatFile, TOTAL_SIZE - 1, SEEK_SET);
		fwrite((void *) &nothing, 1, 1, flatFile);
		fflush(flatFile);
	}
	// read superblock
	superblock = calloc(BLOCK_SIZE, 1);
	bitmap = calloc(BLOCK_SIZE, 1);
	fseek(flatFile, 0, SEEK_SET);
	fread(superblock, BLOCK_SIZE, 1, flatFile);
	
	if (!validSuperBlock(superblock)) {
		printf("invalid %x\n", superblock->magic);
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
		// mark first n+2 blocks as used (superblock + n INode blocks + bitmap block)
		for (i=0; i < 2+superblock->numINodeBlocks; i++) {
			markBlockUsed(i);
		}
	} 
	
	readBlock(superblock->bitmapBlock, bitmap);
	
	if (superblock->numINodes == superblock->numFreeINodes) {
		allocateFile(true); 	// allocate root directory
	}
	
	printf("Inode size: %d\n", sizeof(INode));
	
	handles = calloc(sizeof(FileHandle) * NUM_OPEN_FILES, 1);
		
	sfs_data->flatFile = flatFile;
	sfs_data->superblock = superblock;
	sfs_data->bitmap = bitmap;
	sfs_data->handles = handles;
	//******************************************************************/
    
    // turn over control to fuse
    fprintf(stderr, "about to call fuse_main, %s \n", sfs_data->diskfile);
    fuse_stat = fuse_main(argc, argv, &sfs_oper, sfs_data);
    fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
    return fuse_stat;
}
