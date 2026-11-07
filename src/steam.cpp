// Steam: a friend's game is found and joined without an address or a port.
//
// The game runs under Steam and loads Steam's library itself (steam_api64.dll, used by basic_steam.codelib for
// achievements; SDK 1.5x: SteamFriends017, SteamUser021, SteamNetworking006, SteamUtils010). Its plain C
// entry points are used here, looked up by name - a game without Steam, or with another library, simply has
// no Steam joining.
//
//   - A game that hosts says so in its player's "rich presence" (key "gtmf" = "<network version> <kind of
//     game>"), which Steam shows to his friends' games. No address is published.
//   - JOIN A GAME lists the friends who are in this game with that key set. Joining one of them needs only
//     his Steam id.
//   - Steam's own "Join Game" (friends list) works too: the host also sets Steam's key "connect". A friend
//     whose game is running gets Steam's join request (callback 337) with that text; a friend whose game is
//     not running has it started by Steam with the text on its command line. This engine reads its command
//     line as "<program>, <arguments>" and quits at once on anything else, so the text is a whole command
//     line of the menu program: "root\programs\opgame.progpack, +gtmf_join=steam:<host id>" - the menu
//     program does not mind the argument, and the module finds the host's id in it.
//   - The packets are the same ones that otherwise travel over UDP, sent as Steam peer-to-peer datagrams
//     (ISteamNetworking: at most 1200 bytes each, unreliable like UDP - every packet of this module is
//     smaller). Steam finds the way between the two PCs, through its relays if need be: no port has to be
//     open and no router has to be told.
//   - A game accepts a peer-to-peer session from the player it is talking to: the host from anybody who
//     asks while it is hosting (Steam's "session request" callback; the protocol's own hello decides what
//     happens then), from his friends who are in the game, and from the player the game before this one in
//     the chain of programs was talking to (MP_STEAM_PEER); a joining game from its host.
//
// A player reached through Steam is written "steam:<id>" wherever a UDP player is "address:port".
//
// Testing on one PC: one Steam account cannot talk to itself, so MP_STEAM_FAKE=<n> gives a game the made-up
// Steam id n and carries "its" datagrams over UDP on 127.0.0.1 (port 40000 + n); the games then find each
// other through files in the temporary folder. Everything above the six calls that touch Steam is the same.
// MP_STEAM=0 switches Steam joining off.
#include "common.h"
#include "protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- Steam's flat API
#pragma pack(push, 8)
struct FriendGameInfo { u64 game; u32 ip; unsigned short port, query_port; u64 lobby; };
#pragma pack(pop)

static void *(*SteamFriends)(void), *(*SteamUser)(void), *(*SteamNetworking)(void), *(*SteamUtils)(void);
static u64 (*User_GetSteamID)(void *);
static bool (*User_BLoggedOn)(void *);
static u32 (*Utils_GetAppID)(void *);
static const char *(*Friends_GetPersonaName)(void *);
static bool (*Friends_SetRichPresence)(void *, const char *, const char *);
static const char *(*Friends_GetFriendRichPresence)(void *, u64, const char *);
static void (*Friends_RequestFriendRichPresence)(void *, u64);
static int (*Friends_GetFriendCount)(void *, int);
static u64 (*Friends_GetFriendByIndex)(void *, int, int);
static const char *(*Friends_GetFriendPersonaName)(void *, u64);
static bool (*Friends_GetFriendGamePlayed)(void *, u64, FriendGameInfo *);
static bool (*Net_SendP2PPacket)(void *, u64, const void *, u32, int, int);
static bool (*Net_IsP2PPacketAvailable)(void *, u32 *, int);
static bool (*Net_ReadP2PPacket)(void *, void *, u32, u32 *, u64 *, int);
static bool (*Net_AcceptP2PSessionWithUser)(void *, u64);
static bool (*Net_AllowP2PPacketRelay)(void *, bool);
static void (*RegisterCallback)(void *, int);

static void *g_friends, *g_user, *g_net, *g_utils;
static u64 g_me;
static u32 g_app;
static char g_my_name[64];
static int g_state;                 // 0 not looked at, 1 there, 2 not there
static bool g_hosting;
static char g_presence[48];
static DWORD g_last_accept, g_last_request;
static u64 g_stat_sent, g_stat_received, g_stat_refused;
static const char k_key[] = "gtmf";
enum { k_friends_immediate = 0x04, k_send_unreliable = 0, k_channel = 0, k_cb_session_request = 1202, k_cb_session_fail = 1203, k_cb_presence = 336,
       k_cb_join_requested = 337 };
