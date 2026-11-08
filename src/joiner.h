// Joining side of the connection up to the moment the battle starts: find the host, wait until it is in a
// battle, download the battle description, start the game into that battle.
// Non-blocking; poll() is called every frame by the in-game menu and in a loop by mp_join.exe.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "protocol.h"
#include "buildid.h"
#include "image.h"
#include "opsave.h"
#include "force.h"
#include "netsim.h"

// A host reached through Steam ("steam:<id>" in place of an address) instead of over UDP. The game's menu
// puts its Steam functions here; the command-line launcher has none and cannot join that way.
typedef bool (*JoinerSteamSend)(unsigned long long to, const void *data, int size);
typedef int (*JoinerSteamRecv)(unsigned long long *from, void *buf, int room);
inline JoinerSteamSend &joiner_steam_send() { static JoinerSteamSend f; return f; }
inline JoinerSteamRecv &joiner_steam_recv() { static JoinerSteamRecv f; return f; }

// This player's number (Hello.player): made once and kept in the environment, so every program the game starts (the
// menu started again, the battle, the operational map) says it too.
inline unsigned joiner_player_id(void)
{
    static unsigned id;
    if (id) return id;
    char v[16];
    if (GetEnvironmentVariableA("MP_PLAYER_ID", v, sizeof v)) id = strtoul(v, 0, 16);
    if (!id) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        id = ((unsigned)t.QuadPart ^ (unsigned)(t.QuadPart >> 32) ^ (GetCurrentProcessId() * 2654435761u)) | 1;
        sprintf_s(v, "%08x", id);
        SetEnvironmentVariableA("MP_PLAYER_ID", v);
    }
    return id;
}

struct Joiner {
    enum State { IDLE, CONNECTING, WAITING_FOR_BATTLE, RECEIVING, DONE, FAILED };
    bool steam = false;                // the host is reached through Steam
    unsigned long long steam_id = 0;
    State state = IDLE;
    char error[160] = "";
    char address[128] = "";
    unsigned char host_mode = 0;
    unsigned prefer_side = 0;          // side code the player wants to command; set before start()
    unsigned host_side = 0, other_side = 0;
    bool versus = false;               // decided by the host from prefer_side
    char my_name[24] = "Player", host_name[24] = "";
    bool lobby_ready = false;          // this player pressed READY; the battle is only entered when set
    unsigned char swap_req = 0;        // number of SWAP SIDES presses
    bool swap = false, host_in_battle = false;
    bool host_ready = false;           // skirmish: the host has pressed start on his Skirmish screen (Welcome.map_flags bit 2)
    DWORD last_welcome = 0;
    bool want_map = false;             // what is being received: the operational map instead of a battle
    bool want_extras = false;          // ... or, after the battle, the files of the operation it belongs to
    char battle_of[64] = "";           // "campaign:operation, 0, turn" of the host's battle ("" none)
    unsigned extras_size = 0, extras_hash = 0;
    std::vector<unsigned char> battle_image, extras;
    char map_file[96] = "";
    unsigned map_turns = 0, map_epoch = 0;
    bool map_view = false;             // the map is the host's to look at only (Welcome.map_flags bit 3: battles are fought)
    int game_build = -1;               // this game's build (BUILD_* of build.h / buildid.h); the host's must be the same
    SOCKET sock = INVALID_SOCKET;
    sockaddr_in host = {};
    NetSim sim;                        // (tests: a poor connection, see netsim.h)
    std::vector<unsigned char> image, have;
    std::vector<DWORD> asked;
    unsigned image_hash = 0, got = 0;
    DWORD last_hello = 0, started = 0;
    // skirmish: the force this player picked on his own Skirmish screen (a whole battle description; empty: none)
    std::vector<unsigned char> force;
    unsigned force_hash = 0, force_have = 0, force_refused = 0, force_side = 0, force_limit = 0, force_cost = 0;
    unsigned force_year = 0, force_period = 0, force_enemy = 0;     // the host's setup (Welcome)

    void set_force(const std::vector<unsigned char> &f)
    {
        force = f;
        force_hash = force.empty() ? 0 : fnv(force.data(), force.size());
    }

    bool fail(const char *why)
    {
        strcpy_s(error, why);
        close();
        state = FAILED;
        return false;
    }

    void close()
    {
        if (sock != INVALID_SOCKET) closesocket(sock);
        sock = INVALID_SOCKET;
    }

