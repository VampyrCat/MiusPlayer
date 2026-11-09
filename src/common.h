// Shared between the engine-facing object (basic_network.cpp), the network session (session.cpp)
// and the in-game multiplayer menu (ui.cpp).
#pragma once
#include "engine.h"

void mp_log(const char *fmt, ...);

// Where the module's own time goes (milliseconds summed per part, reported by session.cpp every 5 s):
//   double t = prof_now(); ...; t = prof_add(PROF_X, t); ...   (plain calls: usable in functions with __try)
enum ProfPart { PROF_RECEIVE, PROF_STATE_SAVE, PROF_STATE_LOAD, PROF_SHOTS, PROF_HITS, PROF_TRENCH, PROF_ANIM, PROF_VDAMAGE,
                PROF_SEATS, PROF_CELLS, PROF_MAPOBJ, PROF_FOLLOW, PROF_GUNS, PROF_TRANSFORMS, PROF_SIGHT, PROF_OTHER, PROF_UI, PROF_N };
extern double g_prof_part[PROF_N];
inline double prof_now(void)
{
    static double ms_per_tick;
    LARGE_INTEGER t;
    if (!ms_per_tick) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); ms_per_tick = 1000.0 / (double)f.QuadPart; }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * ms_per_tick;
}
inline double prof_add(int part, double since) { double now = prof_now(); g_prof_part[part] += now - since; return now; }
extern char g_out_dir[MAX_PATH];     // <game>\out
extern char g_program[32];           // "opgame" or "tacgame"
HWND main_window(void);
bool readable(const void *p, size_t n);
void readable_new_step(void);        // a new frame or simulation step: what readable() found is looked at again after 0.5 s
void readable_forget(void);          // the game's objects changed: everything is looked at again
size_t readable_span(const void *p, size_t n);     // the bytes of n at p that can be read now (the system asked, no cache)
unsigned readable_calls(void);       // the system calls readable() has made since the last time asked (the 5 s report)

// Rigid-body state every unit (i_techn, i_human) carries, as offsets into the object:
//   +0x0f0 rotation, 3 rows of 4 floats     +0x130 inverse rotation (transpose)
//   +0x1b0 position x, height, z            +0x1d0 linear velocity     +0x1e0 angular velocity
// public_beta-26494568 moved the whole block by +0x10 (adv_physic, the units' shared methods: slot 11 returns
// +0x1c0 there (+0x1b0 before), slot 13 +0x1e0 (+0x1d0); the impulse routine, slot 15 (+0xa8870; 2024 +0x61dc0),
// reads the inverse at +0x140 (+0x130) and the centre at +0x1c0 (+0x1b0) and adds to +0x1e0 / +0x1f0 (+0x1d0 /
// +0x1e0); the point-velocity routines, slots 12 and 19 (+0xa8800, +0xcd750; +0x61d50, +0x617d0), read the rows at
// +0x100..+0x120 (+0xf0..+0x110)). They are values of the running build, not constants.
#define UNIT_ROT BF(0xf0, 0x100)
#define UNIT_INV BF(0x130, 0x140)
#define UNIT_POS BF(0x1b0, 0x1c0)
#define UNIT_LIN BF(0x1d0, 0x1e0)
#define UNIT_ANG BF(0x1e0, 0x1f0)
// A unit's saved state starts with its rigid body, object UNIT_ROT .. UNIT_ROT + 0x1d0 (+0xf0..+0x2c0; beta26
// +0x100..+0x2d0), as the first 0x1d0 bytes of the blob: the base save's first field table (adv_physic +0xd7e90;
// beta26 +0x178360, +0x1785c0, +0x178490) has the same 0x1d0 bytes of fields there in both, 0x10 further on.
// Motion travels in the transform stream instead, so that part is blanked when sending and kept when loading.
enum { UNIT_MOTION_SIZE = 0x1d0 };

