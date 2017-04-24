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
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
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
INode *curNode = NULL;

void saveGlobals() {
	struct sfs_state *data = SFS_DATA;
	data->flatFile = flatFile;
	data->superblock = superblock;
	data->bitmap = bitmap;
	data->curNode = curNode;
}

void loadGlobals() {
	struct sfs_state *data = SFS_DATA;
	flatFile = data->flatFile;
	superblock = data->superblock;
	bitmap = data->bitmap;
	curNode = data->curNode;
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

/***********************************************************************
 * 
 * File lookup functions
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
	
	if (strlen(path) > 124) {
		// ensure path length is okay
		errno = ENAMETOOLONG;
		return -1;
	}
	
	BlockID blk = 0;
	int i, count, remaining, entriesPerBlock;
	FileEntry *ptr;
	FileEntry *entries = malloc(superblock->blockSize);
	
	readINode(dir);
	
	if (!isDir(curNode)) {
		// ensure this is a directory
		errno = ENOTDIR;
		free(entries);
		return -1;
	}
	
	remaining = curNode->childCount;
	entriesPerBlock = superblock->blockSize / sizeof(FileEntry);
	
	// each iteration will read 1 block of data
	while (remaining > 0) {
		// read next block
		readBlock(curNode->blocks[blk++], entries);
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
	free(entries);
	errno = ENOENT;
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
		errno = EIO;
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

/***********************************************************************
 * 
 * File allocation methods
 * 
 ***********************************************************************/

/**
 * Adds the given child to the specified parent directory INode. Returns the 
 * index the child was added at in the directory data blocks, or -1 if no
 * space is left to add the child.
 */
int addFileEntry(INodeID dir, INodeID child, char *fname) {
	int childrenPerBlock = superblock->blockSize / sizeof(FileEntry);
	int maxChildren = childrenPerBlock * 15;
	readINode(dir);
	
	if (curNode->childCount == maxChildren) {
		errno = ENOSPC;
		return -1;
	}
	// blk = which direct block this child will be in
	// index = which FileEntry the child is in the block
	int blk = curNode->childCount / childrenPerBlock;
	int index = curNode->childCount % childrenPerBlock;
	
	if (blk > 0 && index == 0) {
		// if we are on a new block boundary, allocate a new block
		curNode->blocks[blk] = allocateNextBlock();
		if (curNode->blocks[blk] == (BlockID) -1) return -1;
		curNode->size += superblock->blockSize;
	}
	
	FileEntry *block = malloc(superblock->blockSize);
	readBlock(curNode->blocks[blk], block);
	FileEntry *entry = &(block[index]);
	// copy in name and ID
	strcpy(entry->value, fname);
	entry->id = child;
	// write directory block back
	writeBlock(curNode->blocks[blk], block);
	curNode->childCount++;
	// write INode back
	writeINode(dir);
	free(block);
	return curNode->childCount - 1;
}

INodeID allocateFile(bool isDir) {
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
	
	readINode(id);
	int i;
	
	for (i=0; i<15; i++) {
		curNode->blocks[i] = 0;
	}

	curNode->flags |= (isDir) ? INODE_DIR : INODE_FILE;
	curNode->size = 0;	// set size to 0
	curNode->childCount = 0; // no children in directory
	curNode->lastAccess = time(NULL);
	curNode->lastChange = curNode->lastAccess;
	curNode->lastModify = curNode->lastAccess;
	curNode->blocks[0] = blk;
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
		return node->blocks[offset / superblock->blockSize];
	} else if ((offset -= sizes[0]) < sizes[1]) {
		// inside of the single level indirection block
		// read indirection block
		indirect = malloc(superblock->blockSize);
		readBlock(node->blocks[13], indirect);
	} else {
		// otherwise, the block is inside the double indirection block
		offset -= sizes[1];
		indirect = malloc(superblock->blockSize);
		// divide by how much space each first-level indirection ID takes up
		index = offset / (IDsPerBlock * superblock->blockSize);
		readBlock(node->blocks[14], indirect);
		id = indirect[index];
		readBlock(id, indirect);
	}
	
	index = offset / superblock->blockSize;
	id = indirect[index];
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
		
		if (parent == (INodeID) -1) {
			free(newPath);
			return -errno;
		}
		readINode(parent);
		curNode->lastAccess = time(NULL);
		curNode->lastChange = curNode->lastAccess;
		curNode->lastModify = curNode->lastAccess;
		writeINode(parent);
		// place old char back
		newPath[lastIndex+1] = oldChar;
		id = allocateFile(false);
		
		if (id == (INodeID) -1) {
			free(newPath);
			return -errno;
		}
		// add file entry, and get the address of the start of the actual file name
		int val = addFileEntry(parent, id, &(newPath[lastIndex+1]));
		
		if (val == -1) {
			free(newPath);
			return -errno;
		}
	}
	
	free(newPath);
	return 0;
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
	char *newPath = malloc(strlen(path) + 1);
	strcpy(newPath, path);
    INodeID id = findFile(newPath);
    free(newPath);
    
    if (id == -1) {
		return -errno;
	}
	
	readINode(id);

	statbuf->st_mode = ((isDir(curNode)) ? S_IFDIR : S_IFREG) | S_IRWXU | S_IRWXG | S_IRWXO;
	statbuf->st_nlink = 1;
	statbuf->st_uid = 0;
	statbuf->st_gid = 0;
	statbuf->st_size = curNode->size;
	statbuf->st_atime = curNode->lastAccess;
	statbuf->st_mtime = curNode->lastModify;
	statbuf->st_ctime = curNode->lastChange;
	statbuf->st_blksize = superblock->blockSize;
	statbuf->st_blocks = curNode->size / superblock->blockSize + 1;
	return 0;
}

