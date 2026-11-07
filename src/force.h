// A skirmish where the joining player picks his own force: he sets it up on the stock Skirmish screen of his
// own game, which writes a whole battle description for it (his force as the player's, an enemy picked by
// the computer). The force - his side's platoons, squads and units - is taken out of that description and
// put into the host's, in place of the force the host's battle setup made for that side.
//
// What a force is in a description (image.h has the format): every platoon, squad and unit id carries its
// pool in the top byte, and the pools table names each pool's side (column 1: pool << 24, side code).
// Platoons (plats, id in column 1), squads (squads, id in column 1) and units (units, "i_techn:tec_<id>" /
// "i_human:hum_<id>" in column 0) of one pool are one side's force. Nothing in those rows is a place on the
// map: the game deploys a skirmish's squads in the side's zone itself.
//
// Moving a force into another description: the rows of the target side's pool are removed, the force's
// rows are added with their pool changed to the target pool wherever an id of theirs appears (numbers,
// and the hex in names like "sq_08082220"), their command tag set to the target side's, and their strings
// copied over. Tables are moved to the end of the file to grow, as the engine's own files allow
// (tools\azcf.py does the same).
// Where a platoon stands is the battle's, not the force's: the battle setup gave each of the side's platoons
// a place - in its plats row (name, command chain, controller, task, deployment: columns 0, 3, 4, 5, 7, 9,
// 10, 11) and in a row of the "brief" table (the platoon's id and its spot on the map). The force's platoons
// take those places in order: the role columns are copied over, the brief rows point to the new platoons
// (a platoon more than the battle had gets the last place again, a place left over is dropped).
// The same on the 2026 beta: the same tables and columns (image.h), and its brief rows carry the platoon's id
// in the second number of their 'a' cell, made the same way (adv_opgame+0x123d40, 2024 +0xfcb60).
#pragma once
#include "image.h"
#include <stdio.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace force {

using image::byte;
using image::u32_at;
using image::put_u32;

struct Pool { unsigned id, side; std::string label; bool ok; };

inline std::string cell_string(const std::vector<byte> &b, size_t o)
{
    if (u32_at(b.data(), o + 16) != 2) return "";
    unsigned at = u32_at(b.data(), o + 4);
    if (!at || at >= b.size()) return "";
    const char *s = (const char *)b.data() + at;
    return std::string(s, strnlen(s, b.size() - at));
}

// the pool of a side ('GER', 'USSR')
inline Pool pool_of(const std::vector<byte> &b, unsigned side)
{
    Pool p = { 0, 0, "", false };
    image::Node n = image::find(b, "pools");
    if (!n.ok || n.type != 2 || n.cols < 2) return p;
    for (unsigned r = 0; r < n.rows; r++) {
        size_t o = image::cell(n, r, 1);
        if (u32_at(b.data(), o + 4) != side) continue;
        p.id = u32_at(b.data(), o) >> 24;
        p.side = side;
        p.label = cell_string(b, image::cell(n, r, 0));
        p.ok = p.id != 0;
        return p;
    }
    return p;
}

inline unsigned row_id(const std::vector<byte> &b, const image::Node &n, unsigned r, bool units)
{
    if (!units) return u32_at(b.data(), image::cell(n, r, 1));
    std::string name = cell_string(b, image::cell(n, r, 0));
    size_t us = name.rfind('_');
    return us == std::string::npos ? 0 : (unsigned)strtoul(name.c_str() + us + 1, 0, 16);
}

// How big a side's force is: squads and units of its pool.
inline void count(const std::vector<byte> &b, unsigned side, unsigned *squads, unsigned *units)
{
    *squads = *units = 0;
    Pool p = pool_of(b, side);
    if (!p.ok) return;
    image::Node s = image::find(b, "squads"), u = image::find(b, "units");
    if (s.ok) for (unsigned r = 0; r < s.rows; r++) *squads += row_id(b, s, r, false) >> 24 == p.id;
    if (u.ok) for (unsigned r = 0; r < u.rows; r++) *units += row_id(b, u, r, true) >> 24 == p.id;
}

