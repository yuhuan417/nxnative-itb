// Touchscreen: the front end pushes the whole set of touch points (dm_touch_set, front.h), and the game reads them every frame through nn::hid::GetTouchScreenState(s) (hle_hid.c → touch_fill_n).
// How the game reads (main 0x1b65260): it clears 16 states, asks for 16, and looks only at state 0 -- the touch count and each touch point's finger_id / x / y; the panel coordinate system is 1280x720
// (it writes the constant 0x2d0_00000500 = 720 << 32 | 1280 in the same function). Therefore:
//   · front-end normalized coordinates [0,1) (origin at the top left of the output picture) → x = nx·1280, y = ny·720, clamped to [0,1279] / [0,719];
//   · short-press latch: the game only looks at the newest state, so a finger pressed and lifted between two reads would be lost entirely without a latch -- in this table every finger is first reported once as Start,
//     and is released only after being reported once more as End after lifting (pressed and lifted between two reads = Start in this report, End in the next).
// TouchState (0x28 bytes, = libnx HidTouchState): +0 delta_time (ns, i64), +8 attributes (bit0 Start / bit1 End), +0xc finger_id,
// +0x10 x, +0x14 y, +0x18 diameter x, +0x1c diameter y, +0x20 rotation angle, +0x24 reserved. TouchScreenState<16>: +0 sample number, +8 touch count, 16 TouchState from +0x10.
#include "dm.h"
#include "front.h"
#include <pthread.h>
#include <stdatomic.h>
enum { F_EMPTY, F_DOWN, F_LIFTED };
static struct { int st, reported, again; float x, y, ax, ay; } g_f[16];   // again/ax/ay: the same id was pressed again before End went out (the next read reports End first, then the new Start)
static pthread_mutex_t g_tl = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_tsamp; static uint64_t g_tlast_ns;

// Synthetic gesture (profile key touch_gesture): three fingers swiping the panel, which a game may take as a command. Into The Breach opens its console with a swipe down
//   on the touch screen, and a three-finger swipe is awkward on a phone (the system also uses multi-finger swipes); the profile maps a button to it instead.
//   The fingers take ids 13-15 (the front end can use 0-15, but a game holding 13 touches is not a thing), are reported as Start once, travel down over GEST_NS, and are
//   reported as End once, so the game sees a complete swipe rather than fingers that vanish.
//   A button mapped to "toggle" plays the down swipe first and then alternates, so one button opens the console and closes it again; an explicit down/up swipe sets the
//   same state, so a separate close button (or a swipe the game missed) does not invert the next toggle.
//   A console_command button (profile key console_command) runs a line without the player: it plays the down swipe, answers the console's input with the line and plays the
//   up swipe again. The answer must be given while the game asks (ShowKeyboard, which blocks the game while the down swipe is still running), so the close request is held
//   until that swipe has ended instead of being dropped.
#include "hle.h"
#include <time.h>
enum { GEST_N = 3, GEST_ID = 13 };
#define GEST_NS 350000000ull
static _Atomic int g_gest_req;                        // pad path asked for a swipe (touch_gesture_swipe3): 0 none, 1 down, 2 up; taken by the next read
static _Atomic int g_gest_down;                       // last direction asked for: 1 = down (the console is open), 0 = up (closed)
static int g_gest_on, g_gest_rep, g_gest_up, g_gest_pend; static uint64_t g_gest_t0;   // pend: a request that came in while the current swipe was still running
static void gest_start(int r, uint64_t t0) { g_gest_on = 1; g_gest_rep = 0; g_gest_up = (r == 2); g_gest_t0 = t0; }
void touch_gesture_swipe3(int up) {   // up: the fingers travel from the bottom to the top, otherwise from the top down
    atomic_store(&g_gest_req, up ? 2 : 1);
    atomic_store(&g_gest_down, up ? 0 : 1);
}
int touch_gesture_toggle(void) {      // the next direction for a toggle button: down when the last swipe was up, up when it was down; returns 1 for up
    int up = atomic_load(&g_gest_down) != 0;
    touch_gesture_swipe3(up);
    return up;
}
// Console command macro (profile key console_command, one button): open the console, answer its input with the line and close it again. The line waits at most
//   CONSOLE_CMD_NS for the console to ask, so a swipe the game missed cannot answer some later prompt with it.
#define CONSOLE_CMD_NS 10000000000ull   // the console usually asks within a frame of the down swipe, but a load between the two has taken seconds; the only keyboard this host answers is the console's, so a longer window costs nothing

