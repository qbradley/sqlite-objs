/*
** speedtest1_wrapper.c — Wrapper to run speedtest1 with azqlite VFS
*/

#include "sqlite3.h"
#include <stdio.h>

/* External VFS registration from azqlite */
extern int azqlite_vfs_register(void);

/* Include speedtest1 as a library */
#define main speedtest1_main
#include "speedtest1.c"
#undef main

int main(int argc, char **argv) {
  /* Register azqlite VFS as default */
  int rc = azqlite_vfs_register();
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Failed to register azqlite VFS: %d\n", rc);
    return 1;
  }
  
  /* Set azqlite as default VFS */
  sqlite3_vfs *pVfs = sqlite3_vfs_find("azqlite");
  if (!pVfs) {
    fprintf(stderr, "azqlite VFS not found\n");
    return 1;
  }
  sqlite3_vfs_register(pVfs, 1);
  
  /* Run speedtest1 */
  return speedtest1_main(argc, argv);
}