// The side of the player who set the description up (gamespec user_side).
inline unsigned player_side(const std::vector<byte> &b)
{
    unsigned u = 0, e = 0;
    return image::sides(b, &u, &e) ? u : 0;
}

inline size_t node_pos(const std::vector<byte> &b, const char *name)
{
    unsigned count = u32_at(b.data(), 4), table = u32_at(b.data(), 8);
    for (unsigned i = 0; i < count; i++) {
        size_t o = table + (size_t)i * 0x80;
        if (o + 0x80 > b.size()) break;
        if (!strncmp((const char *)b.data() + o, name, 0x20)) return o;
    }
    return 0;
}

inline void align16(std::vector<byte> &b) { while (b.size() % 16) b.push_back(0); }

inline unsigned add_string(std::vector<byte> &b, const std::string &s)
{
    align16(b);
    unsigned at = (unsigned)b.size();
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
    return at;
}

// "sq_08082220" / "i_techn:tec_08082221": the hex id after the last '_' moved to another pool
inline std::string repool_name(const std::string &name, const std::unordered_set<unsigned> &ids, unsigned from, unsigned to)
{
    size_t us = name.rfind('_');
    if (us == std::string::npos || name.size() - us - 1 != 8) return name;
    unsigned id = (unsigned)strtoul(name.c_str() + us + 1, 0, 16);
    if (id >> 24 != from || !ids.count(id)) return name;
    char hex[16];
    sprintf_s(hex, "%08x", (id & 0xffffff) | (to << 24));
    return name.substr(0, us + 1) + hex;
}

// The names among `names` that the squads of pool `pool` (or, with `others`, of every other pool) name in
// one of their string cells.
inline void support_refs(const std::vector<byte> &b, unsigned pool, bool others, const std::unordered_set<std::string> &names,
                         std::unordered_set<std::string> *refs)
{
    image::Node s = image::find(b, "squads");
    if (!s.ok || s.type != 2 || names.empty()) return;
    for (unsigned r = 0; r < s.rows; r++) {
        unsigned p = row_id(b, s, r, false) >> 24;
        if (others ? p == pool : p != pool) continue;
        for (unsigned c = 0; c < s.cols; c++) {
            size_t o = image::cell(s, r, c);
            if (u32_at(b.data(), o + 16) != 2) continue;
            std::string v = cell_string(b, o);
            if (!v.empty() && names.count(v)) refs->insert(v);
        }
    }
}

inline std::unordered_set<std::string> row_names(const std::vector<byte> &b, const image::Node &n)
{
    std::unordered_set<std::string> out;
    if (n.ok && n.type == 2 && n.cols) for (unsigned r = 0; r < n.rows; r++) out.insert(cell_string(b, image::cell(n, r, 0)));
    return out;
}

inline std::string node_name(const std::vector<byte> &b, unsigned index)
{
    unsigned count = u32_at(b.data(), 4), table = u32_at(b.data(), 8);
    if (index >= count || (size_t)table + (size_t)(index + 1) * 0x80 > b.size()) return "";
    const char *s = (const char *)b.data() + table + (size_t)index * 0x80;
    return std::string(s, strnlen(s, 0x20));
}

inline unsigned node_index(const std::vector<byte> &b, const std::string &name)
{
    unsigned count = u32_at(b.data(), 4);
    for (unsigned i = 0; i < count; i++) if (node_name(b, i) == name) return i;
    return ~0u;
}

