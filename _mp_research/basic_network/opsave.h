// Operation saves (<campaign>_<operation>.opsave in the profile folder): the same binary config format as
// the battle description (see image.h). What matters for multiplayer:
//
//   header name (offset 0x20)   the operation's name, e.g. "07_02_odintsov_rus"
//   frames                      one row per turn of the operation
//   p_<formation>               one struct per formation with `side` ('GER', 'USSR') and `ai`:
//                               'USER' for the formations the player commands, a computer group code
//                               ('EN1', 'U286', ...) for the others, 0 for formations nobody moves
//   <battle group>              one struct per battle group, tagged the same way
//
// The tags decide whose moves the operational map accepts. A save with the tags turned round opens the
// same operation from the other side: that is how the map is handed to the other player and back.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace opsave {

typedef unsigned char byte;
static const unsigned k_user = 0x52455355;      // 'USER'

inline unsigned u32_at(const std::vector<byte> &b, size_t o) { unsigned v; memcpy(&v, b.data() + o, 4); return v; }

// calls f(node index, cell index within the node, offset of the cell) for every cell of the file
template <class F> inline void each_cell(const std::vector<byte> &b, F f)
{
    if (b.size() < 0x40 || memcmp(b.data(), "AZCF", 4)) return;
    unsigned count = u32_at(b, 4), table = u32_at(b, 8);
    for (unsigned i = 0; i < count; i++) {
        size_t o = table + (size_t)i * 0x80;
        if (o + 0x80 > b.size()) break;
        unsigned type = u32_at(b, o + 0x20), cols = u32_at(b, o + 0x24), rows = u32_at(b, o + 0x28), data = u32_at(b, o + 0x30);
        size_t stride = type == 1 ? 0x40 : 0x20, n = type == 1 ? rows : type == 2 ? (size_t)rows * cols : 0;
        if (!n || data + n * stride > b.size()) continue;
        for (size_t k = 0; k < n; k++) f(i, k, data + k * stride);
    }
}

// One formation or battle group: where its tag and its other fields sit in the file.
struct Tagged {
    std::string name;
    size_t ai = 0, vis = 0, px = 0, pz = 0, side = 0;     // offsets of the cells (0: the struct has no such field)
    bool pool = false;
};

// every struct that has an `ai` field
inline std::vector<Tagged> tagged(const std::vector<byte> &b)
{
    std::vector<Tagged> out;
    if (b.size() < 0x40 || memcmp(b.data(), "AZCF", 4)) return out;
    unsigned count = u32_at(b, 4), table = u32_at(b, 8);
    for (unsigned i = 0; i < count; i++) {
        size_t o = table + (size_t)i * 0x80;
        if (o + 0x80 > b.size()) break;
        if (u32_at(b, o + 0x20) != 1) continue;
        unsigned rows = u32_at(b, o + 0x28), data = u32_at(b, o + 0x30);
        if (data + (size_t)rows * 0x40 > b.size()) continue;
        Tagged t;
        for (unsigned r = 0; r < rows; r++) {
            size_t c = data + (size_t)r * 0x40;
            const char *field = (const char *)b.data() + c + 0x20;
            if (u32_at(b, c + 16) != 4) continue;
            if (!strncmp(field, "ai", 0x20)) t.ai = c;
            else if (!strncmp(field, "vis_mode", 0x20)) t.vis = c;
            else if (!strncmp(field, "px", 0x20)) t.px = c;
            else if (!strncmp(field, "pz", 0x20)) t.pz = c;
            else if (!strncmp(field, "side", 0x20)) t.side = c;
        }
        if (!t.ai) continue;
        char name[0x21] = { 0 };
        memcpy(name, b.data() + o, 0x20);
        t.name = name;
        t.pool = !strncmp(name, "p_", 2);
        out.push_back(t);
    }
    return out;
}

// What mirror() changed that unmirror() cannot work out by itself, by the struct's name.
struct Sidecar {
    unsigned code = 0;                                  // the computer code the player's own structs were given
    std::map<std::string, unsigned> ai, vis;            // the original tag / visibility of each struct
};

// The side ('GER', 'USSR') of the structs the player leads: his own side. 0 when it cannot be told.
inline unsigned user_side(const std::vector<byte> &b, const std::vector<Tagged> &all)
{
    std::map<unsigned, int> votes;
    for (const Tagged &t : all) if (t.side && u32_at(b, t.ai) == k_user) votes[u32_at(b, t.side)]++;
    unsigned side = 0;
    int best = 0;
    for (auto &kv : votes) if (kv.second > best) { best = kv.second; side = kv.first; }
    return side;
}

