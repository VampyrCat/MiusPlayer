// Wire format shared by the game module (session.cpp, statesync.cpp, ui.cpp) and the join launcher
// (mp_join.cpp). UDP, little endian.
#pragma once

enum { PKT_HELLO = 1, PKT_WELCOME, PKT_KEEPALIVE, PKT_IMAGE_REQUEST, PKT_IMAGE_DATA, PKT_STATE, PKT_BYE, PKT_OBJECT, PKT_OBJECT_ACK, PKT_SHOT, PKT_SHOT_ACK };
enum { HOST_MODE_SKIRMISH = 1, HOST_MODE_OPERATION = 2, HOST_MODE_MISSION = 3 };       // mission: made in the battle editor
static const unsigned k_magic = 0x504d5447;          // "GTMP"
static const unsigned k_version = 12;
static const char k_protocol[12] = "GTMF_TA_A1";     // the engine's own protocol id for this game
static const unsigned k_chunk = 1000;
static const unsigned short k_default_port = 1943;   // the port the engine's own network settings name
static const char k_image_path[] = "data\\common\\shared\\config\\tacgame_image.cfgpack";
static const unsigned k_side_germans = 0x00524547, k_side_soviets = 0x52535355;      // 'GER', 'USSR'

#pragma pack(push, 1)
struct Head { unsigned magic; unsigned char type, flags; unsigned short count; };
// prefer_side: the side code the joining player wants to command (0: whichever the host does not play)
// ready: the battle phase this player has asked to move on to (see StateHead); phase: where its game is
// lobby_ready: the joining player pressed READY in the lobby; swap_req: counts that player's SWAP SIDES presses
// name: the player's profile name
// phase: 0xff in the menus, 0xfe on the operational map with the map in hand, otherwise the battle phase.
// map_size / map_hash: the joining player offers the operational map back (his moves are in it).
// turn_done: on the operational map, this player has pressed "next turn". map_epoch: which state of the
// operation his copy of the map is of (see Welcome.map_flags); with another number he is not counted as
// being on the map - his copy is of a turn that is over.
struct Hello {
    Head h; char proto[12]; unsigned version, prefer_side; unsigned char ready, phase, lobby_ready, swap_req; char name[24];
    unsigned map_size, map_hash, turn_done, map_epoch;
};
// battle description = the file the operational phase hands to the tactical phase (units, map, sides, seed).
// in_battle 0: the host is still in its menus or on the operational map; the joining game waits.
// versus 1: the joining player commands the host's opponent; 0: both look at the battle from the host's side.
struct Welcome {
    Head h; char proto[12]; unsigned version, image_size, image_hash, user_side, enemy_side;
    unsigned char in_battle, host_mode, versus, swap;      // swap: the players have exchanged forces (skirmish)
    char name[24];                                          // the host's profile name
    // Operation: the host shares the operational map with the joining player (both move at the same time). map_file is the name of
    // the operation save in the profile folder, map_turns the operation's length in turns. With map_size 0,
    // map_hash is the hash of the map the host last took back (the joining game's cue to let go of it).
    unsigned map_size, map_hash, map_turns;
    char map_file[96];
    // bit 0: both players have ended the turn - the joining game sends its map now; bit 1: the host has
    // pressed "next turn"; bits 8 and up: a number that changes whenever the operation moves on (a turn
    // ended, a battle was fought) - a joining game whose copy of the map carries another number takes the new map
    unsigned map_flags;
};
// Head.flags on the two image packets: 0 = battle description, 1 = operation save ("the map")
struct ImageRequest { Head h; unsigned index; };
struct ImageData { Head h; unsigned index, size; unsigned char data[k_chunk]; };
// Unit transforms from the game that owns those units, plus (host only) the battle clock.
// speed: the rate the host's battle time actually advances at (its speed setting, less whatever the engine's
// automatic slow-down takes off); sim_ms: battle time so far.
// phase: 0 deployment, 1 initial orders, 2 battle, 3 battle over (the value of the engine's "gamemode" broadcast)
// ready: deployment and initial orders end only when both players have pressed the game's Start button;
// 1 = this player has finished deployment, 2 = has finished the initial orders as well.
struct StateHead { Head h; unsigned snapshot, sim_ms; float speed, cur_speed; unsigned char paused, phase, ready, pad; };
struct UnitRec { unsigned id; unsigned char kind, pad[3]; float pos[3], rot[9], lin[3], ang[3]; };
// One datagram of an object's saved state: a run-length coded XOR against the version the receiver holds.
struct ObjectFrag { Head h; unsigned long long key; unsigned version, base_version, raw_size, delta_size; unsigned short frag, nfrags; };
struct ObjectAck { Head h; unsigned long long key; unsigned version; };
// Shell launches by units the sending game owns (shots.cpp). They are events, not state: each one has to
// arrive, once, and in the order it happened. A PKT_SHOT datagram is a ShotHead and h.count ShotRecs with
// consecutive sequence numbers; the receiver takes the next one it is waiting for and answers every datagram
// with a ShotAck naming the last one it has; the sender repeats what is not acknowledged. epoch changes
// when the sender starts counting again (a new battle, a new connection).
struct ShotHead { Head h; unsigned epoch; };
struct ShotAck { Head h; unsigned epoch, seq; };
// kind 0: one launch; kind 2: one squad digging in (trench.cpp; its values are in `launch`); kind 3, 4: a
// piece of the sender's table of description / object numbers (xlate.cpp). launch: the engine's launch record with its pointers blanked - they travel as ammo / weapon /
// desc3 (the engine's savegame ids of the descriptions, 0xffffffff: none) and shooter / obj2 / obj5 (unit
// numbers, 0: none). ammo_check / ammo_code: what the receiver compares its own description against.
struct ShotRec {
    unsigned seq;
    unsigned char kind, flags, has_pos, pad;
    unsigned ammo, weapon, desc3, arg6, ammo_code;
    float ammo_check[2];
    unsigned long long shooter, obj2, obj5;
    float pos[4];
    unsigned char launch[0x90];
};
#pragma pack(pop)

inline unsigned fnv(const unsigned char *p, size_t n)
{
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

inline Head make_head(int type, int count = 0)
{
    Head h = { k_magic, (unsigned char)type, 0, (unsigned short)count };
    return h;
}
