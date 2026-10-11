// Shared between the engine-facing object (basic_network.cpp), the network session (session.cpp)
// and the in-game multiplayer menu (ui.cpp).
#pragma once
#include "engine.h"

void mp_log(const char *fmt, ...);
extern char g_out_dir[MAX_PATH];     // <game>\out
extern char g_program[32];           // "opgame" or "tacgame"
HWND main_window(void);
bool readable(const void *p, size_t n);

// Rigid-body state every unit (i_techn, i_human) carries, as offsets into the object:
//   +0x0f0 rotation, 3 rows of 4 floats     +0x130 inverse rotation (transpose)
//   +0x1b0 position x, height, z            +0x1d0 linear velocity     +0x1e0 angular velocity
enum { UNIT_ROT = 0xf0, UNIT_INV = 0x130, UNIT_POS = 0x1b0, UNIT_LIN = 0x1d0, UNIT_ANG = 0x1e0 };
// A unit's saved state starts with its rigid body, object +0xf0..+0x2c0, as the first 0x1d0 bytes of the blob.
// Motion travels in the transform stream instead, so that part is blanked when sending and kept when loading.
enum { UNIT_MOTION_SIZE = 0x1d0 };

// Object manager: +0xf0 array of every live object, +0xfc its length.
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
void session_single_player(void);        // menu: leave multiplayer
void session_note_sim_ms(u32 ms);
void session_note_phase(u32 phase);     // the engine announced a battle phase change
int session_role(void);
int session_host_mode(void);
bool session_connected(void);
bool session_versus(void);
bool session_state_wanted(Object *o);      // false: this unit is followed by position only
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
void session_test_next_turn(void);
void session_test_new_map(void);
void session_test_save(void);
const char *session_status(void);        // banner text, "" outside multiplayer
bool session_owns(Object *o);            // this game is the one that decides what the object does
Object *session_unit(u64 key);           // unit by number (kind << 32 | id), 0 when this game has none
bool session_unit_key(Object *o, u64 *key);
u64 session_battle_ms(void);             // battle time so far
const char *session_local_ip(void);
unsigned short session_port(void);

// ---- full object state through the engine's savegame code (state.cpp)
#ifdef __cplusplus
#include <vector>
bool state_save(Object *o, std::vector<unsigned char> &out);
int state_load(Object *o, const unsigned char *data, size_t size);
bool state_handles_match(Object *o, const unsigned char *data, size_t size);
int state_load_keep_motion(Object *o, const unsigned char *data, size_t size, bool keep_view);
bool state_is_unit(Object *o);
bool state_remote_loading(void);        // a state of the other game is being loaded right now
u64 state_bad_lookups(void);              // node look-ups refused since the last call (see state.cpp)
void state_debug_step(ObjMan *om, u64 sim_ms);
#endif

// ---- replication of full object state (statesync.cpp)
void statesync_owner_frame(ObjMan *om, DWORD now, bool (*owns)(Object *), void (*send)(const void *, int));
void statesync_on_packet(const unsigned char *data, int size, void (*send)(const void *, int), bool versus);
void statesync_apply(ObjMan *om);
void statesync_reset(void);
void statesync_report(void);

// ---- shell launches (shots.cpp)
void shots_install(ObjMan *om);
void shots_frame(void (*send)(const void *, int));
void shots_on_packet(const unsigned char *data, int size, void (*send)(const void *, int));
void shots_sim_step(ObjMan *om);
void shots_reset(void);
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

// ---- the other game's description and object numbers (xlate.cpp)
void xlate_frame(ObjMan *om);
void xlate_received(const ShotRec &s, ObjMan *om);
void xlate_resend(void);
bool xlate_ready(void);                  // the other game's numbers can be translated
u32 xlate_desc(u32 theirs);              // the other game's description number -> this game's
#endif
bool session_owns_squad(u32 id);         // a squad (or unit) id of a squad this game's player commands
u32 session_phase(void);                 // battle phase here: 0 deployment, 1 initial orders, 2 battle, 3 over

// ---- in-game multiplayer menu (ui.cpp)
void ui_start(ObjMan *om);               // once, when the engine starts ticking
void ui_frame(ObjMan *om);               // every frame: input and menu logic
void ui_render(ObjMan *om);              // from the render pipeline, just below the game's cursor
bool ui_blocks_input(void);              // the menu is open: the game must not see mouse or keyboard
