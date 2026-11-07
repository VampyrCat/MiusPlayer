// Wire format shared by the game module (session.cpp, statesync.cpp, ui.cpp) and the join launcher
// (mp_join.cpp). UDP, little endian.
#pragma once

enum { PKT_HELLO = 1, PKT_WELCOME, PKT_KEEPALIVE, PKT_IMAGE_REQUEST, PKT_IMAGE_DATA, PKT_STATE, PKT_BYE, PKT_OBJECT, PKT_OBJECT_ACK, PKT_SHOT, PKT_SHOT_ACK, PKT_ANIM, PKT_SIDE,
       PKT_FORCE_REQUEST, PKT_FORCE_DATA, PKT_VDMG, PKT_GUNS };     // PKT_VDMG: a vehicle's damage from the host (vdamage.cpp); PKT_GUNS: where its guns point (guns.cpp)
enum { HOST_MODE_SKIRMISH = 1, HOST_MODE_OPERATION = 2, HOST_MODE_MISSION = 3 };       // mission: made in the battle editor
static const unsigned k_magic = 0x504d5447;          // "GTMP"
// 21: mpmod2 - the host's game decides everything in a battle (orders travel to it)
// 22: gun angles (PKT_GUNS), a skirmish starts when both players are ready
// 23: the game build in Hello/Welcome (both players need the same one); the operational map shared to look at
//     while battles are being fought (Welcome.map_flags bit 3)
// 24: the joining player's sector / turn / fire-at-ground orders (ShotRec kind 15), seats (16), the ground (17)
// 25: the map's objects (ShotRec kind 18; the joining game makes no object damage of its own)
static const unsigned k_version = 25;
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
// force_size / force_hash: skirmish lobby - the joining player has picked his own force on his Skirmish screen
// (force.h); it is the whole battle description his game made, fetched by the host with PKT_FORCE_REQUEST.
struct Hello {
    Head h; char proto[12]; unsigned version, prefer_side; unsigned char ready, phase, lobby_ready, swap_req; char name[24];
    unsigned map_size, map_hash, turn_done, map_epoch;
    unsigned force_size, force_hash, force_cost;        // force_cost: what the Skirmish screen priced it at
    int game_build;                                     // build.h: the game build this player has (BUILD_*)
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
    // pressed "next turn"; bit 2: skirmish lobby - the host is ready; bit 3: the map is shared to look at only
    // (battles are being fought, or the turn is past its orders): the joining player sees it, gives no orders,
    // and enters each battle from it; bits 8 and up: a number that changes whenever the operation moves on (a
    // turn ended, a battle was fought) - a joining game whose copy of the map carries another number takes the new map
    unsigned map_flags;
    // A battle that belongs to an operation: battle_of = "campaign:operation, 0, turn" as on the host's battle's
    // command line, extras = the operation's files of what earlier battles left on this ground, packed
    // (count, then per file: name length, name, size, bytes). A joined game's battle is started as a
    // battle of that operation with those files, so that it builds the same scene as the host's.
    char battle_of[64];
    unsigned extras_size, extras_hash;
    // Skirmish lobby: the joining player's own force. force_have: the hash of the one the host holds (0 none),
    // force_refused: of one it turned down (not of the side he commands); force_side: the side the host's
    // battle setup gives him - his force has to be of that side.
    unsigned force_have, force_refused, force_side;
    // what the host's own force costs on its Skirmish screen: the joining player's force may cost as much (0: the
    // host has not chosen his force yet)
    unsigned force_limit;
    // the host's battle setup: its year and season (the Skirmish screen's list positions, plus 1; 0: not known)
    // and the side the host commands - the joining player's own force is picked for that year, season and enemy
    unsigned force_year, force_period, force_enemy;
    int game_build;                                     // build.h: the host's game build (BUILD_*)
};
// Head.flags on the two image packets: 0 = battle description, 1 = operation save ("the map"), 2 = extras.
// PKT_FORCE_REQUEST / PKT_FORCE_DATA (host asks, joining game answers) carry the joining player's force in the same shapes.
struct ImageRequest { Head h; unsigned index; };
struct ImageData { Head h; unsigned index, size; unsigned char data[k_chunk]; };
// Unit transforms from the game that owns those units, plus (host only) the battle clock.
// speed: the rate the host's battle time actually advances at (its speed setting, less whatever the engine's
// automatic slow-down takes off); sim_ms: battle time so far.
// phase: 0 deployment, 1 initial orders, 2 battle, 3 battle over (the value of the engine's "gamemode" broadcast)
// ready: deployment and initial orders end only when both players have pressed the game's Start button;
// 1 = this player has finished deployment, 2 = has finished the initial orders as well.
// want_speed / want_pause: what the sending game's own player has asked of the battle's clock with his speed
// and pause keys. The battle pauses when both players ask for it and runs faster (or slower) than normal
// only as far as both ask for; speed and paused above are what the host's clock makes of the two.
struct StateHead {
    Head h; unsigned snapshot, sim_ms; float speed, cur_speed; unsigned char paused, phase, ready, pad;
    float want_speed; unsigned char want_pause, pad2[3];
};
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
// kind 0: one launch (also off-map fire: no shooter); kind 1: an explosion at a place (launch = the engine's
// record, its two description pointers travelling as weapon / ammo); kind 9: a flare (pos; launch = direction,
// kind, side, last argument); kind 2: one squad digging in (trench.cpp; its values are in `launch`); kind 3, 4: a
// piece of the sender's table of description / object numbers (xlate.cpp); kind 5: a crater the host's game
// has made, kind 6: a trench a game has stamped onto the map outside any dig (trench.cpp);
// kind 14: a fire on open ground the host's game has started (fires.cpp); kind 11/12: the joining player's order
// (orders.cpp), kind 15: his sector, turn or fire-at-ground order (i_formation slots 21-24, orders.cpp);
// kind 16: who rides which vehicle in the deciding game (seats.cpp; arg6 seats of 20 bytes in launch);
// kind 17: who holds which of the map's control cells in the host's game (cells.cpp; arg6 the first, ammo how many);
// kind 18: map objects the host's game has damaged (mapobj.cpp; arg6 of them, 60 bytes each in launch);
// kind 7: a hit on a unit of the sending game (hits.cpp); kind 10: a soldier's condition (hits.cpp, launch = float); kind 8: a side leaving the battle, a ceasefire
// proposed or taken back, the battle over (endgame.cpp; flags says which). launch: the engine's launch record with its pointers blanked - they travel as ammo / weapon /
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
// What the owner's game asks of a soldier's animation (anim.cpp): state, not events - the newest wins. A
// PKT_ANIM datagram is a Head and h.count of these. The numbers are fields of the animation manager's
// record for the soldier: want +0xa8 (the state to be in), play / variant +0x104 / +0x10c, p100, p114, d4,
// a b c +0x11c.., p128, p12c, b136, b139; group = the group of extra animations chosen by name.
struct AnimRec {
    unsigned id;
    unsigned want, play, variant, p100, p114;
    float d4, a, b, c, p128, p12c;
    unsigned char b136, b139, has_group, pad;
    char group[28];
    // how the soldier is turned, from the soldier itself: +0x1e70 the direction it lies in (one of eight),
    // +0x1210 / +0x1214 / +0x1218 the lying body's angles, +0x1eac / +0x1ef8 its heading
    unsigned dir8;
    float lie[3], heading[2];
};
// What a game counts its own player's force and ground as (endgame.cpp): the trigger manager's four numbers
// for the side at the start of the battle and now. State, sent a few times a second; the newest wins.
struct SidePacket { Head h; unsigned side; float begin[4], end[4]; };
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