// A node's children are found by name through its lookup slots (node +0x44 slot count, +0x48 the slots:
// {0x80000000 | home slot, child's node index}). How many slots there are and where a name lands depend on
// how many children there are (five tables under i_divisions: 16 slots; six: 32, laid out differently), so
// they are taken from a description the game wrote with the same children: `like`, the force's own. Each of
// its slots is copied with the child of the same name here.
inline bool copy_slots(std::vector<byte> &out, size_t np, const std::vector<byte> &like, const char *parent)
{
    size_t lp = node_pos(like, parent);
    if (!lp) return false;
    unsigned slots = u32_at(like.data(), lp + 0x44), hash = u32_at(like.data(), lp + 0x48);
    unsigned kids = u32_at(out.data(), np + 0x34), nk = u32_at(out.data(), np + 0x38);
    if (!slots || slots > 256 || !hash || (size_t)hash + (size_t)slots * 8 > like.size() || (size_t)kids + (size_t)nk * 4 > out.size()) return false;
    std::vector<byte> table((size_t)slots * 8, 0);
    unsigned placed = 0;
    for (unsigned k = 0; k < slots; k++) {
        unsigned key = u32_at(like.data(), hash + (size_t)k * 8), idx = u32_at(like.data(), hash + (size_t)k * 8 + 4);
        if (!key) continue;
        unsigned here = node_index(out, node_name(like, idx));
        if (here == ~0u) return false;
        put_u32(table.data(), (size_t)k * 8, key);
        put_u32(table.data(), (size_t)k * 8 + 4, here);
        placed++;
    }
    if (placed != nk) return false;                                 // (every child of the node here, no other)
    for (unsigned i = 0; i < nk; i++) {
        unsigned child = u32_at(out.data(), kids + (size_t)i * 4);
        bool found = false;
        for (unsigned k = 0; k < slots && !found; k++) found = u32_at(table.data(), (size_t)k * 8) && u32_at(table.data(), (size_t)k * 8 + 4) == child;
        if (!found) return false;
    }
    align16(out);
    unsigned at = (unsigned)out.size();
    out.insert(out.end(), table.begin(), table.end());
    put_u32(out.data(), np + 0x44, slots);
    put_u32(out.data(), np + 0x48, at);
    return true;
}

// A new table `name` (rows already laid out, `cols` wide) as the last child of node `parent`. The node table
// and the parent's list of children are moved to the end of the file to grow; the parent's lookup slots are
// made as in `like`, a description the game wrote with that table.
inline bool add_table(std::vector<byte> &out, const char *parent, const char *name, unsigned cols, const std::string &types,
                      const std::vector<byte> &rows, const std::vector<byte> &like)
{
    size_t pp = node_pos(out, parent), leaf = node_pos(out, "squads");
    unsigned count = u32_at(out.data(), 4), table = u32_at(out.data(), 8);
    if (!pp || !leaf || !cols || (size_t)table + (size_t)count * 0x80 > out.size()) return false;
    unsigned kids = u32_at(out.data(), pp + 0x34), nk = u32_at(out.data(), pp + 0x38);
    if (nk && (size_t)kids + (size_t)nk * 4 > out.size()) return false;
    std::vector<byte> kid_list(out.begin() + (nk ? kids : 0), out.begin() + (nk ? kids + (size_t)nk * 4 : 0));
    std::vector<byte> nodes(out.begin() + table, out.begin() + table + (size_t)count * 0x80);
    byte e[0x80] = { 0 };
    strncpy_s((char *)e, 0x20, name, _TRUNCATE);
    put_u32(e, 0x20, 2);
    put_u32(e, 0x24, cols);
    put_u32(e, 0x28, (unsigned)(rows.size() / ((size_t)cols * 0x20)));
    put_u32(e, 0x2c, add_string(out, types));
    align16(out);
    put_u32(e, 0x30, (unsigned)out.size());
    out.insert(out.end(), rows.begin(), rows.end());
    put_u32(e, 0x34, u32_at(out.data(), leaf + 0x34));         // (an empty child list, as the other tables have it)
    nodes.insert(nodes.end(), e, e + sizeof e);
    align16(out);
    unsigned nt = (unsigned)out.size();
    out.insert(out.end(), nodes.begin(), nodes.end());
    byte idx[4];
    put_u32(idx, 0, count);
    kid_list.insert(kid_list.end(), idx, idx + 4);
    align16(out);
    unsigned kat = (unsigned)out.size();
    out.insert(out.end(), kid_list.begin(), kid_list.end());
    size_t np = nt + (pp - table);
    put_u32(out.data(), np + 0x34, kat);
    put_u32(out.data(), np + 0x38, nk + 1);
    put_u32(out.data(), 4, count + 1);
    put_u32(out.data(), 8, nt);
    return copy_slots(out, np, like, parent);
}