// Is this struct on the host player's side? His own formations are, and so are computer-led allies of his
// (some operations have them: 'AL1' next to 'USER'); they are not the other player's to command.
inline bool on_side(const std::vector<byte> &b, const Tagged &t, unsigned side)
{
    if (t.side && side) return u32_at(b, t.side) == side;
    return u32_at(b, t.ai) == k_user;
}

// For the other player: every struct of the opposing side becomes 'USER' and fully visible, the structs
// of this player's side (his own and his computer-led allies) get a computer code. Visibility
// (`vis_mode`: 3 own, 1 seen but not identified, 0 unseen) is kept by the game for the host only. The other
// player's view is made from it by taking contact to be mutual: he is shown a battle group of the host's
// side - as an unidentified contact - where it stands next to one of his own groups that the host can
// see. Where the host sees nothing of him, he sees nothing of the host.
inline bool mirror(std::vector<byte> &b, Sidecar *side, unsigned *changed)
{
    std::vector<Tagged> all = tagged(b);
    unsigned host = user_side(b, all);
    std::map<unsigned, int> pool_codes, any_codes;
    bool user = false;
    for (const Tagged &t : all) {
        unsigned v = u32_at(b, t.ai);
        if (v == k_user) user = true;
        else if (v && !on_side(b, t, host)) { any_codes[v]++; if (t.pool) pool_codes[v]++; }
    }
    if (!user || any_codes.empty()) return false;
    unsigned main = 0;
    int best = 0;
    for (auto &kv : (pool_codes.empty() ? any_codes : pool_codes)) if (kv.second > best) { best = kv.second; main = kv.first; }
    side->code = main;
    side->ai.clear();
    side->vis.clear();
    std::vector<std::pair<int, int>> theirs;          // squares of the battle groups that change to the other player
    for (const Tagged &t : all) {
        unsigned v = u32_at(b, t.ai);
        if (v && !on_side(b, t, host) && t.px && t.pz && t.vis && u32_at(b, t.vis) >= 1)
            theirs.push_back({ (int)u32_at(b, t.px), (int)u32_at(b, t.pz) });
    }
    unsigned n = 0;
    for (const Tagged &t : all) {
        unsigned v = u32_at(b, t.ai), to, vis;
        if (!v) continue;
        side->ai[t.name] = v;
        if (t.vis) side->vis[t.name] = u32_at(b, t.vis);
        if (on_side(b, t, host)) {
            to = main;
            vis = 0;
            if (t.px && t.pz)
                for (auto &sq : theirs)
                    if (abs(sq.first - (int)u32_at(b, t.px)) <= 1 && abs(sq.second - (int)u32_at(b, t.pz)) <= 1) { vis = 1; break; }
        } else {
            to = k_user;
            vis = 3;
        }
        memcpy(b.data() + t.ai, &to, 4);
        if (t.vis) memcpy(b.data() + t.vis, &vis, 4);
        n++;
    }
    if (changed) *changed = n;
    return n > 0;
}

// Back again, after the other player has made his moves in the mirrored save.
inline bool unmirror(std::vector<byte> &b, const Sidecar &side, unsigned *changed)
{
    if (!side.code) return false;
    unsigned n = 0;
    for (const Tagged &t : tagged(b)) {
        unsigned v = u32_at(b, t.ai);
        if (!v) continue;
        auto was = side.ai.find(t.name);
        unsigned to = was != side.ai.end() ? was->second : v == k_user ? side.code : k_user;
        memcpy(b.data() + t.ai, &to, 4);
        auto vis = side.vis.find(t.name);
        if (t.vis && vis != side.vis.end()) memcpy(b.data() + t.vis, &vis->second, 4);
        n++;
    }
    if (changed) *changed = n;
    return n > 0;
}

inline std::string operation_name(const std::vector<byte> &b)
{
    if (b.size() < 0x40 || memcmp(b.data(), "AZCF", 4)) return "";
    char name[0x21] = { 0 };
    memcpy(name, b.data() + 0x20, 0x20);
    return name;
}

inline void set_operation_name(std::vector<byte> &b, const std::string &name)
{
    if (b.size() < 0x40 || name.size() >= 0x20) return;
    memset(b.data() + 0x20, 0, 0x20);
    memcpy(b.data() + 0x20, name.data(), name.size());
}

