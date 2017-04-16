
#ifndef __SFS_H_
# define __SFS_H_

typedef int INodeID;

typedef struct {
	int flags;
	time_t lastAccess, lastModify, lastChange;
	INodeID parent;
	INodeID direct[12];
	INodeID singleIndirect;
	INodeID doubleIndirect;
} INode;

#endif