/** Create a directory */
int sfs_mkdir(const char *path, mode_t mode)
{
	log_msg("\nsfs_mkdir(path=\"%s\", mode=0%3o)\n",
	    path, mode);
	    
	loadGlobals();
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
		
		if (parent == (INodeID) -1) {
			free(newPath);
			return -errno;
		}
		readINode(parent);
		curNode->lastAccess = time(NULL);
		curNode->lastChange = curNode->lastAccess;
		curNode->lastModify = curNode->lastAccess;
		writeINode(parent);
		// place old char back
		newPath[lastIndex+1] = oldChar;
		id = allocateFile(true);
		
		if (id == (INodeID) -1) {
			free(newPath);
			return -errno;
		}
		// add file entry, and get the address of the start of the actual file name
		int val = addFileEntry(parent, id, &(newPath[lastIndex+1]));
		
		if (val == -1) {
			free(newPath);
			return -errno;
		}
	}
	
	free(newPath);
	return 0;
}

void sfs_destroy(void *userdata) {
	log_msg("\nsfs_destroy(userdata=0x%08x)\n", userdata);
	loadGlobals();
	fclose(SFS_DATA->logfile);
	fclose(flatFile);
	flatFile = NULL;
	free(superblock);
	superblock = NULL;
	free(bitmap);
	bitmap = NULL;
	free(curNode);
	curNode = NULL;
	free(fuse_get_context()->private_data);
}

/** Remove a file */
int sfs_unlink(const char *path)
{
    int retstat = 0;
    log_msg("sfs_unlink(path=\"%s\")\n", path);

    
    return retstat;
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
    int retstat = 0;
    log_msg("\nsfs_open(path\"%s\", fi=0x%08x)\n",
	    path, fi);

    
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
    int retstat = 0;
    log_msg("\nsfs_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
	    path, buf, size, offset, fi);

   
    return retstat;
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
    int retstat = 0;
    log_msg("\nsfs_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
	    path, buf, size, offset, fi);
    
    
    return retstat;
}

/** Remove a directory */
int sfs_rmdir(const char *path)
{
    int retstat = 0;
    log_msg("sfs_rmdir(path=\"%s\")\n",
	    path);
    
    char *newPath = malloc(strlen(path) + 1);
    strcpy(newPath, path);
    int id = findFile(newPath);
    free(newPath);
    
    if (id == -1) return -errno;
    
    readINode(id);
    
    if (curNode->childCount > 0) return ENOTEMPTY;
    
    
    
    return retstat;
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
    int retstat = 0;
    char *newPath = malloc(strlen(path) + 1);
    BlockID blk = 0;
	int i, count, remaining, entriesPerBlock;
	FileEntry *ptr;
	FileEntry *entries = malloc(superblock->blockSize);
	
	strcpy(newPath, path);
	i = findFile(newPath);
	free(newPath);
	if (i == -1) return -errno;
	
	readINode(i);
	remaining = curNode->childCount;
	entriesPerBlock = superblock->blockSize / sizeof(FileEntry);
	
	// each iteration will read 1 block of data
	while (remaining > 0) {
		// read next block
		readBlock(curNode->blocks[blk++], entries);
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
    return retstat;
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
	curNode = calloc(sizeof(INode), 1);
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
		printf("%d - %d\n", superblock->numINodes, superblock->numFreeINodes);
		allocateFile(true); 	// allocate root directory
	}
		
	sfs_data->flatFile = flatFile;
	sfs_data->superblock = superblock;
	sfs_data->bitmap = bitmap;
	sfs_data->curNode = curNode;
	//******************************************************************/
    
    // turn over control to fuse
    fprintf(stderr, "about to call fuse_main, %s \n", sfs_data->diskfile);
    fuse_stat = fuse_main(argc, argv, &sfs_oper, sfs_data);
    fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
    return fuse_stat;
}
