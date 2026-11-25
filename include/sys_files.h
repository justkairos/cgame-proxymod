#ifndef SYS_FILES_H
#define SYS_FILES_H

#include <stddef.h>

/*
  The few things the proxymod does with the operating system directly instead of through the engine: finding its
  own folder and listing folders. The engine gives a cgame neither (no directory listing syscall, and the paths
  it runs from are cvars a VM may not read). No engine headers are involved here.
*/

// Folder this library was loaded from (the mod folder), without a trailing separator. Returns 0 on failure.
int sys_module_dir(char* out, size_t size);

// Calls cb for every entry of the folder `path` (not "." and ".."). Returns 0 if the folder can't be read.
typedef void (*sys_dir_cb)(char const* name, int is_dir, void* ctx);
int sys_list_dir(char const* path, sys_dir_cb cb, void* ctx);

// 64-bit file positions for archives larger than 2 GB
#include <stdio.h>
int       sys_seek(FILE* f, long long offset, int whence);
long long sys_tell(FILE* f);

#endif // SYS_FILES_H