// "07_02_odintsov_rus" <-> "07_02_odintsov_ger": the same operation played from the other side, when the
// game has one. Started on the other player's state it gives him his own side's view: his territory, fog
// of war over the opponent.
inline std::string counterpart(const std::string &operation)
{
    size_t n = operation.size();
    if (n > 4 && !operation.compare(n - 4, 4, "_rus")) return operation.substr(0, n - 4) + "_ger";
    if (n > 4 && !operation.compare(n - 4, 4, "_ger")) return operation.substr(0, n - 4) + "_rus";
    return "";
}

// One struct of a save: where its fields are.
struct Fields { size_t data = 0; unsigned rows = 0; };
inline std::map<std::string, Fields> structs(const std::vector<byte> &b)
{
    std::map<std::string, Fields> out;
    if (b.size() < 0x40 || memcmp(b.data(), "AZCF", 4)) return out;
    unsigned count = u32_at(b, 4), table = u32_at(b, 8);
    for (unsigned i = 0; i < count; i++) {
        size_t o = table + (size_t)i * 0x80;
        if (o + 0x80 > b.size()) break;
        if (u32_at(b, o + 0x20) != 1) continue;
        Fields f;
        f.rows = u32_at(b, o + 0x28);
        f.data = u32_at(b, o + 0x30);
        if (f.data + (size_t)f.rows * 0x40 > b.size()) continue;
        char name[0x21] = { 0 };
        memcpy(name, b.data() + o, 0x20);
        out[name] = f;
    }
    return out;
}

// Every node of a save, in file order.
struct NodeRef { std::string name; unsigned type = 0, cols = 0, rows = 0; size_t data = 0, header = 0; };
inline std::vector<NodeRef> nodes(const std::vector<byte> &b)
{
    std::vector<NodeRef> out;
    if (b.size() < 0x40 || memcmp(b.data(), "AZCF", 4)) return out;
    unsigned count = u32_at(b, 4), table = u32_at(b, 8);
    for (unsigned i = 0; i < count; i++) {
        size_t o = table + (size_t)i * 0x80;
        if (o + 0x80 > b.size()) break;
        NodeRef n;
        char name[0x21] = { 0 };
        memcpy(name, b.data() + o, 0x20);
        n.name = name;
        n.header = o;
        n.type = u32_at(b, o + 0x20);
        n.cols = u32_at(b, o + 0x24);
        n.rows = u32_at(b, o + 0x28);
        n.data = u32_at(b, o + 0x30);
        size_t cells = n.type == 1 ? (size_t)n.rows * 2 : n.type == 2 ? (size_t)n.rows * n.cols : 0;
        if (n.data + cells * 0x20 > b.size()) n.rows = 0;
        out.push_back(n);
    }
    return out;
}

// While a turn is processed, both players' sides are player-led: the game then carries out the orders both
// players gave and its computer commander plans nothing for the other player's groups (it would otherwise
// move every group of his that he left where it was). Computer-led allies of the host stay the computer's.
// Returns how many structs were changed.
inline unsigned all_player_led(std::vector<byte> &b)
{
    std::vector<Tagged> all = tagged(b);
    unsigned host = user_side(b, all), n = 0;
    for (const Tagged &t : all) {
        unsigned v = u32_at(b, t.ai);
        if (!v || v == k_user || on_side(b, t, host)) continue;
        memcpy(b.data() + t.ai, &k_user, 4);
        n++;
    }
    return n;
}

// After the turn: who commands what goes back to what the sidecar recorded. The host sees the other
// player's battle groups again only where they stand next to one of his own.
inline unsigned restore_tags(std::vector<byte> &b, const Sidecar &side)
{
    std::vector<Tagged> all = tagged(b);
    std::vector<std::pair<int, int>> mine;
    // the host's side: the side of the structs he led before the turn
    std::map<unsigned, int> votes;
    for (const Tagged &t : all) {
        auto was = side.ai.find(t.name);
        if (was != side.ai.end() && was->second == k_user && t.side) votes[u32_at(b, t.side)]++;
    }
    unsigned host = 0;
    int best = 0;
    for (auto &kv : votes) if (kv.second > best) { best = kv.second; host = kv.first; }
    auto own = [&](const Tagged &t, unsigned tag) { return t.side && host ? u32_at(b, t.side) == host : tag == k_user; };
    for (const Tagged &t : all) {
        auto was = side.ai.find(t.name);
        if (was != side.ai.end() && own(t, was->second) && t.px && t.pz) mine.push_back({ (int)u32_at(b, t.px), (int)u32_at(b, t.pz) });
    }
    unsigned n = 0;
    for (const Tagged &t : all) {
        auto was = side.ai.find(t.name);
        if (was == side.ai.end()) continue;
        unsigned vis = 3;
        if (!own(t, was->second)) {
            vis = 0;
            if (t.px && t.pz)
                for (auto &sq : mine)
                    if (abs(sq.first - (int)u32_at(b, t.px)) <= 1 && abs(sq.second - (int)u32_at(b, t.pz)) <= 1) { vis = 1; break; }
        }
        if (u32_at(b, t.ai) != was->second) n++;
        memcpy(b.data() + t.ai, &was->second, 4);
        if (t.vis) memcpy(b.data() + t.vis, &vis, 4);
    }
    return n;
}

