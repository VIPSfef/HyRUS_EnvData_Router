#ifndef MTR_TEST_WORKDIR_H
#define MTR_TEST_WORKDIR_H

/* Keep synthetic configuration files on a filesystem that enforces POSIX
 * permissions, even when the source/build tree lives on a Windows mount. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
typedef struct { int unused; } test_workdir;
static int test_workdir_enter(test_workdir *directory)
{ (void)directory; return 1; }
static int test_workdir_leave(test_workdir *directory)
{ (void)directory; return 1; }
#else
#include <fcntl.h>
#include <unistd.h>
typedef struct { int original_fd; char *path; } test_workdir;
static int test_workdir_enter(test_workdir *directory)
{
    static const char suffix[]="/mavlink-tests-XXXXXX";
    const char *base=getenv("TMPDIR");
    size_t bytes;
    if(!base || !base[0]) base="/tmp";
    directory->original_fd=-1; directory->path=NULL;
    bytes=strlen(base)+sizeof(suffix);
    directory->path=malloc(bytes);
    if(!directory->path) { fputs("Could not allocate temporary test path\n",stderr); return 0; }
    (void)snprintf(directory->path,bytes,"%s%s",base,suffix);
    directory->original_fd=open(".",O_RDONLY);
    if(directory->original_fd<0) { perror("Save test working directory"); goto failed; }
    if(!mkdtemp(directory->path)) { perror("Create temporary test directory"); goto failed; }
    if(chdir(directory->path)) {
        perror("Enter temporary test directory");
        if(rmdir(directory->path)) perror("Remove unused temporary test directory");
        goto failed;
    }
    return 1;
failed:
    if(directory->original_fd>=0 && close(directory->original_fd))
        perror("Close saved test directory");
    free(directory->path); directory->path=NULL; directory->original_fd=-1;
    return 0;
}
static int test_workdir_leave(test_workdir *directory)
{
    int restored=fchdir(directory->original_fd)==0,ok=restored;
    if(!restored) perror("Restore test working directory");
    if(close(directory->original_fd)) { perror("Close saved test directory"); ok=0; }
    if(restored && rmdir(directory->path)) {
        perror("Remove temporary test directory (test files remain)"); ok=0;
    }
    free(directory->path); directory->path=NULL; directory->original_fd=-1;
    return ok;
}
#endif
#endif
