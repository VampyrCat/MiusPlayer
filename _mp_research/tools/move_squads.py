"""Test helper: repositions squads in a running game the way a player does during deployment.
   move_squads.py <pid> <unit id prefix, e.g. 10> <how many squads> <metres>

Every unit of the chosen squads is shifted by the same amount (so the squad keeps its shape) and turned a
quarter circle about the vertical. A unit's rigid body: rotation rows at +0xf0 (3 x 4 floats), its
transpose at +0x130, position at +0x1b0.
"""
import struct
import sys

import objs
import peek
import poke


def main():
    pid, prefix, count, shift = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
    h, hw = peek.open_proc(pid), poke.open_rw(pid)
    squads = {}
    for p, name, _ in objs.objects(h):
        if name.startswith('i_human') or name.startswith('i_techn'):
            uid = name.rsplit('_', 1)[1]
            if uid.startswith(prefix):
                squads.setdefault(int(uid, 16) & 0x7fffffe0, []).append((p, name))
    chosen = sorted(squads)[:count]
    for n, squad in enumerate(chosen):
        dx, dz = shift * (1 if n % 2 == 0 else -1), shift * 0.6
        for p, name in squads[squad]:
            pos = list(struct.unpack('<3f', peek.read(h, p + 0x1b0, 12)))
            pos[0] += dx
            pos[2] += dz
            poke.write(hw, p + 0x1b0, struct.pack('<3f', *pos))
            rot = struct.unpack('<12f', peek.read(h, p + 0xf0, 48))
            rows = [rot[0:3], rot[4:7], rot[8:11]]
            # a quarter turn about the vertical: x' = z, z' = -x for every row
            turned = [(r[2], r[1], -r[0]) for r in rows]
            poke.write(hw, p + 0xf0, b''.join(struct.pack('<4f', *(t + (0.0,))) for t in turned))
            inv = [(turned[0][c], turned[1][c], turned[2][c]) for c in range(3)]
            poke.write(hw, p + 0x130, b''.join(struct.pack('<4f', *(t + (0.0,))) for t in inv))
        print('squad %08x: %d units shifted by %.0f, %.0f m and turned' % (squad, len(squads[squad]), dx, dz))


if __name__ == '__main__':
    main()
