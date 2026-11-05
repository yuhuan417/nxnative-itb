// The game's developer console (Into The Breach, Kaiju engine): the panel a three-finger swipe opens, played from a button by hle_touch.c. A typed line reaches the engine's
//   line handler (main+0x3e0150 region) together with a mode flag at main+0xf4232c: 0 = command mode (the engine's and the game's own commands; anything else gets a red
//   "Command unrecognized"), 1 = Lua mode: the line runs as Lua code through the engine's Lua runner (main+0x3db4bc: loadstring on the engine's lua_State, the pointer at
//   main+0xf5fd80, chunkname "line", then a call; a Lua error comes back as text and the console prints it red). The PC build switches modes with the tab key; the phone's
//   software keyboard has no tab, so the host writes the flag: a console the player opens (the ZL toggle or a plain swipe) opens in Lua mode — that is where command lines
//   like "xp 50" are typed — and the console_command macro (hle_touch.c) opens it in the mode its line needs: plain lines keep command mode so the built-in commands
//   (undoturn) keep working, and a line written "lua …" runs as Lua with the prefix stripped before it is answered.
//   In Lua mode the software keyboard's answer is rewritten from "money 100" into "money(100)" (dm_console_line_filter): Lua needs the parentheses, and the rewrite only
//   touches the identifier + integer shape, so any other Lua text (for/do, print(...)) passes unchanged. The "xp <n>" command is caught before that rewrite and handled by
//   the host (the pilots live in the profile, out of Lua's reach).
//   Fixed addresses hold only for the build they were found in (AGENTS.md); read off 1.2.88 by disassembly.
#include "hle.h"
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

static const struct { const char *build; uint32_t flag, runner, lua_state; } k_console[] = {
    { "ffd0078f805bf3dc13c281319095f8e7", 0xf4232c, 0x3db4bc, 0xf5fd80 },   // Into The Breach 1.2.88 (v589824)
};
#define NCONSOLE ((int)(sizeof k_console / sizeof k_console[0]))

static uint8_t *con_addr(int field_off, int *row_out) {
    for (int i = 0; i < g_nmod; i++)
        if (!strcmp(g_mod[i].name, "main"))
            for (int k = 0; k < NCONSOLE; k++)
                if (!strcmp(g_mod[i].build_id, k_console[k].build)) { if (row_out) *row_out = k; return g_mod[i].base + field_off; }
    return NULL;
}

// Force the console's mode (hle_touch.c when the console opens). Harmless before the console exists: the flag is a plain byte in main's bss.
void dm_console_lua_mode(int on) {
    uint8_t *p = con_addr(k_console[0].flag, NULL); if (!p) return;
    dm_guest_write_prepare(p, 1);
    __atomic_store_n(p, (uint8_t)(on != 0), __ATOMIC_RELAXED);
}

int dm_console_lua_on(void) {
    uint8_t *p = con_addr(k_console[0].flag, NULL); if (!p) return 0;
    return __atomic_load_n(p, __ATOMIC_RELAXED) != 0;
}

// Rewrite a software keyboard answer of the shape "xp 50" into "xp(50)" while the console is in Lua mode (the console's own line otherwise reaches Lua and is a syntax
//   error). The trailing newline (the terminator the macro appends, hle_touch.c) is kept as it is; returns 1 when the text was rewritten.
int dm_console_line_filter(char *text, size_t cap) {
    if (!text || !*text || !cap || !dm_console_lua_on()) return 0;
    size_t n = strlen(text), body = n;
    if (body && text[body - 1] == '\n') body--;                       // the terminator stays
    if (!body) return 0;
    const char *a = text, *e = text + body;
    if (!(*a == '_' || (*a >= 'a' && *a <= 'z') || (*a >= 'A' && *a <= 'Z'))) return 0;
    const char *w = a + 1;
    while (w < e && (*w == '_' || (*w >= 'a' && *w <= 'z') || (*w >= 'A' && *w <= 'Z') || (*w >= '0' && *w <= '9'))) w++;
    const char *q = w;                                                // at least one space or tab between name and argument
    while (q < e && (*q == ' ' || *q == '\t')) q++;
    if (q == w || q >= e) return 0;
    const char *d = q + (*q == '-' ? 1 : 0);                          // the argument: an optionally signed integer, nothing else
    if (d >= e) return 0;
    for (const char *t = d; t < e; t++) if (*t < '0' || *t > '9') return 0;
    size_t name = (size_t)(w - a), arg = (size_t)(e - q), tail = body < n ? 1 : 0;
    if (name + 1 + arg + 1 + tail + 1 > cap) return 0;
    memmove(text + name + 1, q, arg);                                 // dest sits at or before the source (the name plus '(' is never longer than the gap it replaces)
    text[name] = '(';
    text[name + 1 + arg] = ')';
    if (tail) text[name + 2 + arg] = '\n';
    text[name + 3 + arg] = 0;
    return 1;
}