// Object manager: +0xf0 array of every live object, +0xfc its length (the values of its class:name map).
// The same in public_beta-26494568: its find (slot 9, base_shell +0x17a20) and broadcast to all (slot 12,
// +0x17ae0) go through +0xf0 / +0xfc.
inline Object **all_objects(ObjMan *om, u32 *count)
{
    *count = *(u32 *)((char *)om + 0xfc);
    return *(Object ***)((char *)om + 0xf0);
}

// ---- network session (session.cpp), driven from the engine callbacks of the i_network object
enum { ROLE_NONE, ROLE_HOST, ROLE_JOIN };
void session_start(ObjMan *om);          // once, when the engine starts ticking; reads the environment
void session_frame(ObjMan *om);          // every rendered frame
void session_sim_step(ObjMan *om);       // before every simulation step (tactical only)
void session_stop(void);
void session_host(int host_mode);        // menu: become host now
void session_set_port(unsigned short port);      // menu: the port to host on (0: the default)
void session_set_upnp(bool on);          // menu: ask the router to pass that port on to this PC (UPnP)
int session_upnp(void);                  // 0 off, 1 asking the router, 2 the router has opened the port, 3 it has not
const char *session_public_ip(void);     // the public address the router gave with it ("" unknown)
void session_single_player(void);        // menu: leave multiplayer
void session_note_sim_ms(u32 ms);
void session_note_phase(u32 phase);     // the engine announced a battle phase change
int session_role(void);
int session_host_mode(void);
bool session_connected(void);
bool session_resumable(void);            // a battle's connection is lost and still waited for (see session.cpp g_lost_at)
void session_test_netcut(unsigned seconds);     // tests: throw away everything received for a while
// The round trip to the other game, taken from acknowledgements (unit states, the ordered channel) of
// what was sent only once: smoothed, how much it wanders, and the smallest of the last 30 s (the line's own
// delay without anything queued on it). 0 before the first sample.
void net_rtt_sample(u32 ms);
float net_rtt_ms(void);
float net_rtt_dev_ms(void);
float net_rtt_min_ms(void);
void net_rtt_reset(void);
bool session_versus(void);
bool session_state_wanted(Object *o);      // false: this unit is followed by position only
bool session_other_sees(Object *o);        // false: host, versus - a unit of the host's side the other player has not spotted
void session_test_resign(void);     // tests: leave the battle the way the game's own "to the operation" box does
void session_test_ready(void);      // tests: same as this player pressing Start
const char *session_my_name(void);
bool session_peer(char *name, size_t size, bool *ready);     // the other player, if connected
bool session_swap(void);                 // lobby: the players have exchanged forces (skirmish)
void session_toggle_swap(void);
bool session_on_op_map(ObjMan *om);      // an operation's map is on screen
void session_map_give(ObjMan *om);       // hand the operational map to the other player (host) / back (joining game)
void session_map_take_back(void);
bool session_map_given(void);
bool session_map_mode(void);             // this program is the joining player's turn on the map
bool session_map_arriving(void);         // host: the map is coming back
bool session_map_blocking(void);         // the operational map takes no clicks for a moment
bool session_map_view(void);             // joining game: the map is only to look at (the host fights the turn's battles)
void session_test_next_turn(void);
void session_test_start_battle(void);       // tests: the host's map goes into its selected battle ("startbattle")
void session_test_new_map(void);
void session_test_save(void);
const char *session_status(void);        // banner text, "" outside multiplayer
const char *session_status_line(void);   // ... what is going on, without who plays (the player panel's status line)
// The player panel (ui.cpp): who plays - the host first, then the others in the order they joined. tag: "host,
// Germans, ready"; color: the player's own (0 the host's .. 4); you: this game's own player.
struct PanelPlayer { char name[24], tag[48]; int color; bool connected, you; };
int session_players(PanelPlayer *out, int max);
void session_mode_line(char *out, size_t size);  // "Operation - Stepanovka - 1943", "Skirmish - Stepanovka (aut) - 1942"
float session_fps(void);                 // frames this game draws a second
float session_latency_ms(void);          // round trip to the host / (host) to the first player; 0 not known
int session_interface_hidden(void);      // the battle's interface hidden with Ctrl+Backspace: 1, 0, -1 not known here
bool session_owns(Object *o);            // this game is the one that decides what the object does
u32 session_units_generation(void);      // changes whenever the units session_unit finds may have changed
Object *session_unit(u64 key);           // unit by number (kind << 32 | id), 0 when this game has none
bool session_unit_key(Object *o, u64 *key);
u64 session_battle_ms(void);             // battle time so far
void session_drawn_probe(void);          // tests (MP_TEST_DRAWN=1): a moving unit's place at every drawn frame
const char *session_local_ip(void);
unsigned short session_port(void);

