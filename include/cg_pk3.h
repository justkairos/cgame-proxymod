#ifndef CG_PK3_H
#define CG_PK3_H

#include "q_shared.h"

/*
  Walks everything the game can load files from: the pk3 archives and the loose files of the mod folder and of
  the base game folder (see cg_pk3.c for how they are found).

  For every file and every folder, cb gets
    source  where it is: "defrag/zzz-maps.pk3", or "defrag/" for a loose file
    path    path inside that pk3 / folder, with forward slashes: "maps/foo.bsp", "textures/foo"
  Return qfalse from cb to stop. pk3_scan returns the number of pk3 archives read, or -1 if the game folders
  could not be found.
*/
typedef qboolean (*pk3_entry_cb)(char const* source, char const* path, qboolean is_folder, void* ctx);
int32_t pk3_scan(pk3_entry_cb cb, void* ctx);

// pk3search / pk3files / pk3folders <pattern> ...: which of the two lists is printed depends on argv(0)
void cmd_pk3search(void);

#endif // CG_PK3_H
