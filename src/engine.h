// GRAVITECH3 v6.00 (GTMF build 3598) object ABI, recovered from basic_steam.codelib / base_shell.codelib.
//
// public_beta-26494568 (base_shell rebuilt) keeps this ABI, checked in IDA against that build's base_shell:
// the object header (+0x48 class, +0x50 name, +0x60 object manager, +0x6c references, +0x70/+0x78 written by
// addobj +0x17850, +0x80 command table, +0xe0/+0xe8 links, +0xec flags: sync handler +0x2e6b0), the command
// table's look-up (+0x1f840, the same layout), dispatch (+0x2e930), the factory (GetFactory3 +0x2f7b0, slots
// create/register/enumerate +0x2f870..., flags +0xec bit 1 checked after create), the creation context
// (h_mes_objdesc built by addobj), the object manager's find (slot 9, +0x17a20), broadcast (slot 12, +0x17ae0)
// and log (slot 14, +0x18780), and the config reader's open_list / read_fields (h_ac_reader slots 2 and 6,
// +0xbe80 / +0xc0e0, same code). What differs there: the link records (Dep, below: 48 bytes instead of 32) and
// the object manager's slot numbers above 45 (OM_SLOT_* below).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include "build.h"

typedef unsigned long long u64;
typedef unsigned int u32;

struct Object;
typedef void (*Handler)(Object *self, void *msg);
typedef u32 (*HashFn)(const char **key, u32 slots);

// Command table embedded in every object at +0x80: open-addressed hash, name -> handler.
// Pipelines look handlers up here directly, so it must be laid out exactly like the engine's.
struct HashSlot { u32 hash; u32 index; };   // hash has bit 31 set when the slot is used
struct CmdTable {
    Handler     *vals;      // +0x00
    u32          pad08, count;      // +0x08 / +0x0c
    u32          cap, pad14;        // +0x10
    const char **keys;      // +0x18
    u32          pad20, key_count;  // +0x20 / +0x24
    u32          key_cap, pad2c;    // +0x28
    HashSlot    *slots;     // +0x30
    u32          pad38, nslots;     // +0x38 / +0x3c
    u32          slot_cap, pad44;   // +0x40
    HashFn       hash;      // +0x48
    u32          factor, limit;     // +0x50 / +0x54
};

// Dependency entry: the stock "sync" handler asks the object manager for each of these.
// This is the 2024 builds' record (32 bytes). The object manager's find reads only the first two fields (class,
// name) in every build, so this struct is also the key engine_find() hands it.
// public_beta-26494568 links are 48 bytes (sync handler base_shell+0x2e6b0): +0x00 class, +0x08 name,
// +0x10 the owner it is set on, +0x18 set(owner, found), +0x20 get(owner), +0x28 flags. Walk another object's
// links with engine_dep() / dep_size(), never as o->deps[i].
struct Dep {
    const char *iface;      // "i_window"
    const char *name;       // "window"
    Object    **dest;
    u32         flags;      // bit 0: required, bit 1: reference counted
    u32         pad;
};
inline size_t dep_size(void) { return BF(0x20, 0x30); }

// Object manager, reachable from every object at +0x60.
//   vtbl[9]  (+0x48): Object *find(objman, Dep *)                                    (every build)
//   vtbl[12] (+0x60): void broadcast(objman, Msg *, const char *group)               (every build)
//   vtbl[14] (+0x70): void log(objman, Object *self, u32 level, const char *fmt, va_list *)   (every build)
struct ObjMan { void **vtbl; };

// The object manager's look-ups that the savegame code goes through (slot numbers moved in
// public_beta-26494568: base_shell +0xcfd0, +0x1aeb0, +0x1af60 there; the loads/saves call them as
// vtbl+0x178 / +0x188 / +0x190, e.g. adv_physic +0xdbaa0 / +0xdad70).
//   OM_SLOT_OBJECT_BY_PLACE  Object *(objman, u32 place)      the object at that place of the list at +0x150
//   OM_SLOT_DESC_ID          u32 (objman, void *description)  its savegame number, 0xffffffff for none
//   OM_SLOT_DESC_PTR         void *(objman, u32 number)       the description with that number, 0 for none
#define OM_SLOT_OBJECT_BY_PLACE BF(54, 47)
#define OM_SLOT_DESC_ID         BF(56, 49)
#define OM_SLOT_DESC_PTR        BF(57, 50)

