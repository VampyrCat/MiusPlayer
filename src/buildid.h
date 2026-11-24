// Which game build a game folder holds, read from the files (for mp_join.exe, which runs outside the game; the
// module itself uses build.h). The same stamps as build.cpp: the link stamps of base_shell and base_main.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

inline DWORD buildid_file_stamp(const char *path)
{
    FILE *f = 0;
    if (fopen_s(&f, path, "rb") || !f) return 0;
    IMAGE_DOS_HEADER dos = {};
    IMAGE_NT_HEADERS64 nt = {};
    bool ok = fread(&dos, sizeof dos, 1, f) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE && !fseek(f, dos.e_lfanew, SEEK_SET) &&
              fread(&nt, sizeof nt, 1, f) == 1 && nt.Signature == IMAGE_NT_SIGNATURE;
    fclose(f);
    return ok ? nt.FileHeader.TimeDateStamp : 0;
}

// 0 public_beta-16070931, 1 public-15814271, 2 public_beta-26834468, 3 public_beta-26858192 (the BUILD_* numbers), -1 another
// build
// (2 was public_beta-26779327 until 2026-11-21 20:55: base_shell 0x6afeaeb4, base_main 0x6afeae9d; public_beta-26768241
// until 2026-11-19 11:05: 0x6afe0678, 0x6afe0660; public_beta-26744384
// until 2026-11-19 01:53: 0x6afce297, 0x6afce286; public_beta-26665733
// until 2026-11-18: 0x6af84458, 0x6af8443f; public_beta-26514238
// until 2026-11-14: 0x6aefc0ff, 0x6aefc0f2; before 2026-11-08
// public_beta-26494568: 0x6aee3958, 0x6aee394a)
inline int buildid_of_folder(const char *root)
{
    static const DWORD shell[4] = { 0x670d5899, 0x66ed7c47, 0x6b01a91e, 0x6b03951a }, main[4] = { 0x670d5943, 0x66f2d70e, 0x6b01a90b, 0x6b039504 };
    char path[MAX_PATH];
    sprintf_s(path, "%s\\bin\\rel\\x64\\base_shell.codelib", root);
    DWORD s = buildid_file_stamp(path);
    sprintf_s(path, "%s\\bin\\rel\\x64\\base_main.codelib", root);
    DWORD m = buildid_file_stamp(path);
    for (int b = 0; b < 4; b++) if (s == shell[b] && m == main[b]) return b;
    return -1;
}

inline const char *buildid_name(int b)
{
    return b == 0 ? "2024 beta build" : b == 1 ? "Steam default branch" : b == 2 ? "Steam beta branch" : b == 3 ? "Steam beta branch of 2026-11-23" : "an unknown build";
}