// ---- Development probe (DM_LUA_PROBE="<lua code>", DM_LUA_PROBE_AT=<present count>, default 300) ----
// Runs one Lua line through the engine's runner from the present path, once, and logs what came back: the runner's output string carries a Lua error, so a probe ends in
//   error(tostring(...)) to hand a value to the log (the guest's stdout, where Lua's print writes, is not collected on iOS). Console-independent: no touch needed to reach
//   the game's Lua. Called from nvnQueuePresentTexture (hle_nvn.c) on the guest's main thread, the same thread the engine runs its own Lua on.
static void probe_str_set(void *s, const char *c) {
    // Fill a libc++ std::string the way this build's code reads it (first byte bit 0 = long, short size = first byte >> 1, data at +1 / +16)
    size_t n = strlen(c);
    memset(s, 0, 24);
    if (n < 23) { ((uint8_t *)s)[0] = (uint8_t)(n << 1); memcpy((char *)s + 1, c, n + 1); }
    else {
        char *b = malloc(n + 1); if (!b) return;
        memcpy(b, c, n + 1);
        ((uint64_t *)s)[0] = ((n + 1) << 1) | 1; ((uint64_t *)s)[1] = n; ((char **)s)[2] = b;
    }
}

void dm_console_probe_tick(void);
int dm_console_pilots_apply(int n);   // defined below the probe: the profile's pilot record walk (read-only when n = 0)

void dm_console_probe_tick(void) {
    static int state = -2;                                            // -2 unset, -1 no probe, 0 counting presents, 1 done
    static uint64_t presents, at;
    static char code[2048];
    if (state == 1 || state == -1) return;
    presents++;                                                       // called once per present
    if (state == -2) {
        state = -1;
        const char *e = getenv("DM_LUA_PROBE"); if (!e || !*e) return;
        snprintf(code, sizeof code, "%s", e);
        const char *a = getenv("DM_LUA_PROBE_AT"); at = a ? strtoull(a, 0, 0) : 300;
        state = 0;
        LOG("console: DM_LUA_PROBE set, fires at present %llu: %s", (unsigned long long)at, code);
        return;
    }
    if (presents < at) return;
    state = 1;                                                        // one attempt: if Lua is up the line runs, if not the log says nothing and the next launch repeats
    int row = 0;
    uint64_t *lsp = (uint64_t *)con_addr(k_console[0].lua_state, &row);
    uint8_t *rb = con_addr(k_console[0].runner, NULL);
    if (!lsp || !rb) { LOG("console: probe: no main module with this build ID"); return; }
    uint64_t ls = __atomic_load_n(lsp, __ATOMIC_RELAXED);
    if (!ls) { LOG("console: probe: the engine's lua_State is not up at present %llu (raise DM_LUA_PROBE_AT)", (unsigned long long)presents); return; }
    char out[24] = {0}, cs[24] = {0};
    probe_str_set(out, "");
    probe_str_set(cs, code);
    int (*run)(void *, const void *) = (void *)rb;                    // rb = main + runner: the engine's RunLuaString(std::string& out, const std::string& code)
    int r = run(out, cs);
    size_t ol; const char *od;
    if (((uint8_t *)out)[0] & 1) { ol = ((uint64_t *)out)[1]; od = ((char **)out)[2]; }
    else { ol = ((uint8_t *)out)[0] >> 1; od = (char *)out + 1; }
    LOG("console: probe returned %d, output (%zu bytes): %.*s", r, ol, (int)(ol > 600 ? 600 : ol), od);
    if (getenv("DM_XP_PROBE")) dm_console_pilots_apply(0);            // report the profile's pilot records next to the Lua probe (read-only)
}

