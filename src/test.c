#include <stdio.h>
#include <time.h>
#include <fuse.h>

#include "sfs.h"

FILE *flatFile;

void *sfs_init(struct fuse_conn_info *conn)
{
	int nothing = 0;
    flatFile = fopen("flatfile.bin", "wrb");
    fseek(flatFile, TOTAL_SIZE - 1, SEEK_SET);
    fwrite((void *) &nothing, 1, 1, flatFile);
    fclose(flatFile);
    return (void *) 0;
}

int main(int argc, char *argv[]) {
	sfs_init((struct fuse_conn_info *) 0);
	printf("%ld\n", sizeof(struct SuperBlock));
}
