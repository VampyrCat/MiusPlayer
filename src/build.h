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
//   BUILD_BETA26  public_beta-26494568  Steam's beta branch of 2026-11-06
//
// The two 2024 builds lay out every object alike; only the code of six modules moved (adv_opgame, adv_interf,
// adv_physic, basic_game, adv_qbattle, one call in basic_physic). The 2026 beta rebuilt everything: fields,
// record sizes and vtable slots moved too. Values were mapped with IDA through REA, with the symbol maps of
// the Mius Framework research's per-build symbol maps as a guide.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum { BUILD_UNKNOWN = -1, BUILD_BETA16 = 0, BUILD_PUBLIC = 1, BUILD_BETA26 = 2, BUILD_COUNT = 3 };
extern int g_bi;                            // the running game's build (BUILD_UNKNOWN until build_detect())

int build_detect(void);                     // once, before anything else; BUILD_UNKNOWN on an unknown build
const char *build_label(int b);             // "public_beta-16070931", ...
const char *build_label(void);              // the running game's
char *build_module(const char *name);       // the module's base when it is loaded and is this build's, else 0
bool build_known(void);

// A value per build, in the order of the enum above.
#define BV(beta16, pub, beta26) (g_bi == BUILD_BETA26 ? (beta26) : g_bi == BUILD_PUBLIC ? (pub) : (beta16))
// Object fields, record sizes and vtable slots: the same in both 2024 builds.
#define BF(old, beta26) BV(old, old, beta26)
// Not found in that build: what needs it stays off there.
#define NP 0

// The address at `rva` in a module of this build, once the `n` bytes there are `sig` (n 0: not checked).
// 0 when the module is not this build's, the offset is NP or the bytes differ - the caller then turns its
// feature off, and says so in the log.
char *build_code(const char *module, size_t rva, const void *sig, size_t n);