// ---- full object state through the engine's savegame code (state.cpp)
#ifdef __cplusplus
#include <vector>
bool state_save(Object *o, std::vector<unsigned char> &out);
int state_load(Object *o, const unsigned char *data, size_t size);
bool state_handles_match(Object *o, const unsigned char *data, size_t size);
bool state_handles_translatable(Object *o);        // its state can be loaded although the other game numbers handles differently
void state_translate_report(void);
void state_wheels_report(void);                 // 5 s: vehicle states loaded with this game's wheels kept
DWORD state_loaded_at(const Object *o);         // when the unit last had a state of the other game loaded (0: never)
bool state_local_handle(Object *o, u32 *handle);   // a handle of this unit as the other game numbers it -> as this game does
int state_load_keep_motion(Object *o, const unsigned char *data, size_t size, bool keep_view);
bool state_is_unit(Object *o);
bool state_remote_loading(void);        // a state of the other game is being loaded right now
u64 state_bad_lookups(void);              // node look-ups refused since the last call (see state.cpp)
void state_debug_step(ObjMan *om, u64 sim_ms);
void state_test_guard(void);              // tests: the render guards with a bad handle
#endif

// ---- replication of full object state (statesync.cpp)
void statesync_owner_frame(ObjMan *om, DWORD now, bool (*owns)(Object *), void (*send)(const void *, int));
void statesync_on_packet(const unsigned char *data, int size, void (*send)(const void *, int), bool versus);
void statesync_apply(ObjMan *om);
void statesync_reset(void);
void statesync_owner_frame_extra(int ch, ObjMan *om, DWORD now, bool (*owns)(Object *), void (*send)(int, const void *, int));
void statesync_on_packet_extra(int ch, const unsigned char *data, int size);
void statesync_open_extra(int ch);
void statesync_close_extra(int ch);
void statesync_report(void);

// ---- shell launches (shots.cpp)
void shots_install(ObjMan *om);
void shots_frame(void (*send)(const void *, int));
void shots_on_packet(const unsigned char *data, int size, void (*send)(const void *, int));
void shots_sim_step(ObjMan *om);
void shots_reset(void);
// more players (a host's extra players, session.cpp): channels 1-3 of the reliable channel
int shots_target(int ch);               // records queued from now on go to channel ch only (-1: to all); returns the last
void shots_frame_extra(int ch, void (*send)(int, const void *, int));
void shots_on_packet_extra(int ch, const unsigned char *data, int size, void (*send)(int, const void *, int));
void shots_open_channel(int ch);
void shots_close_channel(int ch);
void trench_send_all_to(int ch);
void xlate_send_to(int ch);
struct ShotRec;
void shots_queue_except(ShotRec &s, int ch);     // a record to every channel but ch (the host passing a chat line on)
// chat in a battle (chat.cpp)
bool chat_available(void);
bool chat_is_open(void);
void chat_key_open(void);
void chat_raw_key(unsigned vk, bool make);      // a key from raw input while the line is open (basic_network.cpp)
void chat_frame(bool focused);
void chat_received(const ShotRec &s, int ch);
void chat_test_say(const char *text);
int chat_lines(const char **out, int max);
const char *chat_typed(void);
void session_set_order_sender(int ch);  // the orders carried out now came from channel ch (0: the other player)
int session_order_sender(void);
void shots_report(void);
#ifdef __cplusplus
struct ShotRec;
void shots_queue(ShotRec &s);            // sends a record on the shots' reliable, ordered channel

