// The game builds this module knows, and how it picks the values that differ between them.
//
// Steam has shipped the game's code in several builds, and every engine address, vtable slot and object field
// the module uses belongs to one of them. The build is recognised once, from the link stamps of the code
// modules (build_detect); nothing of the game is touched before that, and on a build it does not know the
// module stays out of the game altogether. Every module is checked against the build's stamp again before
// anything in it is used (build_module), so a module from another build turns off only what needs it.
//
//   BUILD_BETA16  public_beta-16070931  Steam's beta branch of October 2024 - the modding copy
//   BUILD_PUBLIC  public-15814271       Steam's default branch (September 2024)
//   BUILD_BETA26  public_beta-26834468  Steam's beta branch of 2026-11-21 - its DEFAULT branch since 2026-11-22
//   BUILD_BETA26N public_beta-26858192  Steam's beta branch of 2026-11-23
//
// The two 2024 builds lay out every object alike; only the code of six modules moved (adv_opgame, adv_interf,
// adv_physic, basic_game, adv_qbattle, one call in basic_physic). The 2026 beta rebuilt everything: fields,
// record sizes and vtable slots moved too. Values were mapped with IDA through REA, with the symbol maps of
// the Mius Framework research's per-build symbol maps as a guide.
// The BUILD_BETA26 column was first made for the beta of 2026-11-06 (public_beta-26494568, no longer served) and
// moved on to 2026-11-08's (26514238) by matching every function of the two in IDA (NOTES.md, 2026-11-08): most
// code is the same, only moved; the soldier's fields from about +0x2440 on, the vehicle's from about +0x1a00 on,
// i_intereff's from about +0x1000, the AI commander's and the battle timer's pause moved again. On 2026-11-14 it moved
// on to 26665733 (26514238 no longer served) the same way (NOTES.md, 2026-11-14): the soldier's fields from +0x20a0
// on, the vehicle's from +0xab8 on, i_intereff's +0x50, the trigger manager's, the squad record's, the battle group
// record and several record sizes moved. On 2026-11-18 it moved on to 26744384 (26665733 no longer served; NOTES.md,
// 2026-11-18): the soldier's fields from +0x2558 on, the vehicle's from +0x187c on, the wheel, station, squad and
// animation records, the trigger manager's timer and evaluation block, the AI commander's list and the saved
// state's fourth list moved or grew. On 2026-11-19 it moved on to 26768241 (26744384 no longer served; NOTES.md,
// 2026-11-19): adv_opgame, adv_interf and base_shell kept their code where it was; in the other modules the code
// moved, the vehicle's fields from about +0x2230 on are 0x10 further on, i_orender has a slot more (31) and its
// object 0x20 bytes more. The same day (11:05) it moved on to 26779327 (26768241 no longer served; ops\ports.md,
// "26779327"): adv_opgame, base_shell and base_main kept their code where it was, most of adv_interf's is 0xd0
// further on, the other modules' code moved; the vehicle's fields from about +0x1100 on are 0x10 further on and
// from about +0x2238 on 0x1b0 (the object 0x39a0 bytes), i_orender's stored hits are at +0xb10, i_animch has a
// slot more (50) and the AI commander's group step begins otherwise. On 2026-11-21 it moved on to 26834468
// (26779327 no longer served; ops\ports.md, "26834468"): every module relinked, most of them smaller (developers'
// trace lines taken out), and all the code moved, adv_opgame's too; the vehicle's fields from about +0x1590 on are
// 0x10 further on and from about +0x2250 on 0x270 (the object 0x3c10 bytes), the soldier's from about +0x2970 on
// 0x4..0x70 (0x47b0 bytes), i_intereff's from about +0x1000 on 0x46c..0x470 nearer the start, the trigger manager's
// from +0xd30 on 0x90 further on (0xf60 bytes), the AI commander's group records are 0x690 bytes, the battle timer's
// pause is +0x1fc, and several hooked routines begin otherwise. Addresses named "beta26" in this source's comments
// are mostly older builds'; the third values in BV / the second in BF are 26834468's.
// On 2026-11-22 Steam made 26834468 its default branch and on 11-23 put 26858192 on the beta branch: both are served,
// so from 2026-11-24 the module knows four builds. BUILD_BETA26N is 26858192: the fourth value of BV and the third of
// BF (ops\ports.md, "26858192"). IS2026 is either 2026 build - what the source did "on the 2026 beta" it does on both.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum { BUILD_UNKNOWN = -1, BUILD_BETA16 = 0, BUILD_PUBLIC = 1, BUILD_BETA26 = 2, BUILD_BETA26N = 3, BUILD_COUNT = 4 };
extern int g_bi;                            // the running game's build (BUILD_UNKNOWN until build_detect())

int build_detect(void);                     // once, before anything else; BUILD_UNKNOWN on an unknown build
const char *build_label(int b);             // "public_beta-16070931", ...
const char *build_label(void);              // the running game's
char *build_module(const char *name);       // the module's base when it is loaded and is this build's, else 0
bool build_known(void);

// A value per build, in the order of the enum above.
#define BV(beta16, pub, beta26, beta26n) (g_bi == BUILD_BETA26N ? (beta26n) : g_bi == BUILD_BETA26 ? (beta26) : g_bi == BUILD_PUBLIC ? (pub) : (beta16))
// Object fields, record sizes and vtable slots: the same in both 2024 builds.
#define BF(old, beta26, beta26n) BV(old, old, beta26, beta26n)
// Either 2026 build (26834468, 26858192): the 2026 layout, routines and renderer.
#define IS2026 (g_bi >= BUILD_BETA26)
// Not found in that build: what needs it stays off there.
#define NP 0

// The address at `rva` in a module of this build, once the `n` bytes there are `sig` (n 0: not checked).
// 0 when the module is not this build's, the offset is NP or the bytes differ - the caller then turns its
// feature off, and says so in the log.
char *build_code(const char *module, size_t rva, const void *sig, size_t n);
