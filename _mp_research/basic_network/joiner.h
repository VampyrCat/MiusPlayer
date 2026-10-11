// Joining side of the connection up to the moment the battle starts: find the host, wait until it is in a
// battle, download the battle description, start the game into that battle.
// Non-blocking; poll() is called every frame by the in-game menu and in a loop by mp_join.exe.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "protocol.h"
#include "image.h"
#include "opsave.h"

struct Joiner {
    enum State { IDLE, CONNECTING, WAITING_FOR_BATTLE, RECEIVING, DONE, FAILED };
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
    DWORD last_welcome = 0;
    bool want_map = false;             // what is being received: the operational map instead of a battle
    char map_file[96] = "";
    unsigned map_turns = 0, map_epoch = 0;
    SOCKET sock = INVALID_SOCKET;
    sockaddr_in host = {};
    std::vector<unsigned char> image, have;
    std::vector<DWORD> asked;
    unsigned image_hash = 0, got = 0;
    DWORD last_hello = 0, started = 0;

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

    void cancel()
    {
        close();
        state = IDLE;
    }

    bool start(const char *addr)
    {
        cancel();
        error[0] = 0;
        strcpy_s(address, addr);
        char name[128];
        strcpy_s(name, addr);
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
        got = 0;
        last_hello = 0;
        started = GetTickCount();
        state = CONNECTING;
        return true;
    }

    float progress() const { return have.empty() ? 0 : (float)got / have.size(); }
    unsigned waited_seconds() const { return (GetTickCount() - started) / 1000; }

    void poll()
    {
        if (state != CONNECTING && state != WAITING_FOR_BATTLE && state != RECEIVING) return;
        DWORD now = GetTickCount();
        if (state != RECEIVING && now - last_hello >= 500) {
            Hello hello = { make_head(PKT_HELLO) };
            memcpy(hello.proto, k_protocol, sizeof k_protocol);
            hello.version = k_version;
            hello.prefer_side = prefer_side;
            hello.ready = 0;
            hello.phase = 0xff;         // not in a battle yet
            hello.lobby_ready = lobby_ready;
            hello.swap_req = swap_req;
            strncpy_s(hello.name, my_name, sizeof hello.name - 1);
            sendto(sock, (char *)&hello, sizeof hello, 0, (sockaddr *)&host, sizeof host);
            last_hello = now;
        }
        if (state == RECEIVING) {
            unsigned sent = 0;
            for (unsigned i = 0; i < have.size() && sent < 24; i++) {
                if (have[i] || now - asked[i] < 300) continue;
                ImageRequest r = { make_head(PKT_IMAGE_REQUEST), i };
                r.h.flags = want_map ? 1 : 0;
                sendto(sock, (char *)&r, sizeof r, 0, (sockaddr *)&host, sizeof host);
                asked[i] = now;
                sent++;
            }
        }
        for (int guard = 0; guard < 256; guard++) {
            unsigned char buf[2048];
            sockaddr_in from;
            int flen = sizeof from;
            int n = recvfrom(sock, (char *)buf, sizeof buf, 0, (sockaddr *)&from, &flen);
            if (n <= 0) break;
            if (n < (int)sizeof(Head) || from.sin_addr.s_addr != host.sin_addr.s_addr) continue;
            const Head *h = (const Head *)buf;
            if (h->magic != k_magic) continue;
            if (h->type == PKT_WELCOME && n >= 24 && state != RECEIVING) {
                const Welcome *w = (const Welcome *)buf;
                if (w->version != k_version || n < (int)sizeof(Welcome)) { fail("The host runs a different version of the multiplayer files."); return; }
                last_welcome = now;
                swap = w->swap != 0;
                host_in_battle = w->in_battle != 0;
                memcpy(host_name, w->name, sizeof host_name);
                host_name[sizeof host_name - 1] = 0;
                host_mode = w->host_mode;
                host_side = w->user_side;
                other_side = w->enemy_side;
                versus = w->versus != 0;
                // the host hands over the operational map, or is in a battle
                bool map = !w->in_battle && w->map_size != 0;
                unsigned size = map ? w->map_size : w->image_size;
                if ((!map && !w->in_battle) || !size || !lobby_ready) { state = WAITING_FOR_BATTLE; continue; }
                if (size > (64u << 20)) { fail("The host sent an invalid battle."); return; }
                want_map = map;
                if (map) {
                    memcpy(map_file, w->map_file, sizeof map_file);
                    map_file[sizeof map_file - 1] = 0;
                    map_turns = w->map_turns;
                    map_epoch = w->map_flags >> 8;
                    if (!opsave::safe_file_name(map_file)) { fail("The host sent an invalid operation save."); return; }
                }
                image.assign(size, 0);
                image_hash = map ? w->map_hash : w->image_hash;
                have.assign((size + k_chunk - 1) / k_chunk, 0);
                asked.assign(have.size(), 0);
                got = 0;
                state = RECEIVING;
            } else if (h->type == PKT_IMAGE_DATA && state == RECEIVING && n >= (int)(sizeof(ImageData) - k_chunk)) {
                const ImageData *d = (const ImageData *)buf;
                if (d->h.flags != (want_map ? 1 : 0)) continue;
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
            close();
            if (fnv(image.data(), image.size()) != image_hash) fail("The battle arrived damaged. Try again.");
            else state = DONE;
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
        if (fopen_s(&f, path, "wb") || !f) return fail("Could not save the operation to the profile folder.");
        fwrite(image.data(), 1, image.size(), f);
        fclose(f);
        SetEnvironmentVariableA("MP_ROLE", "join");
        SetEnvironmentVariableA("MP_PEER", address);
        SetEnvironmentVariableA("MP_MAP", "1");
        SetEnvironmentVariableA("MP_HOST_MODE", "operation");
        char epoch[16];
        sprintf_s(epoch, "%u", map_epoch);
        SetEnvironmentVariableA("MP_MAP_EPOCH", epoch);       // which state of the operation this copy is of
        sprintf_s(cmd, "\"%s\\starter_x64.exe\" root\\programs\\opgame.progpack, %s, %s, 0, %u", root, id, op.c_str(), map_turns ? map_turns : opsave::turns(image));
        STARTUPINFOA si = { sizeof si };
        PROCESS_INFORMATION pi;
        if (!CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, root, &si, &pi)) return fail("Could not start the operational map.");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
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
        if (versus && !image::mirror(image)) return fail("The host's battle could not be prepared for the other side.");
        sprintf_s(path, "%s\\%s", root, k_image_path);
        if (fopen_s(&f, path, "wb") || !f) return fail("Could not save the battle to the game folder.");
        fwrite(image.data(), 1, image.size(), f);
        fclose(f);
        SetEnvironmentVariableA("MP_ROLE", "join");
        SetEnvironmentVariableA("MP_PEER", address);
        char code[8] = { 0 };
        memcpy(code, &prefer_side, 4);
        SetEnvironmentVariableA("MP_PREFER", prefer_side ? code : 0);
        SetEnvironmentVariableA("MP_VERSUS", versus ? "1" : "0");
        sprintf_s(cmd, "\"%s\\starter_x64.exe\" root\\programs\\tacgame.progpack, %s, users\\profiles, %s, *:*, 0, 1", root, k_image_path, id);
        STARTUPINFOA si = { sizeof si };
        PROCESS_INFORMATION pi;
        if (!CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, root, &si, &pi)) return fail("Could not start the battle.");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
};
