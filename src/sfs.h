
#ifndef __SFS_H_
# define __SFS_H_

# include <stdint.h>

# define BLOCK_SIZE			4096
# define TOTAL_BLOCKS		32768
# define TOTAL_SIZE			(TOTAL_BLOCKS*BLOCK_SIZE)
# define NUM_INODE_BLOCKS	750
# define NUM_DATA_BLOCKS	(TOTAL_BLOCKS-NUM_INODE_BLOCKS)
# define NUM_INODES			(NUM_INODE_BLOCKS*BLOCK_SIZE/sizeof(INode))

typedef uint32_t INodeID;
typedef uint32_t BlockID;

typedef struct {
	int flags;
	time_t lastAccess, lastModify, lastChange;
	INodeID parent;
	INodeID direct[12];
	INodeID singleIndirect;
	INodeID doubleIndirect;
} INode;

# define INODE_IN_USE	0x1
# define INODE_TYPE		0x2
# define INODE_FILE		0x0
# define INODE_DIR		0x1

# define isInUse(node)	(node->flags & INODE_IN_USE)
# define getType(node)	((node->flags & INODE_TYPE) >> 1)
# define isFile(node)	(getType(node) == INODE_FILE)
# define isDir(node)	(getType(node) == INODE_DIR)

struct SuperBlock {
	int magic;
	int blockSize;
	int numBlocks;
	int numINodes;
	int numFreeBlocks;
	int numFreeINodes;
	BlockID filenameMap;
	BlockID firstINode;
	BlockID firstDataBlock;
	BlockID bitmapBlock;
};

# define SUPERBLOCK_MAGIC 0xEF53
# define validSuperBlock(block) (block->magic == SUPERBLOCK_MAGIC)
# define setValidSuperBlock(block) block->magic = SUPERBLOCK_MAGIC

#endif
