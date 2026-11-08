// Chat in a battle (mpmod2): Enter opens a line at the foot of the screen, typing goes into it (the game gets no keys
// meanwhile), Enter sends it, Esc closes it. Every player sees the last few lines for a while.
//
// A line travels on the ordered channel (ShotRec kind 20: launch = the sender's name, 24 bytes, then the text): a
// joining game sends it to the host, the host shows it and passes it on to every other player (shots_queue_except).
// The key that opens the line is taken away from the game where it would see it: the window's own messages
// (basic_network.cpp focus_wndproc) and raw input (hook_GetRawInputData) - in a battle with another player the game
// never sees Enter. Typing is read like the menu's fields (the keys' state every frame) and turned into characters
// with the keyboard's own layout (ToAscii). MP_CHAT=0 switches it off.
#include "common.h"
#include "protocol.h"
#include <string.h>
#include <deque>
#include <vector>

enum { CHAT = 20, NAME = 24, TEXT = 112, KEEP = 6, SHOW_MS = 15000 };
struct Line { char text[NAME + TEXT + 4]; DWORD at; };
static std::deque<Line> g_lines;
static bool g_open, g_checked, g_enabled = true;
static char g_typed[TEXT];
static unsigned char g_was[256];
static DWORD g_opened_at, g_closed_at;
// Every key press while the line is open, in order, from raw input (basic_network.cpp hook_GetRawInputData) - the
// game reads the keyboard that way. Looking at the keys' state once a frame lost a press that began and ended within
// one slow frame ("typed here" arrived as "typed her"); the keys' state is looked at only when no raw input comes.
static SRWLOCK g_raw_lock = SRWLOCK_INIT;
static std::vector<unsigned> g_raw;          // the key, and (bits 16-19) shift, ctrl, alt, caps lock as they were then

static unsigned modifiers_now(void)
{
    return ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0x10000u : 0) | ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 0x20000u : 0) |
           ((GetAsyncKeyState(VK_MENU) & 0x8000) ? 0x40000u : 0) | ((GetKeyState(VK_CAPITAL) & 1) ? 0x80000u : 0);
}
static bool g_raw_works;

bool chat_available(void)
{
    if (!g_checked) { g_checked = true; char v[8]; if (GetEnvironmentVariableA("MP_CHAT", v, sizeof v)) g_enabled = atoi(v) != 0; }
    return g_enabled && session_role() != ROLE_NONE && session_connected() && !strcmp(g_program, "tacgame");
}

bool chat_is_open(void) { return g_open; }

static void add_line(const char *name, const char *text)
{
    Line l;
    sprintf_s(l.text, "%s: %s", name && *name ? name : "?", text);
    l.at = GetTickCount();
    g_lines.push_back(l);
    while (g_lines.size() > KEEP) g_lines.pop_front();
    mp_log("chat: %s", l.text);
}

static void send_line(const char *raw)
{
    // (printable characters only, no spaces at the end - a test command's line comes with its line end)
    char text[TEXT];
    size_t n = 0;
    for (const char *p = raw; *p && n + 1 < sizeof text; p++) if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f) text[n++] = *p;
    while (n && text[n - 1] == ' ') n--;
    text[n] = 0;
    if (!n) return;
    ShotRec r = {};
    r.kind = CHAT;
    strncpy_s((char *)r.launch, NAME, session_my_name(), NAME - 1);
    strncpy_s((char *)r.launch + NAME, TEXT, text, TEXT - 1);
    shots_queue(r);
    add_line(session_my_name(), text);
}

// Enter where the game would have seen it (the window's messages, raw input): the line opens.
void chat_key_open(void)
{
    if (g_open || !chat_available() || GetTickCount() - g_closed_at < 300) return;     // (the Enter that sent the line, late)
    g_open = true;
    g_typed[0] = 0;
    g_opened_at = GetTickCount();
    g_was[VK_RETURN] = 1;           // (this press does not send the empty line at once)
}

static bool pressed(int vk)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_was[vk];
    g_was[vk] = down;
    return edge;
}

void chat_raw_key(unsigned vk, bool make)
{
    if (!g_open || !make || vk >= 256) return;
    AcquireSRWLockExclusive(&g_raw_lock);
    if (g_raw.size() < 256) g_raw.push_back(vk | modifiers_now());
    g_raw_works = true;
    ReleaseSRWLockExclusive(&g_raw_lock);
}

static void close_line(void) { g_open = false; g_closed_at = GetTickCount(); }