// ---- trenches (trench.cpp)
void trench_install(void);
void trench_received(const ShotRec &s);
void trench_sim_step(ObjMan *om);
void trench_resend(void);                // a new connection: this game's digs once more
void trench_report(void);
void trench_test_move(int squads, float metres);
void trench_early(void);
void crater_received(const ShotRec &s);

// ---- hits (hits.cpp)
void hits_sim_step(ObjMan *om);
void hits_received(const ShotRec &s);
void hits_report(void);
void conditions_received(const ShotRec &s);   // a soldier's condition, his owner's (ShotRec.kind 10)
void conditions_resend(void);

// ---- the end of a battle: withdrawing, ceasefire, the battle over (endgame.cpp)
void endgame_frame(ObjMan *om, void (*send)(const void *, int));
void endgame_on_packet(const unsigned char *data, int size);
void endgame_received(const ShotRec &s);
void endgame_resend(void);               // a new connection: where this game stands, once more
void endgame_manual(bool on);            // the calls in between come from the player's own Withdraw button
bool endgame_over(void);
int endgame_truce(void);                 // 0: no ceasefire to be had; 4 | 1 this player has proposed one | 2 the other player has
float endgame_countdown(void);           // seconds until the battle ends, below zero: nothing is counting
int endgame_leaving(void);               // 1: this player's side is leaving the battle, 2: the other side
void endgame_toggle_truce(void);
void shots_idle(void);                   // every frame: records that cannot wait for a simulation step (the game is paused)

// ---- Steam: finding and joining a friend's game without an address (steam.cpp)
struct SteamHost { u64 id; char name[64]; char what[32]; };
bool steam_ready(void);                  // Steam is there for this game (or its stand-in for tests)
u64 steam_me(void);
bool steam_fake(void);
void steam_host(bool hosting, const char *what);     // tell friends this game can be joined / no longer
void steam_frame(void);                  // every frame of a hosting game
int steam_hosts(SteamHost *out, int room);           // the friends who host a game right now
const char *steam_name_of(u64 id);
void steam_connect_text(char *out, size_t size, u64 host);      // what a host gives Steam's "Join Game"
void steam_join_from_text(const char *text, u64 from);          // a "Join Game" text (command line or Steam's request)
bool steam_join_request(char *out, size_t size);                // "steam:<id>" asked for by Steam's "Join Game", once
void steam_accept(u64 who);              // this player may open a session with this game
bool steam_send(u64 to, const void *data, int size);
int steam_recv(u64 *from, void *buf, int room);      // one datagram, 0: none waiting
void steam_report(void);

// ---- soldier animation (anim.cpp)
void anim_sim_step(ObjMan *om);
void anim_frame(void (*send)(const void *, int));
void anim_on_packet(const unsigned char *data, int size);
void anim_reset(void);
void anim_report(void);
void stamp_received(const ShotRec &s);
bool session_versus_or_coop(void);       // a battle with another player in it
bool session_simulating(void);           // this game has finished building its battle and is stepping it
std::vector<u32> session_own_squads(void);

