
#ifndef __SFS_H_
# define __SFS_H_

# include <stdint.h>
# include <time.h>
# include <stdbool.h>

// cannot exceed 32768 blocks without increasing blocksize!
// Only 1 block is allocated for the in-use bitmap. 4096*8 = 32768
# define BLOCK_SIZE			4096
# define TOTAL_BLOCKS		32768
# define TOTAL_SIZE			(TOTAL_BLOCKS*BLOCK_SIZE)

typedef uint32_t INodeID;
typedef uint32_t BlockID;

typedef struct {
	int flags, size, childCount;
	time_t lastAccess, lastModify, lastChange;
	INodeID blocks[14];
} INode;

# define INODE_IN_USE	0x1
# define INODE_TYPE		0x6
# define INODE_FILE		0x2
# define INODE_DIR		0x4

typedef struct {
	char value[124];
	INodeID id;
} FileEntry;

# define isFree(node)	(!((node->flags & INODE_IN_USE) == INODE_IN_USE))
# define getType(node)	(node->flags & INODE_TYPE)
# define isFile(node)	(getType(node) == INODE_FILE)
# define isDir(node)	(getType(node) == INODE_DIR)

struct SuperBlock {
	int magic;
	int blockSize;
	int numBlocks;
	int numINodes;
	int numINodeBlocks;
	int numFreeBlocks;
	int numFreeINodes;
	BlockID filenameMap;
	BlockID firstINodeBlock;
	BlockID firstDataBlock;
	BlockID bitmapBlock;
};

# define SUPERBLOCK_MAGIC 0xEF53
# define validSuperBlock(block) (block->magic == SUPERBLOCK_MAGIC)
# define setValidSuperBlock(block) block->magic = SUPERBLOCK_MAGIC

# define NUM_OPEN_FILES 128

typedef struct {
	bool inUse;
	INodeID id;
	int index;
} FileHandle;

#endif