// The sidecar as a small text file, so it survives the restarts of the host's operation program.
inline bool write_sidecar(const char *path, const Sidecar &side)
{
    FILE *f = 0;
    if (fopen_s(&f, path, "w") || !f) return false;
    fprintf(f, "%u\n", side.code);
    for (auto &kv : side.ai) fprintf(f, "%s %u\n", kv.first.c_str(), kv.second);
    fclose(f);
    return true;
}

inline bool read_sidecar(const char *path, Sidecar *side)
{
    FILE *f = 0;
    if (fopen_s(&f, path, "r") || !f) return false;
    char name[64];
    unsigned v = 0;
    side->ai.clear();
    side->vis.clear();
    bool ok = fscanf_s(f, "%u", &side->code) == 1;
    while (ok && fscanf_s(f, "%63s %u", name, (unsigned)sizeof name, &v) == 2) side->ai[name] = v;
    fclose(f);
    return ok && !side->ai.empty();
}

inline unsigned rows_of(const std::vector<byte> &b, const char *table)
{
    for (const NodeRef &n : nodes(b)) if (n.name == table && n.type == 2) return n.rows;
    return 0;
}

inline bool has_node(const std::vector<byte> &b, const char *name)
{
    for (const NodeRef &n : nodes(b)) if (n.name == name) return true;
    return false;
}

// Both players have moved, each in his own copy of the operation. This puts the other player's moves into
// the host's save as a three-way merge: `base` is the copy the other player was given, `theirs` what he
// sent back, `mine` the host's own save.
//   - every cell he changed (numbers; never text, and never who commands a group or how well it is seen)
//     is copied over the host's;
//   - the rows he added to `user_orders` - one per battle group he gave an order to - are appended to the
//     host's.
// An order on the operational map is a row in user_orders plus cells in the node and marker tables, which
// is why whole tables are compared and not just the battle groups.
inline unsigned merge_moves(std::vector<byte> &mine, const std::vector<byte> &base, const std::vector<byte> &theirs, unsigned *orders)
{
    std::vector<NodeRef> A = nodes(mine), B = nodes(base), T = nodes(theirs);
    // nodes are matched by name and by which of the same-named nodes they are ("pool" occurs once per formation)
    auto index = [](const std::vector<NodeRef> &v) {
        std::map<std::pair<std::string, int>, size_t> m;
        std::map<std::string, int> seen;
        for (size_t i = 0; i < v.size(); i++) m[{ v[i].name, seen[v[i].name]++ }] = i;
        return m;
    };
    std::map<std::pair<std::string, int>, size_t> ia = index(A), ib = index(B);
    std::map<std::string, int> seen;
    unsigned cells = 0, added = 0;
    std::vector<byte> new_orders;
    size_t orders_node = (size_t)-1;
    // The orders table only exists once somebody has given an order. His new rows are the ones after those
    // of the copy he was given (all of them when that copy had no table yet).
    {
        auto ft = std::find_if(T.begin(), T.end(), [](const NodeRef &n) { return n.name == "user_orders" && n.type == 2; });
        auto fa = ia.find({ "user_orders", 0 }), fb = ib.find({ "user_orders", 0 });
        if (ft != T.end() && fa != ia.end() && A[fa->second].type == 2 && A[fa->second].cols == ft->cols) {
            unsigned from = fb != ib.end() && B[fb->second].cols == ft->cols && B[fb->second].rows <= ft->rows ? B[fb->second].rows : 0;
            if (ft->rows > from) {
                size_t row = (size_t)ft->cols * 0x20;
                new_orders.assign(theirs.begin() + ft->data + (size_t)from * row, theirs.begin() + ft->data + (size_t)ft->rows * row);
                orders_node = fa->second;
                added = ft->rows - from;
            }
        }
    }
    for (const NodeRef &t : T) {
        std::pair<std::string, int> key = { t.name, seen[t.name]++ };
        auto fa = ia.find(key), fb = ib.find(key);
        if (fa == ia.end() || fb == ib.end()) continue;
        const NodeRef &a = A[fa->second], &b = B[fb->second];
        if (a.type != t.type || b.type != t.type) continue;
        if (t.type == 1) {
            unsigned rows = t.rows < b.rows ? t.rows : b.rows;
            if (a.rows < rows) rows = a.rows;
            for (unsigned r = 0; r < rows; r++) {
                size_t ct = t.data + (size_t)r * 0x40, cb = b.data + (size_t)r * 0x40, ca = a.data + (size_t)r * 0x40;
                const char *field = (const char *)theirs.data() + ct + 0x20;
                if (strncmp(field, (const char *)base.data() + cb + 0x20, 0x20) || strncmp(field, (const char *)mine.data() + ca + 0x20, 0x20)) continue;
                if (!strncmp(field, "ai", 0x20) || !strncmp(field, "vis_mode", 0x20)) continue;
                unsigned kind = u32_at(theirs, ct + 16);
                if (kind == 2 || u32_at(base, cb + 16) != kind || u32_at(mine, ca + 16) != kind) continue;
                if (!memcmp(theirs.data() + ct, base.data() + cb, 16)) continue;
                memcpy(mine.data() + ca, theirs.data() + ct, 16);
                cells++;
            }
        } else if (t.type == 2 && t.cols == b.cols && t.cols == a.cols) {
            if (t.name == "user_orders" || t.rows != b.rows || t.rows != a.rows) continue;
            size_t n = (size_t)t.rows * t.cols;
            for (size_t k = 0; k < n; k++) {
                size_t ct = t.data + k * 0x20, cb = b.data + k * 0x20, ca = a.data + k * 0x20;
                unsigned kind = u32_at(theirs, ct + 16);
                if (kind == 2 || u32_at(base, cb + 16) != kind || u32_at(mine, ca + 16) != kind) continue;
                if (!memcmp(theirs.data() + ct, base.data() + cb, 16)) continue;
                memcpy(mine.data() + ca, theirs.data() + ct, 16);
                cells++;
            }
        }
    }
    if (orders_node != (size_t)-1 && !new_orders.empty()) {
        // the table grows: it moves to the end of the file (offsets in this format are absolute)
        const NodeRef a = A[orders_node];
        size_t old = (size_t)a.rows * a.cols * 0x20, at = mine.size();
        std::vector<byte> block(mine.begin() + a.data, mine.begin() + a.data + old);
        block.insert(block.end(), new_orders.begin(), new_orders.end());
        mine.insert(mine.end(), block.begin(), block.end());
        unsigned rows = a.rows + added, data = (unsigned)at;
        memcpy(mine.data() + a.header + 0x28, &rows, 4);
        memcpy(mine.data() + a.header + 0x30, &data, 4);
    }
    if (orders) *orders = added;
    return cells;
}