// ---- the other game's description and object numbers (xlate.cpp)
void xlate_frame(ObjMan *om);
void xlate_received(const ShotRec &s, ObjMan *om);
void xlate_resend(void);
bool xlate_ready(void);                  // the other game's numbers can be translated
u32 xlate_desc(u32 theirs);              // the other game's description number -> this game's
#endif
bool session_owns_squad(u32 id);         // a squad (or unit) id of a squad whose simulation this game decides
bool session_commands_squad(u32 id);     // ... of a squad this game's player gives orders to
bool session_peer_commands(u64 key);     // host: a unit the joining player commands
bool session_host_auth(void);            // mpmod2: the host's game owns every unit in a battle
bool session_owns_side(u32 side);
unsigned session_planned_joiner_side(void);      // skirmish lobby (host): the side its setup gives the other player
bool session_peer_force(unsigned *squads, unsigned *side);      // ... and whether it holds his own force
void session_set_force_limit(unsigned cost);     // skirmish lobby (host): what its own force costs - the other player's limit
void session_set_setup_sides(unsigned own, unsigned enemy);    // ... and the two sides its Skirmish screen has
void session_set_setup_date(unsigned year, unsigned period);   // ... and its year and season (list positions)
void session_preset_skirmish(unsigned own, unsigned enemy, unsigned year1, unsigned period1);  // joining: the screen opens on these
void session_restore_skirmish_setup(bool forget);              // ... and the profile's own setup is put back
unsigned session_force_limit(void);
void session_set_host_ready(bool on);    // skirmish lobby (host): pressed start on the Skirmish screen - ready
bool session_host_ready(void);
bool session_peer_ready_to_start(void);  // ... the other player is READY and his own force (if any) has arrived
void session_prepare_battle(const char *description);   // host, menu program: the battle made before it is started
unsigned session_peer_force_cost(void);        // this game decides what a side (not a unit) does: off-map fire, flares
bool session_other_units_centre(float *out);     // tests: the middle of the units this game does not own
void shots_test_flares(int n);           // tests: flares over this player's first squad
void shots_test_blowup(int vehicles);   // tests: crews of this player's vehicles blow them up
void shots_test_artillery(int rounds, int kind);   // tests: this player's squads with off-map support call it in
u32 session_phase(void);                 // battle phase here: 0 deployment, 1 initial orders, 2 battle, 3 over
bool session_menu_open(void);            // the game's own ESC menu is open over the battle
void session_game_menu(ObjMan *om, bool show);
void session_test_withdraw(void);        // tests: the ESC menu's Withdraw, confirmed
void session_test_menu(bool show);       // tests: the ESC menu
void session_test_pause(void);           // tests: the pause key
void session_test_speed(int level);      // tests: the speed keys
void session_test_time(void);            // tests: the battle time into the log

// ---- battle statistics, the host's (stats.cpp)
#ifdef __cplusplus
void stats_install(ObjMan *om);
void stats_received(const ShotRec &s, ObjMan *om);    // ShotRec.kind 13
void stats_frame(void);
void stats_report(void);
#endif

// ---- vehicle damage, the host's numbers (vdamage.cpp)
void vdamage_frame(ObjMan *om, void (*send)(const void *, int));
void vdamage_on_packet(const unsigned char *data, int size);
void vdamage_sim_step(ObjMan *om);
void vdamage_reset(void);
void vdamage_report(void);
#ifdef __cplusplus
void vdamage_track_received(const ShotRec &s);     // ShotRec.kind 19: a track the host's game broke
#endif
void vdamage_test_step(ObjMan *om);
void vdamage_test_tracks(void);           // tests: which tracks are broken here (and a hash)
void vdamage_test_break(int n);           // tests: the n-th vehicle's first track broken by the game's own function
void vdamage_test_repair(void);           // tests: the first broken track's repair made complete (the game's step repairs it)
void guns_frame(ObjMan *om, void (*send)(const void *, int));     // guns.cpp: where vehicles' guns point
void guns_on_packet(const unsigned char *data, int size);
void guns_sim_step(ObjMan *om);
void guns_reset(void);
void guns_report(void);
void fires_sim_step(ObjMan *om);          // fires.cpp: fires on open ground are the host's
void fires_received(const ShotRec &s, ObjMan *om);
void fires_report(void);

