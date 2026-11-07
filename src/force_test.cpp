// Offline check of force.h:  force_test <battle.cfgpack> <force.cfgpack> <GER|USSR> <out.cfgpack>
// Puts the given side's force of the second description into the first and writes the result.
#include "force.h"
#include <stdio.h>
#include <vector>

static std::vector<unsigned char> load(const char *path)
{
    std::vector<unsigned char> b;
    FILE *f = 0;
    if (fopen_s(&f, path, "rb") || !f) return b;
    fseek(f, 0, SEEK_END);
    b.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(b.data(), 1, b.size(), f);
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 5) { printf("force_test <battle> <force> <GER|USSR> <out>\n"); return 2; }
    std::vector<unsigned char> into = load(argv[1]), from = load(argv[2]);
    unsigned side = !strcmp(argv[3], "GER") ? 0x00524547u : 0x52535355u;
    unsigned s0, u0, s1, u1;
    force::count(into, side, &s0, &u0);
    force::count(from, side, &s1, &u1);
    printf("battle: %u squads, %u units of that side; force: %u squads, %u units\n", s0, u0, s1, u1);
    char why[200] = "";
    bool ok = force::transplant(into, from, side, why, sizeof why);
    printf("%s: %s\n", ok ? "done" : "refused", why);
    if (!ok) return 1;
    force::count(into, side, &s0, &u0);
    printf("result: %u squads, %u units of that side, %zu bytes\n", s0, u0, into.size());
    FILE *f = 0;
    if (fopen_s(&f, argv[4], "wb") || !f) return 1;
    fwrite(into.data(), 1, into.size(), f);
    fclose(f);
    return 0;
}