// ---- The run's pilots (the `xp` command's target) ----
// The profile comes from the engine's Profiles::GetProfile (main+0x51e800: takes a std::string, fills it with the resolved profile name and returns the profile
//   object), called here the same way the development probe calls the Lua runner. The current run's data is inline at profile+0x170; its +0x50 holds three pointers
//   to the run's Pilot objects (the current writer at main+0x626610 reads them for pilot0..2). The layout is the Pilot writer's (main+0x5b7378): exp +0x48, level
//   +0x8c, id +0xa8, name +0x28, skills [pilot+0x100]+0x70/+0xe8. Each record is validated before anything is written: a wrong layout must fail safe (log + no
//   write), not corrupt the profile. Fixed addresses hold only for this build (AGENTS.md).
static long con_read_str(uint8_t *p, char *out, size_t cap);          // defined below: one libc++ std::string into a host buffer

static uint8_t *con_profile(void) {
    static const struct { uint32_t get_profile; } k_gp[] = { { 0x51e800 } };   // Profiles::GetProfile(main+0x51e800): fills the name string, returns the profile object
    uint8_t *pb = con_addr(k_gp[0].get_profile, NULL);
    if (!pb) return NULL;
    char name[24] = {0};                                              // an empty libc++ string; GetProfile fills in the profile's name
    void *(*getp)(void *) = (void *)pb;
    uint8_t *p = getp(name);
    char shown[64]; long nl = con_read_str(name, shown, sizeof shown); // the resolved profile name tells an empty result apart from an unselected profile
    LOG("console: xp: profile %p, name '%s' (%ld)", p, nl >= 0 ? shown : "?", nl);
    return p;
}

// Read one libc++ std::string (the layout this build's code uses) into a host buffer; returns its length, or -1 when it does not look like a string.
// Every byte comes through con_rd (declared below): the header, and in the long form also the data itself — a garbage data pointer must not reach memcpy
static int con_rd(const void *a, void *out, size_t n);

static long con_read_str(uint8_t *p, char *out, size_t cap) {
    uint8_t hdr[24];
    if (!con_rd(p, hdr, sizeof hdr)) return -1;
    if (hdr[0] & 1) {                                                 // long form: size at +8, data at +0x10
        uint64_t n; uint8_t *d;
        memcpy(&n, hdr + 8, sizeof n); memcpy(&d, hdr + 0x10, sizeof d);
        if (n >= 0x1000 || (uintptr_t)d < 0x1000) return -1;
        if (n + 1 > cap) n = cap - 1;
        if (!con_rd(d, out, n)) return -1;
        out[n] = 0;
        return (long)n;
    }
    uint64_t n = hdr[0] >> 1;                                         // short form: size = first byte >> 1, data at +1
    if (n >= 23) return -1;
    if (n + 1 > cap) n = cap - 1;
    memcpy(out, hdr + 1, n); out[n] = 0;
    return (long)n;
}

