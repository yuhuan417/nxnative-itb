// The game's RomFS as one packed file (dm/tools/romfs_pack.py): a read-only backend for the nn::fs take-over in hle_fs.c.
//   The IPA used to carry the RomFS as 600-odd files in the app bundle, so every install moved the whole game again even when only the host's code had changed. The packed file is
//   one entry to copy (to Documents, once per game version) and one mmap here: a program-only update is a small IPA and the data stays where it is.
// The format is documented in dm/tools/romfs_pack.py. Entries are in depth-first order, so a directory's children are found by walking first_child / next_sibling; lookups take
//   the path component by component, exactly as hle_fs.c does for a host directory, and a component that does not match exactly falls back to a case-insensitive match (Horizon
//   paths are case-sensitive, but games do not always agree with their own file names, which is why the host directory backend carries guest names in an xattr).
// Handles: file handles are 64-bit values the guest passes back unchanged, so a pak file's handle carries the mount and the entry; hle_fs.c tells them apart with pak_is_handle().
#include "hle.h"
#include "hle_pak.h"
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>

#define PAK_MAGIC "DMPAK1\n"
#define PAK_VERSION 1u
enum { HDR_SIZE = 88, ENT_SIZE = 48, ROOT = 0xFFFFFFFFu };
#define PAK_HMAGIC 0xDA7A000000000000ull
#define PAK_HMASK  0xFFFF000000000000ull
#define MAX_PAK 4

typedef struct {
    char path[1024];
    int fd;
    const uint8_t *base;
    size_t len;
    uint32_t n;
    const uint8_t *toc;
    const char *names;
} pak_t;
static pak_t g_pak[MAX_PAK];
static int g_npak;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static const uint8_t *ent_base(const pak_t *p, uint32_t i) { return p->toc + (size_t)i * ENT_SIZE; }
static const char *ent_name(const pak_t *p, uint32_t i) { return p->names + rd32(ent_base(p, i)); }
static uint32_t ent_name_len(const pak_t *p, uint32_t i) { return ent_base(p, i)[4] | (uint32_t)ent_base(p, i)[5] << 8; }
static int ent_type(const pak_t *p, uint32_t i) { return ent_base(p, i)[6]; }
static uint64_t ent_off(const pak_t *p, uint32_t i) { return rd64(ent_base(p, i) + 8); }
static uint32_t ent_len(const pak_t *p, uint32_t i) { return rd32(ent_base(p, i) + 0x10); }
static uint32_t ent_child(const pak_t *p, uint32_t i) { return rd32(ent_base(p, i) + 0x14); }
static uint32_t ent_next(const pak_t *p, uint32_t i) { return rd32(ent_base(p, i) + 0x18); }

// Open (mmap) a packed RomFS. Returns the mount index, or -1 with errno set. The file stays mapped for the life of the process, as a mount does on the device.
int pak_mount(const char *file) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_npak; i++) if (!strcmp(g_pak[i].path, file)) { pthread_mutex_unlock(&g_lock); return i; }   // one mapping per file: the boot check maps it before MountRom does
    if (g_npak >= MAX_PAK) { pthread_mutex_unlock(&g_lock); errno = ENOSPC; return -1; }
    int fd = open(file, O_RDONLY);
    if (fd < 0) { pthread_mutex_unlock(&g_lock); return -1; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size < HDR_SIZE) { close(fd); pthread_mutex_unlock(&g_lock); errno = EINVAL; return -1; }
    uint8_t *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) { close(fd); pthread_mutex_unlock(&g_lock); return -1; }
    uint32_t n = rd32(base + 0x10);
    uint64_t toc_off = rd64(base + 0x18), names_off = rd64(base + 0x20), names_len = rd64(base + 0x28), total = rd64(base + 0x40);
    int bad = memcmp(base, PAK_MAGIC, 7) || rd32(base + 8) != PAK_VERSION || !n || total != (uint64_t)st.st_size ||
              toc_off + (uint64_t)n * ENT_SIZE > names_off || names_off + names_len > (uint64_t)st.st_size;
    if (bad) { munmap(base, (size_t)st.st_size); close(fd); pthread_mutex_unlock(&g_lock); errno = EINVAL; return -1; }
    pak_t *p = &g_pak[g_npak];
    memset(p, 0, sizeof *p);
    strlcpy(p->path, file, sizeof p->path);
    p->fd = fd; p->base = base; p->len = (size_t)st.st_size; p->n = n; p->toc = base + toc_off; p->names = (const char *)base + names_off;
    int idx = g_npak++;
    pthread_mutex_unlock(&g_lock);
    LOG("pak: %s mapped (%zu MiB, %u entries) as mount %d", file, p->len >> 20, n, idx);
    return idx;
}

int pak_mounted(void) { return g_npak > 0; }

// Is this path a packed RomFS that contains <marker> (the profile's romfs_marker)? Used at boot to pick the data file without looking at its name.
int pak_check(const char *file, const char *marker) {
    int idx = pak_mount(file);
    if (idx < 0) return 0;
    uint32_t e;
    int found = pak_resolve(idx, marker, &e) == 0 && ent_type(&g_pak[idx], e) == PAK_ENT_FILE;
    return found;
}

