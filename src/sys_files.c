#ifndef _WIN32
#  define _GNU_SOURCE       // dladdr
#  define _FILE_OFFSET_BITS 64
#endif

#include "sys_files.h"

#include <string.h>

static char const anchor = 0; // something whose address lies inside this library

#ifdef _WIN32
#  include <windows.h>

int sys_module_dir(char* out, size_t size)
{
  HMODULE module = NULL;
  if (!GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&anchor,
        &module))
    return 0;
  DWORD const n = GetModuleFileNameA(module, out, (DWORD)size);
  if (!n || n >= size) return 0;
  char*       slash = strrchr(out, '\\');
  char* const fwd   = strrchr(out, '/');
  if (fwd && (!slash || fwd > slash)) slash = fwd;
  if (!slash) return 0;
  *slash = 0;
  return 1;
}

int sys_list_dir(char const* path, sys_dir_cb cb, void* ctx)
{
  char pattern[MAX_PATH + 4];
  if (strlen(path) + 3 > sizeof(pattern)) return 0;
  strcpy(pattern, path);
  strcat(pattern, "\\*");
  WIN32_FIND_DATAA data;
  HANDLE const     h = FindFirstFileA(pattern, &data);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do
  {
    if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, "..")) continue;
    cb(data.cFileName, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx);
  } while (FindNextFileA(h, &data));
  FindClose(h);
  return 1;
}

int sys_set_clipboard(char const* text)
{
  // as UTF-16 with CR LF line ends; the console's characters are single bytes
  size_t chars = 1;
  for (char const* c = text; *c; ++c) chars += *c == '\n' ? 2 : 1;
  HGLOBAL const mem = GlobalAlloc(GMEM_MOVEABLE, chars * sizeof(WCHAR));
  if (!mem) return 0;
  WCHAR* out = GlobalLock(mem);
  if (!out)
  {
    GlobalFree(mem);
    return 0;
  }
  for (char const* c = text; *c; ++c)
  {
    if (*c == '\n') *out++ = L'\r';
    *out++ = (WCHAR)(unsigned char)*c;
  }
  *out = 0;
  GlobalUnlock(mem);

  // another program may hold the clipboard for a moment
  int opened = 0;
  for (int attempt = 0; attempt < 10 && !opened; ++attempt)
  {
    opened = OpenClipboard(NULL) != 0;
    if (!opened) Sleep(10);
  }
  if (!opened)
  {
    GlobalFree(mem);
    return 0;
  }
  int ok = 0;
  if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem)) ok = 1; // the system owns mem from here
  CloseClipboard();
  if (!ok) GlobalFree(mem);
  return ok;
}

int sys_seek(FILE* f, long long offset, int whence)
{
  return _fseeki64(f, offset, whence);
}

long long sys_tell(FILE* f)
{
  return _ftelli64(f);
}

#else
#  include <dirent.h>
#  include <dlfcn.h>
#  include <sys/stat.h>
#  include <sys/types.h>

int sys_module_dir(char* out, size_t size)
{
  Dl_info info;
  if (!dladdr(&anchor, &info) || !info.dli_fname) return 0;
  size_t const n = strlen(info.dli_fname);
  if (n >= size) return 0;
  memcpy(out, info.dli_fname, n + 1);
  char* const slash = strrchr(out, '/');
  if (!slash) return 0;
  *slash = 0;
  return 1;
}

int sys_list_dir(char const* path, sys_dir_cb cb, void* ctx)
{
  DIR* const d = opendir(path);
  if (!d) return 0;
  for (struct dirent* e; (e = readdir(d));)
  {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char full[4096];
    if (strlen(path) + strlen(e->d_name) + 2 > sizeof(full)) continue;
    strcpy(full, path);
    strcat(full, "/");
    strcat(full, e->d_name);
    struct stat st;
    cb(e->d_name, stat(full, &st) == 0 && S_ISDIR(st.st_mode), ctx);
  }
  closedir(d);
  return 1;
}

int sys_set_clipboard(char const* text)
{
  (void)text;
  return 0;
}

int sys_seek(FILE* f, long long offset, int whence)
{
  return fseeko(f, (off_t)offset, whence);
}

long long sys_tell(FILE* f)
{
  return (long long)ftello(f);
}
#endif
