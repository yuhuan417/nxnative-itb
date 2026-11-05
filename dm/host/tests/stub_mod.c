// Test stand-in for the module table and the guest-write helper (loader.c / hle.c): one fake "main" module with the Into The Breach 1.2.88 build ID over a plain buffer,
//   so hle_console.c's fixed-address lookups succeed. The buffer is data only: the tests never execute engine code (the xp walk is disabled with DM_TEST_NO_XP_WALK,
//   the same pattern as the other DM_TEST_* switches)
#include "../dm.h"
#include <string.h>

static uint8_t fake_image[0xf50000];                                  // large enough for the console flag at base + 0xf4232c
module_t g_mod[MAX_MOD];
int g_nmod;

__attribute__((constructor)) static void stub_mod_init(void) {
    g_nmod = 1;
    g_mod[0] = (module_t){0};
    strcpy(g_mod[0].name, "main");
    strcpy(g_mod[0].build_id, "ffd0078f805bf3dc13c281319095f8e7");
    g_mod[0].base = fake_image;
}
void dm_guest_write_prepare(void *addr, size_t len) { (void)addr; (void)len; }
