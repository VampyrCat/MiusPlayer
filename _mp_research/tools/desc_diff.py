"""Do two running games number their descriptions the same way?   desc_diff.py <pid host> <pid joiner>

The engine's savegame code writes a pointer to a description (ammunition, weapon, ...) as a number: the
object manager (on the main thread's stack at 0x7FDEF0) keeps a table {number, pointer} sorted by number at
+0x1748 (count at +0x1754), filled in the order the descriptions were registered. A saved unit state from
one game can only be loaded into the other if the same number means the same description in both.
The script compares, number by number, the first 0x60 bytes of what the number points at (values that
look like pointers are left out).
"""
import struct
import sys

import peek

OBJMAN = 0x7FDEF0


def table(pid):
    h = peek.open_proc(pid)
    head = peek.read(h, OBJMAN + 0x1748, 0x10)
    ptr, _, count = struct.unpack_from('<QII', head)
    rows = peek.read(h, ptr, count * 0x10)
    out = {}
    for i in range(count):
        num, _, p = struct.unpack_from('<IIQ', rows, i * 0x10)
        d = peek.read(h, p, 0x60)
        if not d:
            out[num] = None
            continue
        words = struct.unpack('<12Q', d)
        # a pointer differs from run to run: keep only what cannot be one
        out[num] = tuple(w if w < 0x10000 or w > 0x7fffffffffff or (w >> 32) == 0 or (w & 0xffffffff) < 0x10000 else 0 for w in words)
    return out


def main():
    a, b = table(int(sys.argv[1])), table(int(sys.argv[2]))
    common = sorted(set(a) & set(b))
    same = sum(1 for n in common if a[n] == b[n])
    print('host game: %d descriptions, joiner game: %d; numbers in both: %d' % (len(a), len(b), len(common)))
    print('same number, same description: %d; same number, different description: %d' % (same, len(common) - same))
    first = [n for n in common if a[n] != b[n]][:1]
    if first:
        print('first number that differs: %d' % first[0])


if __name__ == '__main__':
    main()