static pthread_mutex_t g_cmd_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_cmd_line[96]; static uint64_t g_cmd_t0; static int g_cmd_on;
static uint64_t cmd_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
int touch_gesture_console_open(void) { return atomic_load(&g_gest_down) != 0; }
void touch_gesture_command_start(const char *line) {
    if (!line || !*line) return;
    pthread_mutex_lock(&g_cmd_mu);
    snprintf(g_cmd_line, sizeof g_cmd_line, "%s", line);
    g_cmd_t0 = cmd_now(); g_cmd_on = 1;
    pthread_mutex_unlock(&g_cmd_mu);
    touch_gesture_swipe3(0);   // open the console
}
int touch_gesture_command_take(char *out, size_t cap) {
    int ok = 0;
    pthread_mutex_lock(&g_cmd_mu);
    if (g_cmd_on) {
        g_cmd_on = 0;
        if (cmd_now() - g_cmd_t0 <= CONSOLE_CMD_NS && cap > 1) {
            size_t n = strlen(g_cmd_line);
            snprintf(out, cap, "%s%s", g_cmd_line, n && g_cmd_line[n - 1] == '\n' ? "" : "\n");
            ok = 1;
        }
    }
    pthread_mutex_unlock(&g_cmd_mu);
    if (ok) touch_gesture_swipe3(1);   // close it again; the game reads this once the line has run (held until the opening swipe ends)
    return ok;
}

void dm_touch_set(const dm_touch *t, int n) {
    pthread_mutex_lock(&g_tl);
    int seen[16] = {0};
    for (int i = 0; i < n; i++) {
        uint32_t id = t[i].id; if (id >= 16) continue;
        seen[id] = 1;
        if (g_f[id].st == F_LIFTED && g_f[id].reported) { g_f[id].again = 1; g_f[id].ax = t[i].x; g_f[id].ay = t[i].y; continue; }
        if (g_f[id].st == F_EMPTY) { g_f[id].st = F_DOWN; g_f[id].reported = 0; }
        else if (g_f[id].st == F_LIFTED) g_f[id].st = F_DOWN;              // Pressed again before the lift was reported: treat it as held down all along
        g_f[id].x = t[i].x; g_f[id].y = t[i].y;
    }
    for (int id = 0; id < 16; id++) {
        if (!seen[id] && g_f[id].st == F_DOWN) g_f[id].st = F_LIFTED;
        if (!seen[id] && g_f[id].again) g_f[id].again = 0;                   // Pressed again and then lifted again (End not even reported yet): drop the re-press, still report End for the original lift
    }
    pthread_mutex_unlock(&g_tl);
}

