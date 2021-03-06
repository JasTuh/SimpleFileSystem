# SimpleFileSystem
Dan Snyder (dts85), Jason Tuhy (jst119), Viraj Patel (vcp25), 
CS 416 Assignment 3 ReadMe

EXTENSIONS AND IMPROVEMENTS:
We implemented directory support, extended directory operations and two levels of indirection as per the extensions and improvements section. 

TESTING:
Untar the file and cd into SimpleFileSystem and run ./configure then make then cd into example and mountdir is our mounted file system. 

Suggested tests:
1) In example run: 
cp -r SystemsAssignment2/ mountdir/ 
cd mountdir/
cd SystemsAssignment2/
make
./compressT_LOLS one_hundred_thousand_characters.txt 5

2) In example run
cp test.sh mountdir/
cd mountdir/
./test.sh test2.txt "hello this is a message" 50
Note: This is a simple bash script to generate a file and add a message to it a certain amount of times, the 
way this file is called is ./test.sh <name of file> <"message"> <count>

Also our thread library/memory manager is being used for compressT_LOLS (a systems assignment from last semester) but you can find
just our thread library/memory manager code in example/thread_library

SOURCE CODE:
The majority of our code is in SimpleFileSystem/src/sfs.c

DESIGN:
When designing our file system the first thing that we had to decide was how we were going to structure our INodes.
After much discussion we decided on the struct:
typedef struct {
	int flags, size, childCount;
	time_t lastAccess, lastModify, lastChange;
	INodeID blocks[14];
} INode;
We store 3 ints, flags being certain attributes of a file like if it is currently in use or if it is a directory.
We store 3 time stamps as that is what is done on unix systems we researched.
Lastly we store 14 pointers to blocks of our flat file.  The first 12 are direct
mapped pointers to the flat file (we split the flat file in to 4096 byte size blocks),
the 13th points to a single level indirect block of our flat file and the 14th pointer points to a double level indirect block of our flat file. 
This allows us to store a max file size of (since each block is 4096 and we refer to 
each INode with a 4 byte int id we can store 4096/4 = 1024 INodes per indirection block):
12 * 4096 + 1024*4096 + 1024*1024*4096 = 4,299,210,752 bytes or 4.3 GB, however we tested with a flat file that was 128 MB in size. 

We also designated our "superblock" to be the first block of the file which contains:
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
The magic is used to determine if our file system was initialized or not so if a user unmounts the file system and remounts later they will retain their files.
filenameMap is a Block of memory that we use to maintain filename to INode ID mappings.
firstINodeBlock & firstDataBlock represent the first block of the file that is used to store INodes & data respectively.
bitmapBlock is a block of the file used to mark which blocks and INodes are free.

The max file name length we allow is 124 characters, and directories use all 14 blocks directly which allows for our directories to have 448 files. 
(124 bytes for name, 4 bytes for INodeID = 128 bytes per file, 4096*14/128 = 448)