// Off-map support (a battery, aircraft) belongs to the squad that can call it: the squad names it in one of
// its string cells (e.g. "rkka_artsup_m30_plat") and the battle's "supports" table (a child of
// i_divisions) has a row of that name with the guns, shells and timings - the battle links them by name
// (adv_game: squad +0x2a8, the row's index at +0x2b0). Rows carry no side. The rows the force's squads
// name come along; rows only the replaced squads named go. A battle set up without supports has no such
// table: it is made. What cannot be carried is taken off the squads, which then are ordinary squads.
inline void carry_supports(std::vector<byte> &out, const std::vector<byte> &into, const std::vector<byte> &from,
                           unsigned fpool, unsigned tpool, unsigned *carried, unsigned *dropped)
{
    *carried = *dropped = 0;
    image::Node fs = image::find(from, "supports"), os = image::find(out, "supports");
    std::unordered_set<std::string> from_names = row_names(from, fs), out_names = row_names(out, os), needed, old_refs, kept_refs;
    support_refs(from, fpool, false, from_names, &needed);
    support_refs(into, tpool, false, out_names, &old_refs);      // what the replaced squads named
    support_refs(out, tpool, true, out_names, &kept_refs);       // what the other side's squads name
    if (needed.empty() && old_refs.empty()) return;
    bool can = fs.ok && fs.type == 2 && (!os.ok || (os.type == 2 && os.cols == fs.cols));
    unsigned cols = os.ok ? os.cols : fs.cols;
    size_t width = (size_t)cols * 0x20;
    std::vector<byte> rows;
    std::unordered_set<std::string> final_names;
    if (os.ok && os.type == 2) for (unsigned r = 0; r < os.rows; r++) {
        std::string n = cell_string(out, image::cell(os, r, 0));
        if (old_refs.count(n) && !kept_refs.count(n) && !(can && needed.count(n))) continue;
        rows.insert(rows.end(), out.begin() + image::cell(os, r, 0), out.begin() + image::cell(os, r, 0) + width);
        final_names.insert(n);
    }
    if (can) for (unsigned r = 0; r < fs.rows; r++) {
        std::string n = cell_string(from, image::cell(fs, r, 0));
        if (!needed.count(n) || final_names.count(n)) continue;
        std::vector<byte> row(from.begin() + image::cell(fs, r, 0), from.begin() + image::cell(fs, r, 0) + width);
        for (unsigned c = 0; c < cols; c++) {
            byte *cell = row.data() + (size_t)c * 0x20;
            if (u32_at(cell, 16) != 2) continue;
            std::string s = cell_string(from, image::cell(fs, r, c));
            put_u32(cell, 0, (unsigned)s.size());
            put_u32(cell, 4, add_string(out, s));
        }
        rows.insert(rows.end(), row.begin(), row.end());
        final_names.insert(n);
        (*carried)++;
    }
    if (os.ok) {
        align16(out);
        unsigned at = (unsigned)out.size();
        out.insert(out.end(), rows.begin(), rows.end());
        size_t pos = node_pos(out, "supports");
        put_u32(out.data(), pos + 0x28, (unsigned)(rows.size() / width));
        put_u32(out.data(), pos + 0x30, at);
    } else if (!rows.empty()) {
        size_t fpos = node_pos(from, "supports");
        unsigned tat = fpos ? u32_at(from.data(), fpos + 0x2c) : 0;
        std::string types = tat && tat < from.size() ? std::string((const char *)from.data() + tat, strnlen((const char *)from.data() + tat, from.size() - tat)) : "";
        std::vector<byte> before = out;
        if (types.size() != cols || !add_table(out, "i_divisions:divisions", "supports", cols, types, rows, from)) {
            out.swap(before);                                       // (as it was: no table, the squads lose the support)
            final_names.clear();
            *carried = 0;
        }
    }
    // the force's squads that name a support the battle does not have now
    image::Node sq = image::find(out, "squads");
    unsigned blank = 0;
    for (unsigned r = 0; sq.ok && r < sq.rows; r++) {
        if (row_id(out, sq, r, false) >> 24 != tpool) continue;
        for (unsigned c = 0; c < sq.cols; c++) {
            size_t o = image::cell(sq, r, c);
            if (u32_at(out.data(), o + 16) != 2) continue;
            std::string v = cell_string(out, o);
            if (v.empty() || !from_names.count(v) || final_names.count(v)) continue;
            if (!blank) blank = add_string(out, "");
            put_u32(out.data(), o, 0);
            put_u32(out.data(), o + 4, blank);
            (*dropped)++;
        }
    }
}

