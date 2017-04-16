
#ifndef __SFS_H_
# define __SFS_H_

# include <stdint.h>

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
	int devSize;
	int numINodes;
	BlockID rootINode;
	BlockID filenameMap;
	BlockID iNodeStart;
	BlockID dataStart;
	BlockID bitmapBlock;
	int flags;
};

# define SUPERBLOCK_ALLOCATED 0x18
# define superblockInitialized(block) (block->flags & SUPERBLOCK_ALLOCATED == SUPERBLOCK_ALLOCATED)

#endif