// Every dereference goes through vm_read_overwrite (as hle_fpslock.c reads): a wrong layout or a wild pointer fails the walk and writes nothing. Off Apple (the unit
//   test host) there is no vm map — the tests disable the walk instead, so con_rd failing is the right default
static int con_rd(const void *a, void *out, size_t n) {
#if defined(__APPLE__)
    vm_size_t got = 0;
    return (uintptr_t)a >= 0x1000 && vm_read_overwrite(mach_task_self(), (vm_address_t)(uintptr_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
#else
    (void)a; (void)out; (void)n; return 0;
#endif
}

// Development dump (DM_XP_DUMP=1): the current run's inline data and the objects its pointers reach — shows where the pilots moved if a future build changes the layout
static void con_profile_dump(uint8_t *prof) {
    {   // the current run's data lives inline at profile+0x170 (the profile writer calls 0x626518 with that address); the pilots start at current+0x50
        uint8_t raw[0x300];
        if (con_rd(prof + 0x170, raw, sizeof raw)) {
            LOG("console: xp: current data @%p (profile+0x170), pilots at +0x50 = %p", (void *)(prof + 0x170), (void *)(prof + 0x1c0));
            for (size_t j = 0; j < sizeof raw; j += 16) {
                char hex[3 * 16 + 1], asc[17]; int hp = 0, ap = 0;
                for (size_t k = 0; k < 16; k++) { uint8_t b = raw[j + k]; hp += snprintf(hex + hp, sizeof hex - hp, "%02x ", b); asc[ap++] = (b >= 32 && b < 127) ? (char)b : '.'; }
                asc[16] = 0;
                LOG("console: xp:   +%#04zx  %s %s", 0x170 + j, hex, asc);
            }
        } else LOG("console: xp: current data at profile+0x170 is unreadable");
    }
    {   // the pilot-looking pointers inside current: dump the object each one points at
        static const uint32_t poffs[] = { 0x198, 0x1a0, 0x1a8, 0x1b0, 0x1b8, 0x1c0, 0x1c8, 0x1d0, 0x1d8, 0x1e0, 0x1e8, 0x1f0 };
        for (unsigned k = 0; k < sizeof poffs / sizeof poffs[0]; k++) {
            uint8_t *c = NULL;
            if (!con_rd(prof + poffs[k], &c, sizeof c) || !c || (uintptr_t)c < 0x1000) continue;
            uint8_t raw[0x100];
            if (!con_rd(c, raw, sizeof raw)) { LOG("console: xp: current+%#x -> %p (unreadable)", poffs[k], c); continue; }
            LOG("console: xp: current+%#x -> %p", poffs[k], c);
            for (size_t j = 0; j < sizeof raw; j += 16) {
                char hex[3 * 16 + 1], asc[17]; int hp = 0, ap = 0;
                for (size_t q = 0; q < 16; q++) { uint8_t b = raw[j + q]; hp += snprintf(hex + hp, sizeof hex - hp, "%02x ", b); asc[ap++] = (b >= 32 && b < 127) ? (char)b : '.'; }
                asc[16] = 0;
                LOG("console: xp:     +%#04zx  %s %s", j, hex, asc);
            }
        }
    }
    static const uint32_t offs[] = { 0x38, 0x40, 0x98, 0xb0, 0xb8, 0xd0, 0xd8, 0x150, 0x160, 0x168, 0x170, 0x178, 0x180, 0x188, 0x190, 0x1f8, 0x210, 0x228, 0x230, 0x240, 0x248, 0x250, 0x280, 0x288 };
    for (unsigned k = 0; k < sizeof offs / sizeof offs[0]; k++) {
        uint8_t *c = NULL;
        if (!con_rd(prof + offs[k], &c, sizeof c) || !c) continue;
        uint64_t w[3] = {0};
        if (!con_rd(c, w, sizeof w)) { LOG("console: xp: profile+%#x = %p (unreadable)", offs[k], c); continue; }
        LOG("console: xp: profile+%#x = %p [%#llx %#llx %#llx]", offs[k], c, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2]);
        uint8_t *node = NULL;
        if (!con_rd(c + 0x10, &node, sizeof node)) continue;
        for (int i = 0; node && i < 4; i++) {
            char key[48] = ""; long kl = con_read_str(node + 0x10, key, sizeof key);
            uint32_t v = 0; con_rd(node + 0x38, &v, sizeof v);
            uint8_t *next = NULL; con_rd(node, &next, sizeof next);
            LOG("console: xp:   +%#x node %d: '%s' (key %ld) int@+0x38=%u", offs[k], i, kl >= 0 ? key : "?", kl, v);
            node = next;
        }
    }
}

// The run's Pilot object i: the current run's data is inline at profile+0x170, and its +0x50 is an array of three pointers to the Pilot objects, 0x10 apart
//   (the current writer at main+0x626610 reads exactly [current+0x50] -> [0], [0x10], [0x20] to serialize pilot0..2).
static uint8_t *con_pilot_ptr(uint8_t *prof, int i) {
    uint8_t *arr = NULL, *p = NULL;
    if (!con_rd(prof + 0x170 + 0x50, &arr, sizeof arr) || !arr) return NULL;
    if (!con_rd(arr + (size_t)i * 0x10, &p, sizeof p) || !p) return NULL;
    return p;
}

// Walk the run's three pilot records and add n to each one's exp (n = 0: report only). The layout is the Pilot writer's (main+0x5b7378): exp is the int at +0x48,
//   level at +0x8c, id is the string at +0xa8, name the string at +0x28, and the skill pair lives in the object at [pilot+0x100] (+0x70 skill1, +0xe8 skill2).
//   Every dereference goes through con_rd, and the whole walk fails safe (log + no write) when one pilot does not validate.
int dm_console_pilots_apply(int n) {
    static int no_walk = -1;                                          // DM_TEST_NO_XP_WALK: the unit tests run no engine code (the fake module is data only)
    if (no_walk < 0) no_walk = getenv("DM_TEST_NO_XP_WALK") != NULL;
    if (no_walk) { LOG("console: xp: walk disabled (DM_TEST_NO_XP_WALK)"); return -1; }
    uint8_t *prof = con_profile();
    if (!prof) { LOG("console: xp: the profile is not up yet"); return -1; }
    if (getenv("DM_XP_DUMP")) con_profile_dump(prof);                 // development: the profile/current pointers, if a future build moves them
    uint8_t *addxp = con_addr(0x5b596c, NULL);                        // Pilot::AddXP(this, int): the game's own xp/level-up path (main+0x5b596c)
    if (n && !addxp) { LOG("console: xp: no Pilot::AddXP with this build ID"); return -1; }
    int cnt = 0, ok = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t *p = con_pilot_ptr(prof, i);
        if (!p) { LOG("console: xp: pilot %d has no record -- no write", i); return -1; }
        char id[64] = "", name[64] = "";
        int32_t exp = 0, level = 0, sk1 = 0, sk2 = 0; uint8_t *sk = NULL;
        long il = con_read_str(p + 0xa8, id, sizeof id);
        long nl = con_read_str(p + 0x28, name, sizeof name);
        if (il <= 0 || nl < 0 || !con_rd(p + 0x48, &exp, sizeof exp) || !con_rd(p + 0x8c, &level, sizeof level) ||
            !con_rd(p + 0x100, &sk, sizeof sk) || !sk ||
            !con_rd(sk + 0x70, &sk1, sizeof sk1) || !con_rd(sk + 0xe8, &sk2, sizeof sk2) ||
            exp < 0 || exp > 1000000) {
            LOG("console: xp: pilot %d failed validation (%s / %s, exp %d, level %d, skills %p) -- no write", i, id, name, exp, level, (void *)sk);
            return -1;
        }
        LOG("console: xp: pilot %d '%s' (%s) exp %d level %d skill %d/%d%s", i, id, name, exp, level, sk1, sk2, n ? "" : " (report only)");
        if (n) {
            // Pilot::AddXP adds at most the current level's remaining XP and promotes once per call when the bar fills, so feed it one point at a
            //   time: that is how the game itself grants XP (the victory path loops it), and it carries the rest of a 50/100 command into the next level.
            int steps = n < 0 ? -n : n, dir = n < 0 ? -1 : 1;
            if (steps > 10000) steps = 10000;
            int (*add_xp)(void *, int) = (void *)addxp;
            for (int k = 0; k < steps; k++) add_xp(p, dir);
            int32_t exp2 = 0, level2 = 0;
            if (con_rd(p + 0x48, &exp2, sizeof exp2) && con_rd(p + 0x8c, &level2, sizeof level2))
                LOG("console: xp: pilot %d -> exp %d level %d", i, exp2, level2);
            ok++;
        }
        cnt++;
    }
    LOG("console: xp: %d pilot(s)%s", cnt, n ? ", applied" : "");
    return n ? ok : cnt;
}