// The path's length per component; "//", "." and a leading "/" are skipped. Returns -1 for ".." (the caller normalizes those away first).
static int next_comp(const char **pp, char *out, size_t n) {
    const char *p = *pp;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) { *pp = p; return 0; }
        const char *end = strchr(p, '/');
        size_t k = end ? (size_t)(end - p) : strlen(p);
        if (k == 1 && *p == '.') { p += 1; continue; }
        if (k == 2 && p[0] == '.' && p[1] == '.') return -1;
        if (k >= n) return -2;
        memcpy(out, p, k); out[k] = 0;
        *pp = end ? end : p + k;
        return 1;
    }
}

// Resolve a guest-relative path ("resources/resource.dat", no "name:" prefix) to an entry index: 0 found, -1 not found.
int pak_resolve(int idx, const char *rel, uint32_t *entry) {
    if (idx < 0 || idx >= g_npak) return -1;
    const pak_t *p = &g_pak[idx];
    uint32_t cur = 0;                      // the root directory
    char comp[0x301]; const char *q = rel;
    int r;
    while ((r = next_comp(&q, comp, sizeof comp)) == 1) {
        uint32_t c = ent_child(p, cur), ci = ROOT;
        while (c != ROOT) {
            uint32_t l = ent_name_len(p, c);
            if (l == strlen(comp) && !memcmp(ent_name(p, c), comp, l)) { ci = c; break; }
            if (ci == ROOT && l == strlen(comp) && !strncasecmp(ent_name(p, c), comp, l)) ci = c;   // a case-insensitive match, used only if no exact one turns up
            c = ent_next(p, c);
        }
        if (ci == ROOT) return -1;
        cur = ci;
    }
    if (r < 0) return -1;
    *entry = cur;
    return 0;
}

int pak_type(int idx, const char *rel) {
    uint32_t e;
    if (pak_resolve(idx, rel, &e)) return -1;
    return ent_type(&g_pak[idx], e) == PAK_ENT_DIR ? PAK_ENT_DIR : PAK_ENT_FILE;
}

int64_t pak_size(int idx, const char *rel) {
    uint32_t e;
    if (pak_resolve(idx, rel, &e) || ent_type(&g_pak[idx], e) != PAK_ENT_FILE) return -1;
    return (int64_t)ent_len(&g_pak[idx], e);
}

int pak_open(int idx, const char *rel, uint64_t *h) {
    uint32_t e;
    if (pak_resolve(idx, rel, &e) || ent_type(&g_pak[idx], e) != PAK_ENT_FILE) return -1;
    *h = PAK_HMAGIC | ((uint64_t)(uint32_t)idx << 32) | e;
    return 0;
}

int pak_is_handle(uint64_t h) { return (h & PAK_HMASK) == PAK_HMAGIC; }
static int h_idx(uint64_t h) { return (int)((h >> 32) & 0xFFFFu); }        // the mount: the top 16 bits are the magic, so the fields must be masked out of it
static uint32_t h_ent(uint64_t h) { return (uint32_t)(h & 0xFFFFFFFFu); }

int64_t pak_handle_size(uint64_t h) {
    int idx = h_idx(h); uint32_t e = h_ent(h);
    if (!pak_is_handle(h) || idx < 0 || idx >= g_npak || e >= g_pak[idx].n) return -1;
    return (int64_t)ent_len(&g_pak[idx], e);
}

int pak_read(uint64_t h, int64_t off, void *buf, size_t n, size_t *got) {
    int idx = h_idx(h); uint32_t e = h_ent(h);
    if (!pak_is_handle(h) || idx < 0 || idx >= g_npak || e >= g_pak[idx].n) return -1;
    const pak_t *p = &g_pak[idx];
    uint64_t foff = ent_off(p, e), flen = ent_len(p, e);
    if (off < 0 || (uint64_t)off > flen) { *got = 0; return 0; }
    size_t k = flen - (uint64_t)off;
    if (k > n) k = n;
    if (foff + flen > p->len) return -1;
    memcpy(buf, p->base + foff + (uint64_t)off, k);
    *got = k;
    return 0;
}

void pak_close(uint64_t h) { (void)h; }   // stateless: the entry table holds everything an open file needs

// List a directory: entries of the requested kinds (DIRMODE_DIR / DIRMODE_FILE as hle_fs.c passes), up to max, returning the number matched (so a count query passes NULL, 0).
int64_t pak_list(int idx, const char *rel, int mode, DirectoryEntry *out, int64_t max) {
    uint32_t e;
    if (pak_resolve(idx, rel, &e) || ent_type(&g_pak[idx], e) != PAK_ENT_DIR) return -1;
    const pak_t *p = &g_pak[idx];
    int64_t n = 0;
    for (uint32_t c = ent_child(p, e); c != ROOT; c = ent_next(p, c)) {
        int is_dir = ent_type(p, c) == PAK_ENT_DIR;
        if (!(mode & (is_dir ? 1 : 2))) continue;   // DIRMODE_DIR = 1, DIRMODE_FILE = 2
        if (out && n < max) {
            DirectoryEntry *o = &out[n];
            memset(o, 0, sizeof *o);
            uint32_t l = ent_name_len(p, c);
            if (l >= sizeof o->name) l = sizeof o->name - 1;
            memcpy(o->name, ent_name(p, c), l); o->name[l] = 0;
            o->type = is_dir ? PAK_ENT_DIR : PAK_ENT_FILE;
            o->size = is_dir ? 0 : (int64_t)ent_len(p, c);
        }
        n++;
    }
    return n;
}