    // Goodbye to the host: this game is done with it (the battle or map downloaded, or the player left) - the host
    // frees its place at once instead of after 8 s of silence (a game that came back in the meantime was told the
    // host had all its players).
    void bye()
    {
        if (state != CONNECTING && state != WAITING_FOR_BATTLE && state != RECEIVING) return;
        if (!steam && sock == INVALID_SOCKET) return;
        Head h = make_head(PKT_BYE);
        xmit(&h, sizeof h);
    }

    void cancel()
    {
        bye();
        close();
        state = IDLE;
    }

    bool start(const char *addr)
    {
        cancel();
        sim.init();
        sim.clear();
        error[0] = 0;
        strcpy_s(address, addr);
        steam = !_strnicmp(addr, "steam:", 6);
        steam_id = steam ? _strtoui64(addr + 6, 0, 10) : 0;
        if (steam && (!steam_id || !joiner_steam_send() || !joiner_steam_recv()))
            return fail("Join through Steam from the game's menu, with Steam running.");
        char name[128];
        strcpy_s(name, steam ? "127.0.0.1" : addr);        // (through Steam there is no address; the socket stays unused)
        unsigned short port = k_default_port;
        if (char *colon = strchr(name, ':')) { *colon = 0; port = (unsigned short)atoi(colon + 1); }
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        host = {};
        host.sin_family = AF_INET;
        host.sin_port = htons(port);
        if (!name[0]) return fail("Enter the host's address first.");
        if (inet_pton(AF_INET, name, &host.sin_addr) != 1) {
            addrinfo hints = {}, *res = 0;
            hints.ai_family = AF_INET;
            if (getaddrinfo(name, 0, &hints, &res) || !res) return fail("That address could not be found.");
            host.sin_addr = ((sockaddr_in *)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        u_long nonblocking = 1;
        ioctlsocket(sock, FIONBIO, &nonblocking);
        DWORD off = 0, ret = 0;
        WSAIoctl(sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, 0, 0, &ret, 0, 0);
        image.clear();
        have.clear();
        battle_image.clear();
        extras.clear();
        want_extras = false;
        got = 0;
        last_hello = 0;
        started = GetTickCount();
        state = CONNECTING;
        return true;
    }

    void xmit(const void *data, int size)
    {
        if (steam) joiner_steam_send()(steam_id, data, size);
        else sendto(sock, (const char *)data, size, 0, (sockaddr *)&host, sizeof host);
    }

    float progress() const { return have.empty() ? 0 : (float)got / have.size(); }
    unsigned waited_seconds() const { return (GetTickCount() - started) / 1000; }

    // (tests) everything waiting goes into the simulated poor connection, poll() takes it from there when due
    void pump()
    {
        unsigned char buf[2048];
        for (int guard = 0; guard < 4096; guard++) {
            sockaddr_in from = {};
            unsigned long long who = 0;
            int n;
            if (steam) n = joiner_steam_recv()(&who, buf, sizeof buf);
            else { int flen = sizeof from; n = recvfrom(sock, (char *)buf, sizeof buf, 0, (sockaddr *)&from, &flen); }
            if (n <= 0) break;
            sim.put(from, who, buf, n);
        }
    }

    void poll()
    {
        if (state != CONNECTING && state != WAITING_FOR_BATTLE && state != RECEIVING) return;
        DWORD now = GetTickCount();
        if (state != RECEIVING && now - last_hello >= 500) {
            Hello hello = { make_head(PKT_HELLO) };
            memcpy(hello.proto, k_protocol, sizeof k_protocol);
            hello.version = k_version;
            hello.game_build = game_build;
            hello.prefer_side = prefer_side;
            hello.ready = 0;
            hello.phase = 0xff;         // not in a battle yet
            hello.lobby_ready = lobby_ready;
            hello.swap_req = swap_req;
            strncpy_s(hello.name, my_name, sizeof hello.name - 1);
            hello.force_size = (unsigned)force.size();
            hello.force_hash = force_hash;
            hello.force_cost = force_cost;
            hello.player = joiner_player_id();
            xmit(&hello, sizeof hello);
            last_hello = now;
        }
        if (state == RECEIVING) {
            unsigned sent = 0;
            for (unsigned i = 0; i < have.size() && sent < 24; i++) {
                if (have[i] || now - asked[i] < 300) continue;
                ImageRequest r = { make_head(PKT_IMAGE_REQUEST), i };
                r.h.flags = want_map ? 1 : want_extras ? 2 : 0;
                xmit(&r, sizeof r);
                asked[i] = now;
                sent++;
            }
        }
        if (sim.on) pump();
        for (int guard = 0; guard < 256; guard++) {
            unsigned char buf[2048];
            sockaddr_in from;
            int flen = sizeof from;
            int n;
            if (sim.on) {
                unsigned long long who = 0;
                n = sim.get(&from, &who, buf, sizeof buf);
                if (n <= 0) break;
                if (steam ? who != steam_id : from.sin_addr.s_addr != host.sin_addr.s_addr) continue;
            } else if (steam) {
                unsigned long long who = 0;
                n = joiner_steam_recv()(&who, buf, sizeof buf);
                if (n <= 0) break;
                if (who != steam_id) continue;
            } else {
                n = recvfrom(sock, (char *)buf, sizeof buf, 0, (sockaddr *)&from, &flen);
                if (n <= 0) break;
                if (from.sin_addr.s_addr != host.sin_addr.s_addr) continue;
            }
            if (n < (int)sizeof(Head)) continue;
            const Head *h = (const Head *)buf;
            if (h->magic != k_magic) continue;
            if (h->type == PKT_WELCOME && n >= 24 && state != RECEIVING) {
                const Welcome *w = (const Welcome *)buf;
                if (w->version != k_version || n < (int)sizeof(Welcome)) { fail("The host has another version of the mod - both need the same."); return; }
                if (w->map_flags & 16) { fail("The host's game has all its players. (Just left it? Try again in 10 seconds.)"); return; }
                if (w->game_build != game_build) {
                    char why[160];
                    _snprintf_s(why, _TRUNCATE, "The host has the %s, you the %s - both need the same (Steam: Properties, Betas).",
                                buildid_name(w->game_build), buildid_name(game_build));
                    fail(why);
                    return;
                }
                last_welcome = now;
                swap = w->swap != 0;
                host_in_battle = w->in_battle != 0;
                host_ready = (w->map_flags & 4) != 0;
                memcpy(host_name, w->name, sizeof host_name);
                host_name[sizeof host_name - 1] = 0;
                host_mode = w->host_mode;
                host_side = w->user_side;
                other_side = w->enemy_side;
                versus = w->versus != 0;
                force_have = w->force_have;
                force_refused = w->force_refused;
                force_side = w->force_side;
                force_limit = w->force_limit;
                force_year = w->force_year;
                force_period = w->force_period;
                force_enemy = w->force_enemy;
                // the host hands over the operational map, or is in a battle
                bool map = !w->in_battle && w->map_size != 0;
                unsigned size = map ? w->map_size : w->image_size;
                if ((!map && !w->in_battle) || !size || !lobby_ready) { state = WAITING_FOR_BATTLE; continue; }
                if (size > (64u << 20)) { fail("The host sent an invalid battle."); return; }
                want_map = map;
                want_extras = false;
                extras_size = map ? 0 : w->extras_size;
                extras_hash = w->extras_hash;
                memcpy(battle_of, w->battle_of, sizeof battle_of);
                battle_of[sizeof battle_of - 1] = 0;
                if (extras_size > (64u << 20)) { fail("The host sent an invalid battle."); return; }
                if (map) {
                    memcpy(map_file, w->map_file, sizeof map_file);
                    map_file[sizeof map_file - 1] = 0;
                    map_turns = w->map_turns;
                    map_epoch = w->map_flags >> 8;
                    map_view = (w->map_flags & 8) != 0;
                    if (!opsave::safe_file_name(map_file)) { fail("The host sent an invalid operation save."); return; }
                }
                image.assign(size, 0);
                image_hash = map ? w->map_hash : w->image_hash;
                have.assign((size + k_chunk - 1) / k_chunk, 0);
                asked.assign(have.size(), 0);
                got = 0;
                state = RECEIVING;
            } else if (h->type == PKT_FORCE_REQUEST && n >= (int)sizeof(ImageRequest) && !force.empty()) {
                unsigned index = ((const ImageRequest *)buf)->index;
                if ((size_t)index * k_chunk >= force.size()) continue;
                ImageData d = { make_head(PKT_FORCE_DATA), index, 0 };
                d.size = (unsigned)min((size_t)k_chunk, force.size() - (size_t)index * k_chunk);
                memcpy(d.data, force.data() + (size_t)index * k_chunk, d.size);
                xmit(&d, (int)(sizeof d - k_chunk + d.size));
            } else if (h->type == PKT_IMAGE_DATA && state == RECEIVING && n >= (int)(sizeof(ImageData) - k_chunk)) {
                const ImageData *d = (const ImageData *)buf;
                if (d->h.flags != (want_map ? 1 : want_extras ? 2 : 0)) continue;
                if (d->index >= have.size() || have[d->index] || d->size > k_chunk) continue;
                if ((size_t)d->index * k_chunk + d->size > image.size() || n < (int)(sizeof(ImageData) - k_chunk + d->size)) continue;
                memcpy(image.data() + (size_t)d->index * k_chunk, d->data, d->size);
                have[d->index] = 1;
                got++;
            }
        }
        // a host that stops answering is looked for again
        if (state == WAITING_FOR_BATTLE && now - last_welcome > 6000) { state = CONNECTING; started = now; host_name[0] = 0; }
        if (state == RECEIVING && got == have.size()) {
            if (fnv(image.data(), image.size()) != image_hash) { close(); fail("The battle arrived damaged. Try again."); }
            else if (!want_map && !want_extras && extras_size) {
                // the battle is here; now the files of the operation it belongs to
                battle_image.swap(image);
                want_extras = true;
                image.assign(extras_size, 0);
                image_hash = extras_hash;
                have.assign((extras_size + k_chunk - 1) / k_chunk, 0);
                asked.assign(have.size(), 0);
                got = 0;
            } else {
                if (want_extras) { extras.swap(image); image.swap(battle_image); want_extras = false; }
                bye();
                close();
                state = DONE;
            }
        }
    }

    // Is there an operation of this name in the game's data? Operations are files
    // <pack>\shared\camps\<campaign>\<name>.campack2, where <pack> is a folder one or two levels below
    // data\ (data\oprum, data\addons\rage_bridge, data\common\ma_croat_leg, ...).
    static bool operation_in(const char *dir, const char *campaign, const char *name, int depth)
    {
        char find[MAX_PATH], path[MAX_PATH];
        sprintf_s(path, "%s\\shared\\camps\\%s\\%s.campack2", dir, campaign, name);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return true;
        if (depth <= 0) return false;
        WIN32_FIND_DATAA fd;
        sprintf_s(find, "%s\\*", dir);
        HANDLE h = FindFirstFileA(find, &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            if (sprintf_s(path, "%s\\%s", dir, fd.cFileName) > 0) found = operation_in(path, campaign, name, depth - 1);
        } while (!found && FindNextFileA(h, &fd));
        FindClose(h);
        return found;
    }

    static bool operation_exists(const char *root, const char *campaign, const char *name)
    {
        char data[MAX_PATH];
        sprintf_s(data, "%s\\data", root);
        return operation_in(data, campaign, name, 2);
    }

    // The operational map has arrived (the host's operation save, with the command tags turned round):
    // put it in the profile as that operation's save and start the game straight on the operation's map.
    bool launch_map(const char *root)
    {
        char path[MAX_PATH], keep[MAX_PATH], id[64] = "cloud", text[512] = "", cmd[1024];
        sprintf_s(path, "%s\\users\\profiles\\cur_profile.engcfg", root);
        FILE *f = 0;
        if (!fopen_s(&f, path, "rb") && f) {
            text[fread(text, 1, sizeof text - 1, f)] = 0;
            fclose(f);
            const char *p = strstr(text, "cur_id");
            if (p && (p = strchr(p, '='))) {
                for (p++; *p == ' ' || *p == '\t'; p++) {}
                size_t k = strcspn(p, ";\r\n");
                if (k && k < sizeof id) { memcpy(id, p, k); id[k] = 0; }
            }
        }
        std::string name = opsave::operation_name(image);
        std::string op = opsave::operation_id(map_file, name);
        if (op.empty()) return fail("The host's operation save could not be recognised.");
        // The same operation from this player's side, if this game has it: own territory, fog of war over
        // the host's forces. The host's state is then used as that operation's save.
        std::string other = opsave::counterpart(name), campaign = op.substr(0, op.find(':'));
        if (!other.empty() && operation_exists(root, campaign.c_str(), other.c_str())) {
            opsave::set_operation_name(image, other);
            sprintf_s(map_file, "%s_%s.opsave", campaign.c_str(), other.c_str());
            op = campaign + ":" + other;
        }
        sprintf_s(path, "%s\\users\\profiles\\%s\\%s", root, id, map_file);
        // an own save of the same operation is kept
        sprintf_s(keep, "%s\\users\\mp_saves", root);
        CreateDirectoryA(keep, 0);
        sprintf_s(keep, "%s\\users\\mp_saves\\_replaced", root);
        CreateDirectoryA(keep, 0);
        sprintf_s(keep, "%s\\users\\mp_saves\\_replaced\\%s", root, map_file);
        if (GetFileAttributesA(keep) == INVALID_FILE_ATTRIBUTES) CopyFileA(path, keep, TRUE);
        if (fopen_s(&f, path, "wb") || !f) return fail("Could not save the operation.");
        fwrite(image.data(), 1, image.size(), f);
        fclose(f);
        SetEnvironmentVariableA("MP_ROLE", "join");
        SetEnvironmentVariableA("MP_PEER", address);
        SetEnvironmentVariableA("MP_MAP", "1");
        SetEnvironmentVariableA("MP_HOST_MODE", "operation");
        char epoch[16];
        sprintf_s(epoch, "%u", map_epoch);
        SetEnvironmentVariableA("MP_MAP_EPOCH", epoch);       // which state of the operation this copy is of
        SetEnvironmentVariableA("MP_MAP_VIEW", map_view ? "1" : 0);  // a copy to look at while the turn's battles are fought
        sprintf_s(cmd, "\"%s\\starter_x64.exe\" root\\programs\\opgame.progpack, %s, %s, 0, %u", root, id, op.c_str(), map_turns ? map_turns : opsave::turns(image));
        STARTUPINFOA si = { sizeof si };
        PROCESS_INFORMATION pi;
        if (!CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, root, &si, &pi)) return fail("Could not start the operational map.");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }

    // The profile a joined battle of an operation runs under. The battle reads the operation's files from
    // its profile folder and writes them again when it ends; they are the HOST's operation, so they live
    // in a folder of their own and the player's real profile and saves are never touched.
    static const char *guest_profile() { return "mp_guest"; }

    // An operation's file of what a piece of ground keeps between battles: <campaign>_<operation>_<ground>.damsav2.
    // The 2026 beta (basic_game+0xf44b0) also keeps the file from before the last battle, <name>.damsav2.prev,
    // and loads that one when the battle is fought again or the newer file is broken (it writes through
    // <name>.damsav2.tmp); the 2024 builds have neither.
    static bool safe_name(const char *name, size_t n)
    {
        if (!n || n > 120) return false;
        for (size_t i = 0; i < n; i++) {
            char c = name[i];
            if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
        }
        if (n > 13 && !_strnicmp(name + n - 13, ".damsav2.prev", 13)) n -= 5;
        return n > 8 && !_strnicmp(name + n - 8, ".damsav2", 8) && name[0] != '.';
    }

    // Puts the operation's files into the guest profile (emptied first), next to copies of the player's own
    // settings - `own` is his profile - so that the battle looks and handles as he has set his game up.
    // False: the files are not usable.
    bool unpack_extras(const char *root, const char *own)
    {
        char dir[MAX_PATH], path[MAX_PATH], from[MAX_PATH];
        sprintf_s(dir, "%s\\users\\profiles\\%s", root, guest_profile());
        CreateDirectoryA(dir, 0);
        WIN32_FIND_DATAA fd;
        for (const char *pattern : { "*.cfgpack", "*.tcsprofile" }) {
            sprintf_s(path, "%s\\users\\profiles\\%s\\%s", root, own, pattern);
            HANDLE h = FindFirstFileA(path, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                sprintf_s(from, "%s\\users\\profiles\\%s\\%s", root, own, fd.cFileName);
                sprintf_s(path, "%s\\%s", dir, fd.cFileName);
                CopyFileA(from, path, FALSE);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        for (const char *pattern : { "*.damsav2", "*.damsav2.prev", "*.damsav2.tmp" }) {      // (the last two: 2026 beta)
            sprintf_s(path, "%s\\%s", dir, pattern);
            HANDLE h = FindFirstFileA(path, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do { sprintf_s(path, "%s\\%s", dir, fd.cFileName); DeleteFileA(path); } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        size_t pos = 4;
        unsigned count;
        if (extras.size() < 4) return false;
        memcpy(&count, extras.data(), 4);
        if (count > 4096) return false;
        for (unsigned i = 0; i < count; i++) {
            if (pos + 1 > extras.size()) return false;
            size_t n = extras[pos++];
            if (pos + n + 4 > extras.size() || !safe_name((const char *)extras.data() + pos, n)) return false;
            char name[128];
            memcpy(name, extras.data() + pos, n);
            name[n] = 0;
            pos += n;
            unsigned size;
            memcpy(&size, extras.data() + pos, 4);
            pos += 4;
            if (pos + size > extras.size()) return false;
            sprintf_s(path, "%s\\%s", dir, name);
            FILE *f = 0;
            if (fopen_s(&f, path, "wb") || !f) return false;
            fwrite(extras.data() + pos, 1, size, f);
            fclose(f);
            pos += size;
        }
        return true;
    }

    static bool safe_battle_of(const char *s)
    {
        if (!strchr(s, ':') || !strncmp(s, "*:*", 3)) return false;
        for (const char *c = s; *c; c++)
            if (!(isalnum((unsigned char)*c) || *c == '_' || *c == ':' || *c == ',' || *c == ' ')) return false;
        return true;
    }

    // Writes the received battle where the game expects it and starts the game straight into it, with the
    // multiplayer module told to follow the host. `root` is the game folder.
    bool launch(const char *root)
    {
        char path[MAX_PATH], id[64] = "cloud", text[512] = "", cmd[1024];
        sprintf_s(path, "%s\\users\\profiles\\cur_profile.engcfg", root);
        FILE *f = 0;
        if (!fopen_s(&f, path, "rb") && f) {
            text[fread(text, 1, sizeof text - 1, f)] = 0;
            fclose(f);
            const char *p = strstr(text, "cur_id");
            if (p && (p = strchr(p, '='))) {
                for (p++; *p == ' ' || *p == '\t'; p++) {}
                size_t k = strcspn(p, ";\r\n");
                if (k && k < sizeof id) { memcpy(id, p, k); id[k] = 0; }
            }
        }
        // commanding the host's opponent: same battle, with the command of the two forces exchanged
        if (versus && !image::mirror(image)) return fail("The battle could not be prepared for your side.");
        sprintf_s(path, "%s\\%s", root, k_image_path);
        if (fopen_s(&f, path, "wb") || !f) return fail("Could not save the battle.");
        fwrite(image.data(), 1, image.size(), f);
        fclose(f);
        SetEnvironmentVariableA("MP_ROLE", "join");
        SetEnvironmentVariableA("MP_PEER", address);
        char code[8] = { 0 };
        memcpy(code, &prefer_side, 4);
        SetEnvironmentVariableA("MP_PREFER", prefer_side ? code : 0);
        SetEnvironmentVariableA("MP_VERSUS", versus ? "1" : "0");
        // The last three items name the operation the battle belongs to ("campaign:operation, 0, turn"); a
        // battle that belongs to none has "*:*, 0, 1". MP_JOIN_BATTLE_OF (an experiment) gives the joined
        // game's battle the host's operation.
        char of[160] = "*:*, 0, 1";
        // The host's battle belongs to an operation: so does this one, with the operation's files in the
        // guest profile. Without that the two games build different scenes - the host's has what earlier
        // battles left behind - and number everything in them differently (MP_JOIN_PLAIN=1: as before).
        char plain[8];
        bool as_before = GetEnvironmentVariableA("MP_JOIN_PLAIN", plain, sizeof plain) && atoi(plain);
        if (!as_before && !extras.empty() && safe_battle_of(battle_of) && _stricmp(id, guest_profile()) && unpack_extras(root, id)) {
            strcpy_s(of, battle_of);
            strcpy_s(id, guest_profile());
        }
        GetEnvironmentVariableA("MP_JOIN_BATTLE_OF", of, sizeof of);
        GetEnvironmentVariableA("MP_JOIN_PROFILE", id, sizeof id);
        sprintf_s(cmd, "\"%s\\starter_x64.exe\" root\\programs\\tacgame.progpack, %s, users\\profiles, %s, %s", root, k_image_path, id, of);
        STARTUPINFOA si = { sizeof si };
        PROCESS_INFORMATION pi;
        if (!CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, root, &si, &pi)) return fail("Could not start the battle.");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
};
