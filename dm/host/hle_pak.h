// The packed RomFS backend (hle_pak.c): one file holding the game's RomFS (dm/tools/romfs_pack.py), mapped read-only.
//   hle_fs.c mounts it wherever it used to mount the extracted directory, and branches to these calls for a path on such a mount.
#pragma once
#include <stdint.h>
#include <stddef.h>

// The Horizon ABI's directory entry (0x310 bytes), filled by hle_fs.c for host directories and by hle_pak.c for a packed RomFS
typedef struct { char name[0x301]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
enum { PAK_ENT_DIR = 0, PAK_ENT_FILE = 1 };

int pak_mount(const char *file);                                  // map a packed RomFS: mount index, or -1 (errno set); the mapping lives for the process
int pak_mounted(void);                                            // whether any packed RomFS is mapped
int pak_check(const char *file, const char *marker);              // is <file> a packed RomFS that contains <marker> (the profile's romfs_marker)?
int pak_resolve(int pak, const char *rel, uint32_t *entry);       // guest-relative path → entry index (0 found, -1 not found)
int pak_type(int pak, const char *rel);                           // PAK_ENT_DIR / PAK_ENT_FILE / -1
int64_t pak_size(int pak, const char *rel);                       // file size, or -1
int pak_open(int pak, const char *rel, uint64_t *h);              // 0 and a file handle, or -1
int pak_is_handle(uint64_t h);                                    // is this guest file handle a packed-RomFS file?
int64_t pak_handle_size(uint64_t h);                              // size of an open pak file, or -1
int pak_read(uint64_t h, int64_t off, void *buf, size_t n, size_t *got);
void pak_close(uint64_t h);
int64_t pak_list(int pak, const char *rel, int mode, DirectoryEntry *out, int64_t max);   // mode: 1 = directories, 2 = files (DIRMODE_* in hle_fs.c); returns the count
