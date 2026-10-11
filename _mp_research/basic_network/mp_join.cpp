// mp_join.exe <host address>[:port]
// Command-line alternative to the in-game "Join multiplayer game" menu: waits for the host's battle,
// downloads it and starts this copy of the game straight into it. Lives next to starter_x64.exe.
#include "joiner.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: mp_join <host address>[:port] [germans | soviets]\n");
        return 2;
    }
    char root[MAX_PATH];
    GetModuleFileNameA(0, root, MAX_PATH);
    if (char *s = strrchr(root, '\\')) *s = 0;

    Joiner j;
    j.lobby_ready = true;        // no lobby on the command line: enter the battle as soon as there is one
    if (argc > 2) j.prefer_side = !_stricmp(argv[2], "soviets") ? k_side_soviets : k_side_germans;
    if (!j.start(argv[1])) { printf("%s\n", j.error); return 1; }
    printf("Joining %s...\n", argv[1]);
    Joiner::State shown = Joiner::IDLE;
    unsigned noted = 0;
    for (;;) {
        j.poll();
        if (j.state != shown) {
            shown = j.state;
            if (shown == Joiner::WAITING_FOR_BATTLE) printf("Connected. Waiting for the host to start a battle...\n");
            if (shown == Joiner::RECEIVING) printf("Receiving the battle (%zu KB)...\n", j.image.size() / 1024);
        }
        if (j.state == Joiner::CONNECTING && j.waited_seconds() >= noted + 10) {
            noted = j.waited_seconds();
            printf("  no answer yet (%u s) - check the address, and that UDP %u reaches the host\n", noted, ntohs(j.host.sin_port));
        }
        if (j.state == Joiner::FAILED) { printf("%s\n", j.error); return 1; }
        if (j.state == Joiner::DONE) break;
        Sleep(10);
    }
    if (!j.launch(root)) { printf("%s\n", j.error); return 1; }
    printf("Battle received. Starting the game...\n");
    return 0;
}
