// The battle description (tacgame_image.cfgpack): the engine's binary config format, read and patched in place.
//
//   header:  'AZCF', node count, node table offset
//   node:    name[0x20], then u32 type (1 struct, 2 table), columns, rows, type string offset, data offset
//   struct:  rows of cell[0x20] + field name[0x20];   table: rows of `columns` cells
//   cell:    u32 value (or length), u32 string offset, u32, u32, u32 kind (2 string, 4 number, 6 number list)
//
// What matters for multiplayer:
//   i_gamespec:gamespec  user_side / enemy_side   four-character side codes ('USSR', 'GER')
//   squads               one row per squad: id in column 1, a side code and a command tag ('USER' = the player's)
//   units                one row per soldier or vehicle, named "i_techn:tec_<id>" / "i_human:hum_<id>";
//                        a unit belongs to the squad with id (unit id & ~0x1f)
// The owner tag decides who commands a squad. Exchanging the tags (and the two side fields) gives the
// joining player the host's opponent while every unit keeps its id, so both games still name every
// object the same way.
#pragma once
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace image {

typedef unsigned char byte;
static const unsigned k_user = 0x52455355, k_enem = 0x4d454e45;      // 'USER', 'ENEM'

struct Node { unsigned type, cols, rows, data; bool ok; };

inline unsigned u32_at(const byte *b, size_t o) { unsigned v; memcpy(&v, b + o, 4); return v; }
inline void put_u32(byte *b, size_t o, unsigned v) { memcpy(b + o, &v, 4); }

inline Node find(const std::vector<byte> &b, const char *name)
{
    Node n = { 0, 0, 0, 0, false };
    if (b.size() < 0x40 || memcmp(b.data(), "AZCF", 4)) return n;
    unsigned count = u32_at(b.data(), 4), table = u32_at(b.data(), 8);
    for (unsigned i = 0; i < count; i++) {
        size_t o = table + (size_t)i * 0x80;
        if (o + 0x80 > b.size()) break;
        if (strncmp((const char *)b.data() + o, name, 0x20)) continue;
        n.type = u32_at(b.data(), o + 0x20);
        n.cols = u32_at(b.data(), o + 0x24);
        n.rows = u32_at(b.data(), o + 0x28);
        n.data = u32_at(b.data(), o + 0x30);
        size_t row = n.type == 1 ? 0x40 : (size_t)n.cols * 0x20;
        n.ok = n.data + (size_t)n.rows * row <= b.size();
        return n;
    }
    return n;
}

inline size_t cell(const Node &n, unsigned row, unsigned col) { return n.data + ((size_t)row * n.cols + col) * 0x20; }

// offset of a struct field's value cell, or 0
inline size_t field(const std::vector<byte> &b, const Node &n, const char *name)
{
    if (!n.ok || n.type != 1) return 0;
    for (unsigned r = 0; r < n.rows; r++) {
        size_t o = n.data + (size_t)r * 0x40;
        if (!strncmp((const char *)b.data() + o + 0x20, name, 0x20)) return o;
    }
    return 0;
}

inline bool sides(const std::vector<byte> &b, unsigned *user, unsigned *enemy)
{
    Node g = find(b, "i_gamespec:gamespec");
    size_t u = field(b, g, "user_side"), e = field(b, g, "enemy_side");
    if (!u || !e) return false;
    *user = u32_at(b.data(), u);
    *enemy = u32_at(b.data(), e);
    return true;
}

// Columns of the squads table that matter: the side code ('GER', 'USSR') and the command tag. The tag is
// 'USER' for the squads this game's player commands; every other value names a computer-led group ('ENEM'
// in a skirmish, formation codes such as 'U286' in an operation).
struct SquadColumns { int side, tag; };

inline SquadColumns squad_columns(const std::vector<byte> &b, const Node &s)
{
    SquadColumns c = { -1, -1 };
    unsigned user = 0, enemy = 0;
    if (!s.ok || s.type != 2 || !s.rows || !sides(b, &user, &enemy)) return c;
    for (unsigned col = 2; col < s.cols && (c.side < 0 || c.tag < 0); col++) {
        bool all_sides = true, any_user = false;
        for (unsigned r = 0; r < s.rows; r++) {
            size_t o = cell(s, r, col);
            unsigned v = u32_at(b.data(), o);
            if (u32_at(b.data(), o + 16) != 4) { all_sides = false; break; }
            if (v != user && v != enemy) all_sides = false;
            if (v == k_user) any_user = true;
        }
        if (all_sides && c.side < 0) c.side = (int)col;
        else if (any_user && c.tag < 0) c.tag = (int)col;
    }
    return c;
}

// squad id -> command tag
inline std::unordered_map<unsigned, unsigned> squad_owners(const std::vector<byte> &b)
{
    std::unordered_map<unsigned, unsigned> out;
    Node s = find(b, "squads");
    SquadColumns c = squad_columns(b, s);
    if (c.tag < 0 || s.cols < 2) return out;
    for (unsigned r = 0; r < s.rows; r++)
        out[u32_at(b.data(), cell(s, r, 1))] = u32_at(b.data(), cell(s, r, c.tag));
    return out;
}

// The keys (kind << 32 | id; kind 1 vehicle, 2 soldier) of every unit whose squad carries `tag`.
inline std::unordered_set<unsigned long long> units_of(const std::vector<byte> &b, unsigned tag)
{
    std::unordered_set<unsigned long long> out;
    std::unordered_map<unsigned, unsigned> owners = squad_owners(b);
    Node u = find(b, "units");
    if (!u.ok || u.type != 2) return out;
    for (unsigned r = 0; r < u.rows; r++) {
        size_t o = cell(u, r, 0);
        unsigned str = u32_at(b.data(), o + 4);
        if (u32_at(b.data(), o + 16) != 2 || !str || str >= b.size()) continue;
        const char *name = (const char *)b.data() + str;
        unsigned long long kind = !strncmp(name, "i_techn:", 8) ? 1 : !strncmp(name, "i_human:", 8) ? 2 : 0;
        const char *us = strrchr(name, '_');
        if (!kind || !us) continue;
        unsigned id = strtoul(us + 1, 0, 16);
        auto it = owners.find(id & ~0x1fu);
        if (it != owners.end() && it->second == tag) out.insert((kind << 32) | id);
    }
    return out;
}

// Turns the host's battle description into the one for a player commanding the host's opponent: the
// opponent's squads get the 'USER' tag, the host's own squads the generic computer tag, and the two side
// fields change places. Unit and squad ids stay as they are.
inline bool mirror(std::vector<byte> &b)
{
    Node g = find(b, "i_gamespec:gamespec"), s = find(b, "squads"), p = find(b, "pools");
    size_t u = field(b, g, "user_side"), e = field(b, g, "enemy_side");
    SquadColumns c = squad_columns(b, s);
    if (!u || !e || c.tag < 0 || c.side < 0) return false;
    unsigned us = u32_at(b.data(), u), es = u32_at(b.data(), e);
    unsigned turned = 0;
    for (unsigned r = 0; r < s.rows; r++) {
        size_t tag = cell(s, r, c.tag);
        unsigned side = u32_at(b.data(), cell(s, r, c.side)), v = u32_at(b.data(), tag);
        if (v == k_user) put_u32(b.data(), tag, k_enem);
        else if (side == es) { put_u32(b.data(), tag, k_user); turned++; }
    }
    if (!turned) return false;
    put_u32(b.data(), u, es);
    put_u32(b.data(), e, us);
    if (p.ok && p.type == 2 && p.rows == 2 && p.cols >= 3)
        for (unsigned col = 0; col <= 2; col += 2) {      // skirmish: pool label and command-chain name
            byte tmp[0x20];
            memcpy(tmp, b.data() + cell(p, 0, col), 0x20);
            memcpy(b.data() + cell(p, 0, col), b.data() + cell(p, 1, col), 0x20);
            memcpy(b.data() + cell(p, 1, col), tmp, 0x20);
        }
    return true;
}

// unit key -> model name (column 2 of the units table, e.g. "opel_blitz_36")
inline std::unordered_map<unsigned long long, std::string> unit_models(const std::vector<byte> &b)
{
    std::unordered_map<unsigned long long, std::string> out;
    Node u = find(b, "units");
    if (!u.ok || u.type != 2 || u.cols < 3) return out;
    for (unsigned r = 0; r < u.rows; r++) {
        size_t o = cell(u, r, 0), m = cell(u, r, 2);
        unsigned str = u32_at(b.data(), o + 4), model = u32_at(b.data(), m + 4);
        if (u32_at(b.data(), o + 16) != 2 || !str || str >= b.size()) continue;
        if (u32_at(b.data(), m + 16) != 2 || !model || model >= b.size()) continue;
        const char *name = (const char *)b.data() + str;
        unsigned long long kind = !strncmp(name, "i_techn:", 8) ? 1 : !strncmp(name, "i_human:", 8) ? 2 : 0;
        const char *us = strrchr(name, '_');
        if (!kind || !us) continue;
        out[(kind << 32) | strtoul(us + 1, 0, 16)] = (const char *)b.data() + model;
    }
    return out;
}

// "GER" -> "Germans", for the menu
inline const char *side_name(unsigned code)
{
    static char text[8];
    if (code == 0x00524547) return "Germans";
    if (code == 0x52535355) return "Soviets";
    memcpy(text, &code, 4);
    text[4] = 0;
    return text;
}

} // namespace image