static const char k_connect_key[] = "connect", k_join_tag[] = "+gtmf_join=";
static char g_join_request[48];     // "steam:<id>" from Steam's "Join Game", not yet taken

// ---------------------------------------------------------------- the made-up Steam of the tests
static u64 g_fake;                  // this game's made-up id (0: the real Steam)
static SOCKET g_fake_sock = INVALID_SOCKET;
static DWORD g_fake_written;

static void fake_path(char *out, size_t size, u64 id)
{
    char tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    sprintf_s(out, size, "%sgtmf_fake_steam_%llu.txt", tmp, id);
}

static bool fake_open(void)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_fake_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nonblocking = 1;
    ioctlsocket(g_fake_sock, FIONBIO, &nonblocking);
    DWORD off = 0, ret = 0;
    WSAIoctl(g_fake_sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, 0, 0, &ret, 0, 0);
    int reuse = 1;      // the programs of one game follow each other on the same id
    setsockopt(g_fake_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof reuse);
    sockaddr_in me = {};
    me.sin_family = AF_INET;
    me.sin_port = htons((unsigned short)(40000 + g_fake));
    inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);
    if (bind(g_fake_sock, (sockaddr *)&me, sizeof me)) { closesocket(g_fake_sock); g_fake_sock = INVALID_SOCKET; return false; }
    return true;
}

// ---------------------------------------------------------------- callbacks
// Steam calls back through objects of this shape (steam_api_common.h, CCallbackBase): three virtual
// functions and two fields it fills in. Both forms of Run do the same here, so it does not matter which of
// the two the library uses for a plain callback.
class CallbackBase {
public:
    CallbackBase() { m_nCallbackFlags = 0; m_iCallback = 0; }
    virtual void Run(void *param) = 0;
    virtual void Run(void *param, bool io_failure, u64 call) = 0;
    int GetICallback() { return m_iCallback; }
    virtual int GetCallbackSizeBytes() = 0;
protected:
    unsigned char m_nCallbackFlags;
    int m_iCallback;
};

static bool g_callbacks_seen;

struct SessionRequest : CallbackBase {
    void got(void *param)
    {
        u64 id;
        memcpy(&id, param, 8);
        g_callbacks_seen = true;
        // a host takes the session; what the other game may do then is the protocol's business
        if (g_hosting && g_net) {
            Net_AcceptP2PSessionWithUser(g_net, id);
            mp_log("steam: session asked for by %llu - accepted", id);
        } else {
            g_stat_refused++;
        }
    }
    void Run(void *param) override { got(param); }
    void Run(void *param, bool, u64) override { got(param); }
    int GetCallbackSizeBytes() override { return 8; }
};

struct SessionFail : CallbackBase {
    void got(void *param)
    {
        u64 id;
        memcpy(&id, param, 8);
        mp_log("steam: no connection to %llu could be made (error %u)", id, (unsigned)((unsigned char *)param)[8]);
    }
    void Run(void *param) override { got(param); }
    void Run(void *param, bool, u64) override { got(param); }
    int GetCallbackSizeBytes() override { return 16; }
};

struct PresenceUpdate : CallbackBase {
    void got(void *)
    {
        if (!g_callbacks_seen) mp_log("steam: Steam's callbacks arrive in this program");
        g_callbacks_seen = true;
    }
    void Run(void *param) override { got(param); }
    void Run(void *param, bool, u64) override { got(param); }
    int GetCallbackSizeBytes() override { return 16; }
};

// Steam's "Join Game" on a friend while this game is running: GameRichPresenceJoinRequested_t
// { CSteamID friend; char connect[256]; }.
struct JoinRequested : CallbackBase {
    void got(void *param)
    {
        u64 id;
        memcpy(&id, param, 8);
        char connect[257] = {};
        memcpy(connect, (char *)param + 8, 256);
        g_callbacks_seen = true;
        steam_join_from_text(connect, id);
    }
    void Run(void *param) override { got(param); }
    void Run(void *param, bool, u64) override { got(param); }
    int GetCallbackSizeBytes() override { return 8 + 256; }
};

static SessionRequest g_on_request;
static SessionFail g_on_fail;
static PresenceUpdate g_on_presence;
static JoinRequested g_on_join;