static int32_t clampi(float v, int hi) { int32_t r = (int32_t)(v * (float)(hi + 1)); return r < 0 ? 0 : r > hi ? hi : r; }
// Fill one TouchScreenState<max> (out is 0x10 + 0x28 · max bytes; max 1–16); now_ns is used for delta_time. Returns the number of touch points written.
// Fingers beyond max are left for a later read (their Start / End is not used up)
int touch_fill_n(void *out, int max, uint64_t now_ns) {
    if (max < 1) max = 1; else if (max > 16) max = 16;
    uint8_t *o = out; memset(o, 0, 0x10 + 0x28 * (size_t)max);
    pthread_mutex_lock(&g_tl);
    int64_t samp = ++g_tsamp; int64_t dt = g_tlast_ns ? (int64_t)(now_ns - g_tlast_ns) : 0; g_tlast_ns = now_ns;
    int k = 0;
    for (int id = 0; id < 16 && k < max; id++) {
        if (g_f[id].st == F_EMPTY) continue;
        uint32_t attr = 0;
        if (!g_f[id].reported) { attr |= 1; g_f[id].reported = 1; }          // Start (reported for the first time this time)
        else if (g_f[id].st == F_LIFTED) attr |= 2;                         // End (Start already reported, already lifted)
        uint8_t *s = o + 0x10 + 0x28 * k++;
        int32_t x = clampi(g_f[id].x, 1279), y = clampi(g_f[id].y, 719), d = 30, zero = 0, fid = id;
        memcpy(s + 0x00, &dt, 8); memcpy(s + 0x08, &attr, 4); memcpy(s + 0x0c, &fid, 4);
        memcpy(s + 0x10, &x, 4); memcpy(s + 0x14, &y, 4); memcpy(s + 0x18, &d, 4); memcpy(s + 0x1c, &d, 4); memcpy(s + 0x20, &zero, 4);
        if (attr & 2) {                                                      // End has been reported: release; if the same id was pressed again in the meantime, turn it into a new press
            if (g_f[id].again) { g_f[id].st = F_DOWN; g_f[id].reported = 0; g_f[id].again = 0; g_f[id].x = g_f[id].ax; g_f[id].y = g_f[id].ay; }
            else g_f[id].st = F_EMPTY;
        }
    }
    pthread_mutex_unlock(&g_tl);
    // Synthetic three-finger downward swipe: a request starts it, the fingers then travel down for GEST_NS and lift, one report each for Start and End. A request that
    //   comes in while a swipe is still running waits for it to end (the console-command macro asks for the close while the opening swipe is still on screen)
    { int r = atomic_exchange(&g_gest_req, 0); if (r) { if (g_gest_on) g_gest_pend = r; else gest_start(r, now_ns); } }
    if (g_gest_on) {
        double p = (double)(now_ns - g_gest_t0) / (double)GEST_NS; int liftp = p >= 1.0;
        if (p > 1.0) p = 1.0;
        if (!liftp || g_gest_rep) {                                        // liftp with nothing reported yet: the swipe was shorter than a frame, skip straight to the end
            for (int i = 0; i < GEST_N && k < max; i++) {
                uint32_t at = liftp ? 2u : g_gest_rep ? 0u : 1u;
                uint8_t *s = o + 0x10 + 0x28 * k++;
                int32_t x = clampi(0.30f + 0.20f * i, 1279), y = clampi((float)(g_gest_up ? 0.90 - 0.68 * p : 0.22 + 0.68 * p), 719), d = 40, z = 0, fid = GEST_ID + i;
                int64_t gdt = dt;
                memcpy(s + 0x00, &gdt, 8); memcpy(s + 0x08, &at, 4); memcpy(s + 0x0c, &fid, 4);
                memcpy(s + 0x10, &x, 4); memcpy(s + 0x14, &y, 4); memcpy(s + 0x18, &d, 4); memcpy(s + 0x1c, &d, 4); memcpy(s + 0x20, &z, 4);
            }
            if (!liftp) g_gest_rep = 1;
        }
        if (liftp) { g_gest_on = 0; if (g_gest_pend) { int r = g_gest_pend; g_gest_pend = 0; gest_start(r, now_ns); } }
    }
    int32_t cnt = k; memcpy(o + 0, &samp, 8); memcpy(o + 8, &cnt, 4);
    // DM_TOUCH_LOG=1 (acceptance diagnostics): each read with non-zero touch points logs one line -- sample number, touch count, and for each point finger_id / panel x,y / attributes (1 Start, 2 End); capped at 500 lines
    static int lg = -1, nl; if (lg < 0) lg = getenv("DM_TOUCH_LOG") != NULL;
    if (lg && k && nl < 500) {
        char b[256]; int m = snprintf(b, sizeof b, "touch read sample %lld points %d", (long long)samp, k);
        for (int i = 0; i < k && m < (int)sizeof b - 40; i++) { int32_t v[4]; memcpy(v, o + 0x10 + 0x28 * i + 0x08, 16);
            m += snprintf(b + m, sizeof b - m, " | #%d (%d,%d) attribute %d", v[1], v[2], v[3], v[0]); }
        LOG("%s", b); if (++nl == 500) LOG("DM_TOUCH_LOG reached 500 lines, logging stopped");
    }
    return k;
}
int touch_fill(void *out, uint64_t now_ns) { return touch_fill_n(out, 16, now_ns); }   // TouchScreenState<16>, 0x290 bytes
