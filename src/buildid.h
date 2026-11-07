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

// 0 public_beta-16070931, 1 public-15814271, 2 public_beta-26494568 (the BUILD_* numbers), -1 another build
inline int buildid_of_folder(const char *root)
{
    static const DWORD shell[3] = { 0x670d5899, 0x66ed7c47, 0x6aee3958 }, main[3] = { 0x670d5943, 0x66f2d70e, 0x6aee394a };
    char path[MAX_PATH];
    sprintf_s(path, "%s\\bin\\rel\\x64\\base_shell.codelib", root);
    DWORD s = buildid_file_stamp(path);
    sprintf_s(path, "%s\\bin\\rel\\x64\\base_main.codelib", root);
    DWORD m = buildid_file_stamp(path);
    for (int b = 0; b < 3; b++) if (s == shell[b] && m == main[b]) return b;
    return -1;
}

inline const char *buildid_name(int b)
{
    return b == 0 ? "2024 beta build" : b == 1 ? "Steam default branch" : b == 2 ? "Steam beta branch" : "an unknown build";
}
