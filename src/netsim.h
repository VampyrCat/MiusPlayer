// Tests only: a poor connection between two games on one PC. Every datagram this game RECEIVES is held back
// MP_TEST_LAG milliseconds plus up to MP_TEST_JITTER more at random (so some overtake others), and
// MP_TEST_LOSS percent of them (fractions allowed, e.g. 2.5) are thrown away; MP_TEST_BURST=n makes a loss
// take the next n-1 datagrams with it (losses come in bursts on real lines). MP_TEST_KBPS=n: the line carries
// n KB a second - what comes faster waits in a queue (a home router's) of MP_TEST_QUEUE milliseconds (250 by
// default) and what does not fit is thrown away, as on an upload that is too slow for what is sent. Set in both
// games, each direction gets it. None set: nothing is held, the receive loops work as before.
// Used by the session (session.cpp) and by the joining side before the battle (joiner.h, also mp_join.exe).
// The shown loss counts the datagrams thrown away here only, not what a real line lost.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>

struct NetSim {
    struct Dgram { sockaddr_in from; unsigned long long who; std::vector<unsigned char> data; };
    bool read = false, on = false;
    unsigned lag = 0, jitter = 0, loss = 0, burst = 1;     // loss: per 10,000
    unsigned kbps = 0, queue_ms = 250;
    double line_free = 0;                                    // (MP_TEST_KBPS) when the line has sent what is queued, ms
    unsigned overflowed = 0;
    unsigned long long rng = 0;
    unsigned burst_left = 0;
    std::multimap<unsigned long long, Dgram> q;              // by the time it is handed on (GetTickCount64)
    unsigned dropped = 0, passed = 0;

    void init()
    {
        if (read) return;
        read = true;
        char v[32];
        if (GetEnvironmentVariableA("MP_TEST_LAG", v, sizeof v)) lag = (unsigned)max(atoi(v), 0);
        if (GetEnvironmentVariableA("MP_TEST_JITTER", v, sizeof v)) jitter = (unsigned)max(atoi(v), 0);
        if (GetEnvironmentVariableA("MP_TEST_LOSS", v, sizeof v)) { double p = atof(v); loss = p <= 0 ? 0 : p >= 100 ? 10000 : (unsigned)(p * 100 + 0.5); }
        if (GetEnvironmentVariableA("MP_TEST_BURST", v, sizeof v)) burst = (unsigned)max(atoi(v), 1);
        if (GetEnvironmentVariableA("MP_TEST_KBPS", v, sizeof v)) kbps = (unsigned)max(atoi(v), 0);
        if (GetEnvironmentVariableA("MP_TEST_QUEUE", v, sizeof v)) queue_ms = (unsigned)max(atoi(v), 1);
        on = lag || jitter || loss || kbps;
        rng = (GetTickCount64() * 0x9E3779B97F4A7C15ull) ^ ((unsigned long long)GetCurrentProcessId() << 32) ^ 0x2545F4914F6CDD1Dull;
        if (!rng) rng = 1;
    }
    unsigned next()
    {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (unsigned)(rng >> 16);
    }
    // a datagram just received: lost, or held until it is due
    void put(const sockaddr_in &from, unsigned long long who, const void *data, int n)
    {
        if (burst_left) { burst_left--; dropped++; return; }
        if (loss && next() % 10000 < loss) { burst_left = burst - 1; dropped++; return; }
        unsigned long long now = GetTickCount64(), at = now;
        if (kbps) {
            // the line sends one datagram after the other (28 bytes of IP and UDP headers each)
            double start = line_free > (double)now ? line_free : (double)now;
            if (start - (double)now > queue_ms) { overflowed++; dropped++; return; }
            line_free = start + (n + 28) * 1000.0 / (kbps * 1024.0);
            at = (unsigned long long)line_free;
        }
        unsigned long long due = at + lag + (jitter ? next() % (jitter + 1) : 0);
        Dgram d = { from, who, std::vector<unsigned char>((const unsigned char *)data, (const unsigned char *)data + n) };
        q.emplace(due, std::move(d));
    }
    // the next datagram that is due: its size, 0 none
    int get(sockaddr_in *from, unsigned long long *who, void *buf, int room)
    {
        if (q.empty() || q.begin()->first > GetTickCount64()) return 0;
        auto it = q.begin();
        int n = (int)it->second.data.size();
        if (n > room) n = room;
        memcpy(buf, it->second.data.data(), n);
        if (from) *from = it->second.from;
        if (who) *who = it->second.who;
        q.erase(it);
        passed++;
        return n;
    }
    void clear() { q.clear(); burst_left = 0; line_free = 0; }
};

// Each receiving socket has its own: the session's is here, a Joiner holds one for its socket.
inline NetSim &netsim() { static NetSim s; s.init(); return s; }