// Base header shared by every engine class (0xf0 bytes).
//   vtbl[0] deleting destructor(self, flags)   vtbl[1] query(self, u32 *out) -> 0   vtbl[2] dispatch(self, msg)
struct Object {
    void      **vtbl;        // +0x00
    char        full[64];    // +0x08  "i_network:network"
    const char *iface;       // +0x48
    char       *inst;        // +0x50  points into full[]
    const char *author;      // +0x58
    ObjMan     *objman;      // +0x60
    u32         z68;         // +0x68
    u32         refs;        // +0x6c
    u64         z70, z78;    // +0x70  (addobj writes the creation index at +0x70 and the module at +0x78)
    CmdTable    cmds;        // +0x80
    u64         zd8;         // +0xd8
    Dep        *deps;        // +0xe0  (records of dep_size() bytes)
    u32         dep_n;       // +0xe8
    u32         flags;       // +0xec  bit 0: synced, bit 1: single instance
};
static_assert(offsetof(Object, iface) == 0x48, "layout");
static_assert(offsetof(Object, objman) == 0x60, "layout");
static_assert(offsetof(Object, cmds) == 0x80, "layout");
static_assert(offsetof(Object, cmds.slots) == 0xb0, "layout");
static_assert(offsetof(Object, cmds.hash) == 0xc8, "layout");
static_assert(offsetof(Object, deps) == 0xe0, "layout");
static_assert(offsetof(Object, flags) == 0xec, "layout");
static_assert(sizeof(Object) == 0xf0, "layout");

// The i-th link of an object in this build's record size (class and name are at +0 / +8 in every build;
// dest and flags only mean that in the 2024 builds).
inline const Dep *engine_dep(const Object *o, u32 i) { return (const Dep *)((const char *)o->deps + (size_t)i * dep_size()); }

// Message passed to dispatch/handlers: a vtable, then the command name, then command-specific payload.
struct Msg { void **vtbl; const char *name; };

// Creation context passed to a class creator.
//   +0x10 instance name, +0x18 object manager, +0x20 config reader
// Config reader: vtbl[2] bool find(reader, "iface:name", 0, u32 *handle, 0)
//                vtbl[6] void read(reader, handle, Field[], count)   keys are "u:name", "b:name", "f:name", "$:name"
struct Field { const char *key; void *dest; };
struct CreateCtx { void *p0, *p8; const char *inst; ObjMan *objman; void **reader; };

inline u32 engine_hash(const char **key, u32 n)
{
    u32 h = 0, m = 0x7e9f;
    for (const char *s = *key; *s; s++) {
        h = (m * h + (int)*s) % n;
        m = (m * 0x6a2f) % (n - 1);
    }
    return h;
}

inline Object *engine_find(ObjMan *om, const char *iface, const char *name)
{
    Object *out = 0;
    Dep d = { iface, name, &out, 0, 0 };
    return ((Object *(*)(ObjMan *, Dep *))om->vtbl[9])(om, &d);
}

inline Handler engine_lookup(Object *o, const char *name)
{
    CmdTable &t = o->cmds;
    if (!t.nslots || !t.hash) return 0;
    u32 h = t.hash(&name, t.nslots);
    for (u32 i = 0; i < t.nslots; i++) {
        HashSlot &s = t.slots[(i + h) & (t.nslots - 1)];
        if (!(s.hash & 0x80000000u)) return 0;
        if ((s.hash & 0x7fffffffu) == h && !strcmp(t.keys[s.index], name)) return t.vals[s.index];
    }
    return 0;
}