// Puts the force of `side` from `from` (a whole description) into `into`, replacing what `into` has for that
// side. False (with `why` filled in) leaves `into` as it was.
inline bool transplant(std::vector<byte> &into, const std::vector<byte> &from, unsigned side, char *why, size_t why_size)
{
    Pool fp = pool_of(from, side), tp = pool_of(into, side);
    if (!fp.ok) { sprintf_s(why, why_size, "the chosen force has no units of that side"); return false; }
    if (!tp.ok) { sprintf_s(why, why_size, "the battle has no pool for that side"); return false; }
    unsigned user = 0, enemy = 0;
    image::sides(into, &user, &enemy);
    unsigned tag = side == user ? image::k_user : image::k_enem;
    std::string want_label = tp.label;

    // every id of the force: platoons, squads, units
    std::unordered_set<unsigned> ids;
    const char *tables[] = { "plats", "squads", "units" };
    for (const char *t : tables) {
        image::Node n = image::find(from, t), m = image::find(into, t);
        if (!n.ok || n.type != 2 || !m.ok || m.type != 2 || n.cols != m.cols) {
            sprintf_s(why, why_size, "table %s is not the same in both descriptions", t);
            return false;
        }
        for (unsigned r = 0; r < n.rows; r++) {
            unsigned id = row_id(from, n, r, !strcmp(t, "units"));
            if (id >> 24 == fp.id) ids.insert(id);
        }
    }
    image::SquadColumns sc = image::squad_columns(from, image::find(from, "squads"));
    if (sc.tag < 0) { sprintf_s(why, why_size, "the chosen force's squads have no command tag"); return false; }

    std::vector<byte> out = into;
    unsigned added[3] = { 0, 0, 0 }, removed[3] = { 0, 0, 0 };
    std::vector<std::vector<byte>> slots;           // the side's platoon rows the force's platoons take the place of
    std::vector<unsigned> slot_ids, new_ids;
    for (int ti = 0; ti < 3; ti++) {
        const char *t = tables[ti];
        bool units = ti == 2;
        image::Node n = image::find(from, t), m = image::find(out, t);
        size_t width = (size_t)m.cols * 0x20;
        std::vector<byte> rows;
        for (unsigned r = 0; r < m.rows; r++) {             // what stays: the other pools' rows
            if (row_id(out, m, r, units) >> 24 == tp.id) {
                removed[ti]++;
                if (ti == 0) {
                    slots.emplace_back(out.begin() + image::cell(m, r, 0), out.begin() + image::cell(m, r, 0) + width);
                    slot_ids.push_back(row_id(out, m, r, false));
                }
                continue;
            }
            rows.insert(rows.end(), out.begin() + image::cell(m, r, 0), out.begin() + image::cell(m, r, 0) + width);
        }
        for (unsigned r = 0; r < n.rows; r++) {             // the force's rows, moved to the target pool
            if (row_id(from, n, r, units) >> 24 != fp.id) continue;
            std::vector<byte> row(from.begin() + image::cell(n, r, 0), from.begin() + image::cell(n, r, 0) + width);
            for (unsigned c = 0; c < n.cols; c++) {
                byte *cell = row.data() + (size_t)c * 0x20;
                unsigned kind = u32_at(cell, 16);
                if (kind == 2) {
                    std::string s = cell_string(from, image::cell(n, r, c));
                    s = repool_name(s, ids, fp.id, tp.id);
                    unsigned at = u32_at(cell, 4) ? add_string(out, s) : 0;
                    put_u32(cell, 0, (unsigned)s.size());
                    put_u32(cell, 4, at);
                } else if (kind == 4 || kind == 6) {                // a number, or up to four of them: ids are moved
                    for (int k = 0; k < (kind == 6 ? 4 : 1); k++) {
                        unsigned v = u32_at(cell, k * 4);
                        if (v >> 24 == fp.id && ids.count(v)) put_u32(cell, k * 4, (v & 0xffffff) | (tp.id << 24));
                    }
                } else if (kind != 0 && kind != 1 && kind != 5) {     // (floats, vectors, switches stay)
                    sprintf_s(why, why_size, "an unknown kind of value (%u) in table %s", kind, t);
                    return false;
                }
            }
            if (!strcmp(t, "squads")) put_u32(row.data() + (size_t)sc.tag * 0x20, 0, tag);
            if (ti == 0) {
                size_t k = added[0];
                if (!slots.empty()) {
                    const std::vector<byte> &slot = slots[k < slots.size() ? k : slots.size() - 1];
                    static const unsigned role[] = { 0, 3, 4, 5, 7, 9, 10, 11 };
                    for (unsigned c : role) if (c < m.cols) memcpy(row.data() + (size_t)c * 0x20, slot.data() + (size_t)c * 0x20, 0x20);
                    if (k >= slots.size()) {                    // a platoon more than the battle had: a name of its own
                        char name[48];
                        sprintf_s(name, "pl_%s_%u", want_label.c_str(), (unsigned)k);
                        unsigned at = add_string(out, name);
                        put_u32(row.data(), 0, (unsigned)strlen(name));
                        put_u32(row.data(), 4, at);
                    }
                }
                new_ids.push_back(u32_at(row.data(), 0x20));    // (column 1: the platoon's id)
            }
            rows.insert(rows.end(), row.begin(), row.end());
            added[ti]++;
        }
        align16(out);
        unsigned at = (unsigned)out.size();
        out.insert(out.end(), rows.begin(), rows.end());
        size_t pos = node_pos(out, t);
        if (!pos) { sprintf_s(why, why_size, "table %s not found", t); return false; }
        put_u32(out.data(), pos + 0x28, (unsigned)(rows.size() / width));
        put_u32(out.data(), pos + 0x30, at);
    }
    // the platoons' spots on the map
    unsigned briefed = 0;
    image::Node bn = image::find(out, "brief");
    if (bn.ok && bn.type == 2 && bn.cols >= 1 && !slot_ids.empty()) {
        size_t width = (size_t)bn.cols * 0x20;
        std::unordered_set<unsigned> old(slot_ids.begin(), slot_ids.end());
        std::vector<byte> rows, last;
        for (unsigned r = 0; r < bn.rows; r++) {
            std::vector<byte> row(out.begin() + image::cell(bn, r, 0), out.begin() + image::cell(bn, r, 0) + width);
            unsigned id = u32_at(row.data(), 4);
            if (u32_at(row.data(), 16) == 6 && old.count(id)) {
                size_t k = 0;
                while (k < slot_ids.size() && slot_ids[k] != id) k++;
                if (k >= new_ids.size()) continue;              // a place no platoon of the force takes
                put_u32(row.data(), 4, new_ids[k]);
                last = row;
                briefed++;
            }
            rows.insert(rows.end(), row.begin(), row.end());
        }
        for (size_t k = slot_ids.size(); k < new_ids.size() && !last.empty(); k++) {
            put_u32(last.data(), 4, new_ids[k]);
            rows.insert(rows.end(), last.begin(), last.end());
            briefed++;
        }
        align16(out);
        unsigned at = (unsigned)out.size();
        out.insert(out.end(), rows.begin(), rows.end());
        size_t pos = node_pos(out, "brief");
        put_u32(out.data(), pos + 0x28, (unsigned)(rows.size() / width));
        put_u32(out.data(), pos + 0x30, at);
    }
    unsigned carried = 0, dropped = 0;
    carry_supports(out, into, from, fp.id, tp.id, &carried, &dropped);
    into.swap(out);
    sprintf_s(why, why_size, "%u platoons, %u squads, %u units in place of %u, %u, %u; %u platoons placed where the battle's stood; %u supports%s",
              added[0], added[1], added[2], removed[0], removed[1], removed[2], briefed, carried,
              dropped ? " (a support could not be carried: its squad is an ordinary one)" : "");
    return added[1] != 0;
}

} // namespace force
