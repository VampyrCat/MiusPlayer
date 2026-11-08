// Recognising the game build (see build.h). The link stamps are those of the Steam builds' shipped files
// (Mius Framework research, builds\<label>.json, checked against the files of each build). The third column is
// public_beta-26514238's, read from Steam's files of 2026-11-08; the beta it replaced (26494568, 2026-11-06, no
// longer served) had base_shell 0x6aee3958 / base_main 0x6aee394a.
#include "build.h"
#include "common.h"
#include <string.h>

int g_bi = BUILD_UNKNOWN;

static const char *const k_labels[BUILD_COUNT] = { "public_beta-16070931", "public-15814271", "public_beta-26514238" };

struct Stamp { const char *module; DWORD stamp[BUILD_COUNT]; };
static const Stamp k_stamps[] = {
    { "base_shell.codelib",   { 0x670d5899, 0x66ed7c47, 0x6aefc0ff } },
    { "base_main.codelib",    { 0x670d5943, 0x66f2d70e, 0x6aefc0f2 } },
    { "adv_game.codelib",     { 0x670d58d4, 0x66f2d3a9, 0x6aefc1ab } },
    { "adv_interf.codelib",   { 0x670d58bf, 0x66f2a92e, 0x6aefc15e } },
    { "adv_opgame.codelib",   { 0x670d58d4, 0x66f2d62b, 0x6aefc191 } },
    { "adv_physic.codelib",   { 0x67101542, 0x66f2a91e, 0x6aefc65f } },
    { "adv_qbattle.codelib",  { 0x670d589d, 0x66f32f3f, 0x6aefc162 } },
    { "adv_render.codelib",   { 0x670d5948, 0x66f2d711, 0x6aefc0da } },
    { "basic_game.codelib",   { 0x670d59b5, 0x66f2d772, 0x6aefc197 } },
    { "basic_input.codelib",  { 0x670d593e, 0x66f2d70c, 0x6aefc0d8 } },
    { "basic_physic.codelib", { 0x670d5886, 0x66f2a8ff, 0x6aefc12d } },
    { "basic_render.codelib", { 0x670d5944, 0x66f2d710, 0x6aefc0cf } },
    { "basic_sound.codelib",  { 0x670d5942, 0x66f2d70b, 0x6aefc09a } },
    { "basic_steam.codelib",  { 0x670d593a, 0x66f2d6f8, 0x6aefc088 } },
};

static DWORD link_stamp(HMODULE m)
{
    if (!m) return 0;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)m;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)m + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->FileHeader.TimeDateStamp : 0;
}

static const Stamp *stamp_of(const char *module)
{
    for (const Stamp &s : k_stamps) if (!_stricmp(s.module, module)) return &s;
    return 0;
}

int build_detect(void)
{
    static bool done;
    if (done) return g_bi;
    done = true;
    DWORD shell = link_stamp(GetModuleHandleA("base_shell.codelib")), main = link_stamp(GetModuleHandleA("base_main.codelib"));
    for (int b = 0; b < BUILD_COUNT; b++)
        if (shell == k_stamps[0].stamp[b] && main == k_stamps[1].stamp[b]) g_bi = b;
    if (g_bi == BUILD_UNKNOWN)
        mp_log("build: unknown game build (base_shell %08lx, base_main %08lx) - multiplayer stays off; the launcher "
               "installs the version of the mod that knows it", shell, main);
    else
        mp_log("build: %s", k_labels[g_bi]);
    return g_bi;
}

bool build_known(void) { return g_bi != BUILD_UNKNOWN; }
const char *build_label(int b) { return b >= 0 && b < BUILD_COUNT ? k_labels[b] : "unknown"; }
const char *build_label(void) { return build_label(g_bi); }

char *build_code(const char *module, size_t rva, const void *sig, size_t n)
{
    char *m = build_module(module);
    if (!m || !rva) return 0;
    if (n && (!readable(m + rva, n) || memcmp(m + rva, sig, n))) return 0;
    return m + rva;
}

char *build_module(const char *name)
{
    if (g_bi == BUILD_UNKNOWN) return 0;
    HMODULE m = GetModuleHandleA(name);
    const Stamp *s = stamp_of(name);
    if (!m || !s) return 0;
    if (link_stamp(m) != s->stamp[g_bi]) {
        static char said[16][32];
        bool told = false;
        for (auto &n : said) if (!_stricmp(n, name)) told = true;
        if (!told) {
            for (auto &n : said) if (!n[0]) { strncpy_s(n, name, _TRUNCATE); break; }
            mp_log("build: %s is not %s's own (stamp %08lx) - what needs it stays off", name, k_labels[g_bi], link_stamp(m));
        }
        return 0;
    }
    return (char *)m;
}