// The host's "xp <n>" command (the pilots are not reachable from the console's Lua mode): the run's three pilot records get n experience each, and the answer becomes
//   a silent Lua comment, so nothing else executes; the details go to the log. DM_XP_REPORT=1 only reports (no write), for checking the layout. Returns 1 when the
//   answer was replaced.
static int xp_hook(char *text, size_t cap) {
    const char *a = text, *e = text + strlen(text);
    if (e > a && e[-1] == '\n') e--;                                  // the terminator the macro appends (hle_touch.c)
    a += 2;                                                           // the caller matched "xp"
    while (a < e && (*a == ' ' || *a == '\t')) a++;
    int sign = 1; if (a < e && (*a == '-' || *a == '+')) { sign = *a++ == '-' ? -1 : 1; }
    if (a >= e) return 0;
    long n = 0;
    for (const char *t = a; t < e; t++) { if (*t < '0' || *t > '9') return 0; n = n * 10 + (*t - '0'); if (n > 1000000) return 0; }
    n *= sign;
    const char *r = getenv("DM_XP_REPORT");
    int report = r && *r && *r != '0';
    int applied = dm_console_pilots_apply(report ? 0 : (int)n);
    dm_console_lua_mode(1);                                           // the comment below evaluates harmlessly only in Lua mode
    if (applied < 0) snprintf(text, cap, "-- xp %+ld: profile not readable (see log.txt)\n", n);
    else snprintf(text, cap, "-- xp %+ld: %d pilot(s) %s (see log.txt)\n", n, applied, report ? "reported" : "updated");
    return 1;
}