// ---- the joining player's orders, carried out by the host's game (orders.cpp)
#ifdef __cplusplus
void orders_install(ObjMan *om);
void orders_received(const ShotRec &s);  // ShotRec.kind 11 / 12
void orders_reset(void);
void orders_report(void);
void orders_test_move(int index, float dx, float dz);   // tests: this player's n-th squad moves by dx, dz
void orders_test_where(u32 squad);        // tests: where a squad stands here
void orders_test_place_vehicle(int index, float dx, float dz);   // tests: the n-th squad with a vehicle, wheels and all
void orders_test_wheel_gap(ObjMan *om, u32 squad);               // tests: how far vehicles' wheels are from their bodies (0: all)
void orders_test_place(int index, float dx, float dz);  // tests: this player's n-th squad put elsewhere at once (deployment)
void orders_test_aim(int slot, int index, float dx, float dz, float deg);   // tests: an i_formation slot 21-24 order for this player's n-th squad
void orders_test_attack(void);            // tests: all of this player's squads move against the other side
void orders_test_selectable(void);        // tests: how many of this player's units can be selected
#endif

// ---- who rides which vehicle, the deciding game's (seats.cpp)
#ifdef __cplusplus
void seats_sim_step(ObjMan *om, bool apply);    // apply: the other game's seats may be taken here now
void seats_received(const ShotRec &s);   // ShotRec.kind 16
void seats_reset(void);                  // a new connection: every seat is sent again
void seats_report(void);
void seats_test_list(void);               // tests: how many soldiers ride here, and a hash of who rides where
void seats_test_move(int index);          // tests: the n-th seated soldier out of his seat; below 0: the last one back in

// ---- who holds the ground: the map's control cells, the host's (cells.cpp)
void cells_sim_step(ObjMan *om);
void cells_received(const ShotRec &s);   // ShotRec.kind 17
void cells_flush(void);                  // host: the changes not sent yet, now; joining game: the host's cells, now
void cells_reset(void);                  // a new connection: every cell is sent again
void cells_report(void);
void cells_test_step(void);
void cells_test_list(void);               // tests: how many cells each side holds here, and a hash of the owners
void cells_test_capture(int index);       // tests: the n-th held cell to the other side, by the game's own capture

// ---- the map's objects (buildings, fences, trees): what happens to them is the host's (mapobj.cpp)
void mapobj_sim_step(ObjMan *om);
void mapobj_received(const ShotRec &s);  // ShotRec.kind 18
void mapobj_reset(void);                 // a new connection: every damaged object is sent again
void mapobj_report(void);
void mapobj_test_step(void);
void mapobj_test_list(void);              // tests: how many map objects are damaged / destroyed here, and a hash
void mapobj_test_list_all(void);          // tests: the same, and each damaged object (at most 120)
void mapobj_test_demolish(int index);     // tests: the n-th destructible building collapses (the game's own hit)

// ---- what each player sees of the enemy: the host's spotting (sight.cpp)
void sight_frame(ObjMan *om, void (*send)(const void *, int), void (*send_opposite)(const void *, int));
void sight_on_packet(const unsigned char *data, int size);
void sight_report(void);
int sight_other_sees(u64 key);            // host: the other player's side has spotted this unit (1), not (0), not known (-1)
void sight_test_step(void);
void sight_test_list(void);               // tests: the list sent (host) / the side list here (joining game), count and hash
#endif

// ---- in-game multiplayer menu (ui.cpp)
void ui_start(ObjMan *om);               // once, when the engine starts ticking
void ui_frame(ObjMan *om);               // every frame: input and menu logic
void ui_render(ObjMan *om);              // from the render pipeline, just below the game's cursor
bool ui_blocks_input(void);              // the menu is open: the game must not see mouse or keyboard
bool ui_blocks_clicks(void);             // the map is only to look at: the game must not see mouse clicks