// One key pressed while the line is open: false when it closed the line.
static bool key_in(unsigned key)
{
    int vk = (int)(key & 0xff);
    size_t n = strlen(g_typed);
    if (vk == VK_ESCAPE) { close_line(); return false; }
    if (vk == VK_RETURN) {
        if (n) send_line(g_typed);
        g_typed[0] = 0;
        close_line();
        return false;
    }
    if (vk == VK_BACK) { if (n) g_typed[--n] = 0; return true; }
    BYTE state[256] = {};
    // (shift, ctrl, alt and caps lock as they were when the key was pressed)
    if (key & 0x10000u) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (key & 0x20000u) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
    if (key & 0x40000u) state[VK_MENU] = state[VK_RMENU] = 0x80;
    if (key & 0x80000u) state[VK_CAPITAL] = 1;
    state[vk] |= 0x80;
    WORD out[2] = { 0, 0 };
    int k = ToAscii((UINT)vk, MapVirtualKeyA((UINT)vk, 0), state, out, 0);
    for (int i = 0; i < k && i < 2; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c >= 0x20 && c < 0x7f && n + 1 < sizeof g_typed) { g_typed[n++] = (char)c; g_typed[n] = 0; }
    }
    return true;
}

// Every frame of a battle: the keys while the line is open.
void chat_frame(bool focused)
{
    if (!g_open) return;
    if (!chat_available()) { close_line(); return; }
    if (g_raw_works) {
        std::vector<unsigned> keys;
        AcquireSRWLockExclusive(&g_raw_lock);
        keys.swap(g_raw);
        ReleaseSRWLockExclusive(&g_raw_lock);
        for (unsigned key : keys) if (!key_in(key)) break;
        return;
    }
    if (!focused) return;
    size_t n = strlen(g_typed);
    if (pressed(VK_ESCAPE)) { close_line(); return; }
    if (pressed(VK_RETURN)) {
        while (n && g_typed[n - 1] == ' ') g_typed[--n] = 0;
        if (n) send_line(g_typed);
        g_typed[0] = 0;
        close_line();
        return;
    }
    if (pressed(VK_BACK) && n) g_typed[--n] = 0;
    BYTE state[256];
    if (!GetKeyboardState(state)) return;
    for (int vk = 0x20; vk < 0xf0; vk++) {           // (0xe2: the extra key of European keyboards, < > |)
        if (vk == VK_RETURN || vk == VK_BACK || vk == VK_ESCAPE || vk == VK_PROCESSKEY || vk == VK_PACKET ||
            (vk >= VK_PRIOR && vk <= VK_HELP) || (vk >= VK_LWIN && vk <= VK_SLEEP) ||
            (vk >= VK_F1 && vk <= VK_F24) || (vk >= VK_NUMLOCK && vk <= VK_LMENU + 1) || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
            continue;
        if (!pressed(vk)) continue;
        WORD out[2] = { 0, 0 };
        state[vk] |= 0x80;
        // (the keys held as they are now: the thread's own key state lags when the game reads raw input)
        for (int m : { VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU })
            state[m] = (GetAsyncKeyState(m) & 0x8000) ? 0x80 : 0;
        state[VK_CAPITAL] = (BYTE)(GetKeyState(VK_CAPITAL) & 1);
        int k = ToAscii((UINT)vk, MapVirtualKeyA((UINT)vk, 0), state, out, 0);
        for (int i = 0; i < k && i < 2; i++) {
            unsigned char c = (unsigned char)out[i];
            if (c >= 0x20 && c < 0x7f && n + 1 < sizeof g_typed) { g_typed[n++] = (char)c; g_typed[n] = 0; }
        }
    }
}

// A line from another game (on channel ch: 0 the other player, 1-3 a host's extra players).
void chat_received(const ShotRec &s, int ch)
{
    if (!chat_available()) return;
    char name[NAME], text[TEXT];
    memcpy(name, s.launch, NAME);
    name[NAME - 1] = 0;
    memcpy(text, s.launch + NAME, TEXT);
    text[TEXT - 1] = 0;
    for (char *c = name; *c; c++) if ((unsigned char)*c < 0x20 || (unsigned char)*c >= 0x7f) *c = '?';
    for (char *c = text; *c; c++) if ((unsigned char)*c < 0x20 || (unsigned char)*c >= 0x7f) *c = '?';
    add_line(name, text);
    if (session_role() == ROLE_HOST) {           // (the host passes it on to everyone else)
        ShotRec r = s;
        shots_queue_except(r, ch);
    }
}

// Tests: "say <text>" - a line sent as if typed.
void chat_test_say(const char *text) { if (chat_available() && *text) send_line(text); }

// For drawing: the lines still to show (newest last), and what is being typed (0 when the line is closed).
int chat_lines(const char **out, int max)
{
    DWORD now = GetTickCount();
    int n = 0;
    for (const Line &l : g_lines)
        if ((g_open || now - l.at < SHOW_MS) && n < max) out[n++] = l.text;
    return n;
}
const char *chat_typed(void) { return g_open ? g_typed : 0; }