// op_stream.turn_phase: 0 while orders are given, 2 with battles pending; -1 when it cannot be read
inline int turn_phase(const std::vector<byte> &b)
{
    std::map<std::string, Fields> all = structs(b);
    auto it = all.find("op_stream");
    if (it == all.end()) return -1;
    for (unsigned r = 0; r < it->second.rows; r++) {
        size_t c = it->second.data + (size_t)r * 0x40;
        if (!strncmp((const char *)b.data() + c + 0x20, "turn_phase", 0x20)) return (int)u32_at(b, c);
    }
    return -1;
}

// rows of the `frames` table: the number of turns
inline unsigned turns(const std::vector<byte> &b)
{
    if (b.size() < 0x40 || memcmp(b.data(), "AZCF", 4)) return 0;
    unsigned count = u32_at(b, 4), table = u32_at(b, 8);
    for (unsigned i = 0; i < count; i++) {
        size_t o = table + (size_t)i * 0x80;
        if (o + 0x80 > b.size()) break;
        if (!strncmp((const char *)b.data() + o, "frames", 0x20)) return u32_at(b, o + 0x28);
    }
    return 0;
}

// "<campaign>_<operation>.opsave" + the operation's name -> "<campaign>:<operation>", the id the game's
// programs are started with. Empty when the file name does not fit.
inline std::string operation_id(const std::string &file, const std::string &operation)
{
    std::string tail = "_" + operation + ".opsave";
    if (operation.empty() || file.size() <= tail.size() || file.compare(file.size() - tail.size(), tail.size(), tail)) return "";
    return file.substr(0, file.size() - tail.size()) + ":" + operation;
}

// a bare file name of an operation save, nothing that could leave the profile folder
inline bool safe_file_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 8 || n > 90 || strcmp(name + n - 7, ".opsave")) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (c == '.' && i == n - 7);
        if (!ok) return false;
    }
    return true;
}

} // namespace opsave