// ---- The pilot skill command ----
// "skill" lists the current run's pilots and their two chosen skills ("skill"), or sets them: "skill <pilot> <skill1> [skill2]". <pilot> is 1..3, "all", a
//   pilot id (Pilot_Rust) or a case-insensitive name fragment (Rust, Philip); a skill is 0..3 or health/move/grid/reactor (the four level-up skills), and "-"
//   keeps the current one. The pilot's two skill ints live in the object at [pilot+0x100], at +0x70 and +0xe8 (the Pilot writer, main+0x5b7378, reads them there).
//   Writing them is a save edit: the hangar and the next save read the same fields. Details go to the log; the console answer is a silent Lua comment.
static const char *skill_name(int v) {
    static const char *const names[] = { "health", "move", "grid", "reactor" };
    return v >= 0 && v < 4 ? names[v] : "?";
}
static int skill_value(const char *s) {
    if (!strcasecmp(s, "-") || !strcasecmp(s, "keep")) return -1;
    if (s[0] >= '0' && s[0] <= '3' && !s[1]) return s[0] - '0';
    if (!strcasecmp(s, "health")) return 0;
    if (!strcasecmp(s, "move")) return 1;
    if (!strcasecmp(s, "grid")) return 2;
    if (!strcasecmp(s, "reactor")) return 3;
    return -2;
}
static int con_skill_get(uint8_t *p, int which, int *out) {
    uint8_t *sk = NULL; uint32_t v = 0;
    if (!con_rd(p + 0x100, &sk, sizeof sk) || !sk || !con_rd(sk + (which ? 0xe8 : 0x70), &v, sizeof v)) return -1;
    *out = (int)v;
    return 0;
}
static int con_skill_set(uint8_t *p, int which, int v) {
    uint8_t *sk = NULL;
    if (!con_rd(p + 0x100, &sk, sizeof sk) || !sk) return -1;
    uint32_t *q = (uint32_t *)(sk + (which ? 0xe8 : 0x70));
    dm_guest_write_prepare(q, sizeof *q);
    __atomic_store_n(q, (uint32_t)v, __ATOMIC_RELAXED);
    return 0;
}
static int ci_contains(const char *h, const char *n) {
    if (!*n) return 1;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}