// ---------------------------------------------------------------- start
static bool resolve(HMODULE dll)
{
    struct { void *dest; const char *name; } all[] = {
        { &SteamFriends, "SteamAPI_SteamFriends_v017" }, { &SteamUser, "SteamAPI_SteamUser_v021" },
        { &SteamNetworking, "SteamAPI_SteamNetworking_v006" }, { &SteamUtils, "SteamAPI_SteamUtils_v010" },
        { &User_GetSteamID, "SteamAPI_ISteamUser_GetSteamID" }, { &User_BLoggedOn, "SteamAPI_ISteamUser_BLoggedOn" },
        { &Utils_GetAppID, "SteamAPI_ISteamUtils_GetAppID" },
        { &Friends_GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName" },
        { &Friends_SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence" },
        { &Friends_GetFriendRichPresence, "SteamAPI_ISteamFriends_GetFriendRichPresence" },
        { &Friends_RequestFriendRichPresence, "SteamAPI_ISteamFriends_RequestFriendRichPresence" },
        { &Friends_GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount" },
        { &Friends_GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex" },
        { &Friends_GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName" },
        { &Friends_GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed" },
        { &Net_SendP2PPacket, "SteamAPI_ISteamNetworking_SendP2PPacket" },
        { &Net_IsP2PPacketAvailable, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable" },
        { &Net_ReadP2PPacket, "SteamAPI_ISteamNetworking_ReadP2PPacket" },
        { &Net_AcceptP2PSessionWithUser, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser" },
        { &Net_AllowP2PPacketRelay, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay" },
        { &RegisterCallback, "SteamAPI_RegisterCallback" },
    };
    for (auto &f : all) {
        void *p = (void *)GetProcAddress(dll, f.name);
        if (!p) { mp_log("steam: this game's Steam library has no %s - no Steam joining", f.name); return false; }
        *(void **)f.dest = p;
    }
    return true;
}

bool steam_ready(void)
{
    if (g_state) return g_state == 1;
    g_state = 2;
    char v[32];
    if (GetEnvironmentVariableA("MP_STEAM", v, sizeof v) && !atoi(v)) { mp_log("steam: switched off (MP_STEAM=0)"); return false; }
    if (GetEnvironmentVariableA("MP_STEAM_FAKE", v, sizeof v) && _strtoui64(v, 0, 10)) {
        g_fake = _strtoui64(v, 0, 10);
        if (!fake_open()) { mp_log("steam: test stand-in: UDP port %llu is taken", 40000 + g_fake); return false; }
        g_me = g_fake;
        sprintf_s(g_my_name, "fake%llu", g_fake);
        g_state = 1;
        mp_log("steam: TEST STAND-IN for Steam: this game is 'steam:%llu', its datagrams travel over UDP on this PC", g_fake);
        return true;
    }
    HMODULE dll = GetModuleHandleA("steam_api64.dll");
    if (!dll) { mp_log("steam: the game has not loaded Steam's library - no Steam joining"); return false; }
    if (!resolve(dll)) return false;
    g_friends = SteamFriends(); g_user = SteamUser(); g_net = SteamNetworking(); g_utils = SteamUtils();
    if (!g_friends || !g_user || !g_net || !g_utils) { mp_log("steam: Steam is not running for this game - no Steam joining"); return false; }
    g_me = User_GetSteamID(g_user);
    g_app = Utils_GetAppID(g_utils);
    const char *name = Friends_GetPersonaName(g_friends);
    strncpy_s(g_my_name, name ? name : "", sizeof g_my_name - 1);
    if (!g_me) { mp_log("steam: no Steam user - no Steam joining"); return false; }
    Net_AllowP2PPacketRelay(g_net, true);
    RegisterCallback(&g_on_request, k_cb_session_request);
    RegisterCallback(&g_on_fail, k_cb_session_fail);
    RegisterCallback(&g_on_presence, k_cb_presence);
    RegisterCallback(&g_on_join, k_cb_join_requested);
    Friends_RequestFriendRichPresence(g_friends, g_me);      // (answers with a callback: shows that they arrive)
    g_state = 1;
    mp_log("steam: ready - user %llu (%s), application %u, %d friends, logged on: %s", g_me, g_my_name, g_app,
           Friends_GetFriendCount(g_friends, k_friends_immediate), User_BLoggedOn(g_user) ? "yes" : "no");
    return true;
}

u64 steam_me(void) { return steam_ready() ? g_me : 0; }
const char *steam_my_name(void) { return g_my_name; }
bool steam_fake(void) { return g_fake != 0; }

// ---------------------------------------------------------------- datagrams
void steam_accept(u64 who)
{
    if (!steam_ready() || !who || g_fake) return;
    Net_AcceptP2PSessionWithUser(g_net, who);
}

bool steam_send(u64 to, const void *data, int size)
{
    if (!steam_ready() || !to || size <= 0 || size > 1200) return false;
    g_stat_sent++;
    if (g_fake) {
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)(40000 + to));
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        return sendto(g_fake_sock, (const char *)data, size, 0, (sockaddr *)&a, sizeof a) == size;
    }
    return Net_SendP2PPacket(g_net, to, data, (u32)size, k_send_unreliable, k_channel);
}

// One datagram, or 0 when none is waiting.
int steam_recv(u64 *from, void *buf, int room)
{
    if (!steam_ready()) return 0;
    if (g_fake) {
        sockaddr_in a;
        int alen = sizeof a;
        int n = recvfrom(g_fake_sock, (char *)buf, room, 0, (sockaddr *)&a, &alen);
        if (n <= 0) return 0;
        *from = (u64)ntohs(a.sin_port) - 40000;
        g_stat_received++;
        return n;
    }
    u32 size = 0;
    if (!Net_IsP2PPacketAvailable(g_net, &size, k_channel)) return 0;
    u64 id = 0;
    if (!Net_ReadP2PPacket(g_net, buf, (u32)room, &size, &id, k_channel) || !size) return 0;
    *from = id;
    g_stat_received++;
    return (int)size;
}

// ---------------------------------------------------------------- hosting: told to friends
void steam_host(bool hosting, const char *what)
{
    if (!steam_ready()) return;
    char text[48] = "";
    if (hosting) sprintf_s(text, "%u %s", k_version, what ? what : "game");
    if (hosting == g_hosting && !strcmp(text, g_presence)) return;
    g_hosting = hosting;
    strcpy_s(g_presence, text);
    g_fake_written = 0;
    if (g_fake) {
        if (!hosting) { char path[MAX_PATH]; fake_path(path, sizeof path, g_fake); DeleteFileA(path); }
    } else {
        bool ok = Friends_SetRichPresence(g_friends, k_key, text);
        const char *held = Friends_GetFriendRichPresence(g_friends, g_me, k_key);        // what Steam now holds for this player
        if (!ok || strcmp(held ? held : "", text)) mp_log("steam: Steam did not take the note for friends (asked for '%s', it holds '%s')", text, held ? held : "");
        // Steam's own "Join Game" in the friends list
        char connect[128] = "";
        if (hosting) steam_connect_text(connect, sizeof connect, g_me);
        if (!Friends_SetRichPresence(g_friends, k_connect_key, connect)) mp_log("steam: Steam did not take the \"connect\" text for its Join Game");
    }
    mp_log(hosting ? "steam: friends are told that this game can be joined (%s)" : "steam: no longer shown to friends as a game to join%s", hosting ? text : "");
}

// Every frame of a game that hosts: its friends who are in this game may talk to it.
void steam_frame(void)
{
    if (g_state != 1 || !g_hosting) return;
    DWORD now = GetTickCount();
    if (g_fake) {
        if (now - g_fake_written >= 2000 || !g_fake_written) {      // (the file's age says the host is still there)
            g_fake_written = now ? now : 1;
            char path[MAX_PATH];
            fake_path(path, sizeof path, g_fake);
            FILE *f = 0;
            if (!fopen_s(&f, path, "w") && f) { fprintf(f, "%s\n%s\n", g_presence, g_my_name); fclose(f); }
        }
        return;
    }
    if (now - g_last_accept < 3000) return;
    g_last_accept = now;
    int n = Friends_GetFriendCount(g_friends, k_friends_immediate);
    for (int i = 0; i < n && i < 2000; i++) {
        u64 id = Friends_GetFriendByIndex(g_friends, i, k_friends_immediate);
        FriendGameInfo info = {};
        if (id && Friends_GetFriendGamePlayed(g_friends, id, &info) && (u32)(info.game & 0xffffff) == g_app) Net_AcceptP2PSessionWithUser(g_net, id);
    }
}

// ---------------------------------------------------------------- joining: the friends who host
// Fills `out` with the friends hosting a game of this network version; returns how many.
int steam_hosts(SteamHost *out, int room)
{
    if (!steam_ready()) return 0;
    int count = 0;
    char mine[16];
    sprintf_s(mine, "%u ", k_version);
    if (g_fake) {
        for (u64 id = 1; id <= 8 && count < room; id++) {
            if (id == g_fake) continue;
            char path[MAX_PATH], line[2][64] = {};
            fake_path(path, sizeof path, id);
            WIN32_FILE_ATTRIBUTE_DATA fa;
            FILETIME nowft;
            GetSystemTimeAsFileTime(&nowft);
            if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) continue;
            ULARGE_INTEGER a, b;
            a.LowPart = fa.ftLastWriteTime.dwLowDateTime; a.HighPart = fa.ftLastWriteTime.dwHighDateTime;
            b.LowPart = nowft.dwLowDateTime; b.HighPart = nowft.dwHighDateTime;
            if (b.QuadPart - a.QuadPart > 80000000ull) continue;        // older than 8 s: that host is gone
            FILE *f = 0;
            if (fopen_s(&f, path, "r") || !f) continue;
            for (int i = 0; i < 2 && fgets(line[i], sizeof line[i], f); i++) line[i][strcspn(line[i], "\r\n")] = 0;
            fclose(f);
            if (strncmp(line[0], mine, strlen(mine))) continue;
            out[count].id = id;
            strncpy_s(out[count].name, line[1], sizeof out[count].name - 1);
            strncpy_s(out[count].what, line[0] + strlen(mine), sizeof out[count].what - 1);
            count++;
        }
        return count;
    }
    DWORD now = GetTickCount();
    bool ask = now - g_last_request > 5000;
    if (ask) g_last_request = now;
    int n = Friends_GetFriendCount(g_friends, k_friends_immediate);
    for (int i = 0; i < n && i < 2000 && count < room; i++) {
        u64 id = Friends_GetFriendByIndex(g_friends, i, k_friends_immediate);
        FriendGameInfo info = {};
        if (!id || !Friends_GetFriendGamePlayed(g_friends, id, &info) || (u32)(info.game & 0xffffff) != g_app) continue;
        const char *text = Friends_GetFriendRichPresence(g_friends, id, k_key);
        if (!text || !text[0]) { if (ask) Friends_RequestFriendRichPresence(g_friends, id); continue; }
        if (strncmp(text, mine, strlen(mine))) continue;                // hosting with other multiplayer files
        const char *name = Friends_GetFriendPersonaName(g_friends, id);
        out[count].id = id;
        strncpy_s(out[count].name, name ? name : "?", sizeof out[count].name - 1);
        strncpy_s(out[count].what, text + strlen(mine), sizeof out[count].what - 1);
        count++;
    }
    return count;
}

// ---------------------------------------------------------------- Steam's own "Join Game"
// The text a host gives Steam for its friends' "Join Game": a command line of the menu program (see the top).
void steam_connect_text(char *out, size_t size, u64 host)
{
    sprintf_s(out, size, "root\\programs\\opgame.progpack, %ssteam:%llu", k_join_tag, host);
}

// That text, from the command line or from Steam's join request: the host to join is remembered until the
// menu takes it (steam_join_request). Anything else is ignored.
void steam_join_from_text(const char *text, u64 from)
{
    const char *tag = text ? strstr(text, k_join_tag) : 0;
    u64 id = tag && !_strnicmp(tag + strlen(k_join_tag), "steam:", 6) ? _strtoui64(tag + strlen(k_join_tag) + 6, 0, 10) : 0;
    if (!id) { if (text && text[0]) mp_log("steam: a join request that is not a multiplayer game's ('%.80s') - ignored", text); return; }
    sprintf_s(g_join_request, "steam:%llu", id);
    mp_log("steam: Steam's Join Game asks to join %s%s", g_join_request, from ? " (a friend's game, while this game runs)" : " (on the command line)");
}

bool steam_join_request(char *out, size_t size)
{
    if (!g_join_request[0]) return false;
    strcpy_s(out, size, g_join_request);
    g_join_request[0] = 0;
    return true;
}

// A friend's name by his id ("" when Steam does not know him).
const char *steam_name_of(u64 id)
{
    static char name[64];
    name[0] = 0;
    if (!steam_ready() || !id) return name;
    if (g_fake) sprintf_s(name, "fake%llu", id);
    else if (const char *n = Friends_GetFriendPersonaName(g_friends, id)) strncpy_s(name, n, sizeof name - 1);
    return name;
}

void steam_report(void)
{
    if (!g_stat_sent && !g_stat_received && !g_stat_refused) return;
    mp_log("steam: 5 s: %llu datagrams sent through Steam, %llu received%s", g_stat_sent, g_stat_received, g_fake ? " (test stand-in)" : "");
    g_stat_sent = g_stat_received = g_stat_refused = 0;
}
