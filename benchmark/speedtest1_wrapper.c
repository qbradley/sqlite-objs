/*
** speedtest1_wrapper.c — Wrapper to run speedtest1 with sqliteObjs VFS
*/

#include "sqlite3.h"
#include <stdio.h>

/* Public API for VFS registration */
#include "sqlite_objs.h"

/* Include speedtest1 as a library */
#define main speedtest1_main
#include "speedtest1.c"
#undef main

int main(int argc, char **argv) {
  /* Register sqliteObjs VFS as default */
  int rc = sqlite_objs_vfs_register(1);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Failed to register sqliteObjs VFS: %d\n", rc);
    return 1;
  }
  
  /* Run speedtest1 */
  return speedtest1_main(argc, argv);
}