// 1 = one pilot in *idx, 3 = "all" (*idx = -1), 0 = no match, >1 = that many ambiguous matches
static int pilot_find(uint8_t *prof, const char *tok, int *idx) {
    if (!strcasecmp(tok, "all")) { *idx = -1; return 3; }
    if (tok[0] >= '1' && tok[0] <= '3' && !tok[1]) { *idx = tok[0] - '1'; return 1; }
    int hit = -1, count = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t *p = con_pilot_ptr(prof, i);
        if (!p) continue;
        char id[64] = "", name[64] = "";
        con_read_str(p + 0xa8, id, sizeof id);
        con_read_str(p + 0x28, name, sizeof name);
        if (!strcasecmp(tok, id) || !strcasecmp(tok, name) || ci_contains(id, tok) || ci_contains(name, tok)) { hit = i; count++; }
    }
    if (count == 1) *idx = hit;
    return count;
}
static int skill_hook(char *text, size_t cap) {
    const char *a = text, *e = text + strlen(text);
    if (e > a && e[-1] == '\n') e--;
    if (strncmp(a, "skill", 5) || (a + 5 < e && a[5] != ' ' && a[5] != '\t')) return 0;
    a += 5;
    char arg[4][96]; int nt = 0;
    while (a < e && nt < 4) {
        while (a < e && (*a == ' ' || *a == '\t')) a++;
        if (a >= e) break;
        int n = 0;
        while (a < e && *a != ' ' && *a != '\t' && n < 95) arg[nt][n++] = *a++;
        arg[nt][n] = 0; nt++;
    }
    uint8_t *prof = con_profile();
    if (!prof) { snprintf(text, cap, "-- skill: profile not readable (see log.txt)\n"); dm_console_lua_mode(1); return 1; }
    if (nt > 3 || (nt >= 2 && (skill_value(arg[1]) == -2 || (nt == 3 && skill_value(arg[2]) == -2)))) {
        snprintf(text, cap, "-- skill <pilot> <skill1> [skill2] (0..3 or health/move/grid/reactor, - = keep)\n");
        dm_console_lua_mode(1); return 1;
    }
    int idx = -1, count = nt ? pilot_find(prof, arg[0], &idx) : 3;
    if (count != 1 && count != 3) {
        snprintf(text, cap, "-- skill: '%s' matches %d pilot(s); use 1..3 or all (see log.txt)\n", nt ? arg[0] : "", count);
        dm_console_lua_mode(1); return 1;
    }
    int targets[3], ntargets = count == 3 ? 3 : 1;
    for (int i = 0; i < ntargets; i++) targets[i] = count == 3 ? i : idx;
    int v1 = nt >= 2 ? skill_value(arg[1]) : -1;
    int v2 = nt >= 3 ? skill_value(arg[2]) : -1;
    int changed = 0;
    for (int i = 0; i < ntargets; i++) {
        uint8_t *p = con_pilot_ptr(prof, targets[i]);
        char id[64] = "", name[64] = "";
        int c1 = -1, c2 = -1;
        if (!p || con_read_str(p + 0xa8, id, sizeof id) <= 0 || con_skill_get(p, 0, &c1) || con_skill_get(p, 1, &c2)) {
            LOG("console: skill: pilot %d is unreadable -- no write", targets[i]);
            snprintf(text, cap, "-- skill: pilot %d unreadable (see log.txt)\n", targets[i]);
            dm_console_lua_mode(1); return 1;
        }
        con_read_str(p + 0x28, name, sizeof name);
        int n1 = v1 < 0 ? c1 : v1, n2 = v2 < 0 ? c2 : v2;
        if (v1 >= 0 && con_skill_set(p, 0, n1)) { snprintf(text, cap, "-- skill: pilot %d unreadable (see log.txt)\n", targets[i]); dm_console_lua_mode(1); return 1; }
        if (v2 >= 0 && con_skill_set(p, 1, n2)) { snprintf(text, cap, "-- skill: pilot %d unreadable (see log.txt)\n", targets[i]); dm_console_lua_mode(1); return 1; }
        LOG("console: skill: pilot %d '%s' (%s) skill1 %d(%s) -> %d(%s), skill2 %d(%s) -> %d(%s)%s",
            targets[i], id, name, c1, skill_name(c1), n1, skill_name(n1), c2, skill_name(c2), n2, skill_name(n2),
            (v1 >= 0 || v2 >= 0) ? "" : " (report only)");
        changed += (v1 >= 0 || v2 >= 0);
    }
    if (nt == 0 || (nt == 1 && count == 1)) snprintf(text, cap, "-- skill: %d pilot(s) listed (see log.txt)\n", ntargets);
    else snprintf(text, cap, "-- skill: %d pilot(s) %s (see log.txt)\n", ntargets, changed ? "updated" : "listed");
    dm_console_lua_mode(1);
    return 1;
}

// The software keyboard's answer hook (hle_swkbd.c): the host's "xp <n>" command first, then the generic Lua-mode rewrite for everything else
int dm_console_answer_hook(char *text, size_t cap) {
    if (!text || !*text || !cap) return 0;
    const char *a = text;
    if (a[0] == 'x' && a[1] == 'p' && (a[2] == ' ' || a[2] == '\t' || a[2] == '-')) {
        if (xp_hook(text, cap)) return 1;                             // the command: the answer is the silent comment now
        return 0;                                                     // "xp <not a number>": not the command, leave the line as it is
    }
    if (a[0] == 's' && skill_hook(text, cap)) return 1;               // "skill ..." (the hook checks the whole prefix itself)
    return dm_console_line_filter(text, cap);
}
