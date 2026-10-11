"""Compares the launch logs of two games (MP_LOG_SHOTS=1):  shots_diff.py <host log> <joiner log> [host unit prefix]

Every launch of a shell, bullet, grenade or flare is one line: battle time, what the game did with it
(own: fired by a unit this game owns and sent; received: the other game's launch carried out here; dropped:
a launch this game's own simulation made for a unit of the other game; free: shot replication off, each
game fires for itself), the firing unit, ammunition, start position, direction.

With replication on, every "own" launch of one game should have a "received" twin in the other with the
same numbers. With it off (free), the script pairs each unit's launches by time and reports how many have
no counterpart in the other game and how far the directions of the pairs are apart.
"""
import collections
import math
import re
import sys

LINE = re.compile(r'^(\w) (\d+) (\w+) (\S+) ammo=(\d+) weapon=(\d+) aflags=(\w+) acode=(\w+) flags=(\w+) '
                  r'pos=([-\d.]+),([-\d.]+),([-\d.]+) dir=([-\d.]+),([-\d.]+),([-\d.]+) speed=([-\d.]+)')


def load(path):
    out = []
    for line in open(path, errors='replace'):
        m = LINE.match(line)
        if not m:
            continue
        out.append(dict(kind=m.group(1), ms=int(m.group(2)), who=m.group(3), unit=m.group(4), ammo=int(m.group(5)),
                        weapon=int(m.group(6)), pos=tuple(float(m.group(i)) for i in (10, 11, 12)),
                        dir=tuple(float(m.group(i)) for i in (13, 14, 15)), speed=float(m.group(16))))
    return out


def angle(a, b):
    dot = sum(x * y for x, y in zip(a, b))
    na, nb = math.sqrt(sum(x * x for x in a)), math.sqrt(sum(x * x for x in b))
    if not na or not nb:
        return 0.0
    return math.degrees(math.acos(max(-1.0, min(1.0, dot / (na * nb)))))


def klass(speed):
    return 'under 100 m/s (grenades, flares)' if speed < 100 else 'under 400 m/s (mortars, pistols, howitzers)' if speed < 400 else '400 m/s and over (rifles, machine guns, guns)'


def replicated(a, b, la, lb):
    """own launches of game `la` against received launches of game `lb`"""
    own = [s for s in a if s['kind'] == 'L' and s['who'] == 'own']
    got = collections.Counter((s['unit'], s['ammo'], s['pos'], s['dir']) for s in b if s['kind'] == 'L' and s['who'] == 'received')
    missing = []
    by = collections.Counter()
    for s in own:
        key = (s['unit'], s['ammo'], s['pos'], s['dir'])
        by[klass(s['speed'])] += 1
        if got[key] > 0:
            got[key] -= 1
        else:
            missing.append(s)
    extra = sum(got.values())
    dropped = sum(1 for s in b if s['who'] == 'dropped')
    print('%s fired %d launches of its own units; %s carried out %d of them with identical start and direction, %d missing, %d extra' % (
        la, len(own), lb, len(own) - len(missing), len(missing), extra))
    for k, v in sorted(by.items()):
        print('      %-46s %d' % (k, v))
    print('   %s dropped %d launches its own simulation made for %s\'s units' % (lb, dropped, la))
    for s in missing[:5]:
        print('      missing: %d ms %s ammo %d' % (s['ms'], s['unit'], s['ammo']))
    return len(missing) + extra


def free(a, b, host_prefix):
    units = collections.defaultdict(lambda: ([], []))
    for i, log in enumerate((a, b)):
        for s in log:
            if s['kind'] == 'L' and s['unit'] not in ('-', '?'):
                units[s['unit']][i].append(s)
    total = collections.Counter()
    angles = []
    for unit, (ua, ub) in units.items():
        host_owned = unit.rsplit('_', 1)[1].startswith(host_prefix)
        owner, other = (ua, ub) if host_owned else (ub, ua)
        used = [False] * len(other)
        for s in owner:
            best, at = None, -1
            for i, t in enumerate(other):
                if used[i] or t['ammo'] != s['ammo'] or abs(t['ms'] - s['ms']) > 3000:
                    continue
                if best is None or abs(t['ms'] - s['ms']) < abs(best['ms'] - s['ms']):
                    best, at = t, i
            total['fired by the owner\'s game'] += 1
            if best is None:
                total['  never fired in the other game'] += 1
            else:
                used[at] = True
                total['  also fired in the other game'] += 1
                angles.append(angle(s['dir'], best['dir']))
        total['fired only in the other game (not by the owner)'] += used.count(False)
    for k, v in total.items():
        print('   %-52s %d' % (k, v))
    if angles:
        angles.sort()
        print('   direction of the pairs apart: median %.2f deg, 90%% within %.2f deg, largest %.2f deg; identical in %d of %d' % (
            angles[len(angles) // 2], angles[int(len(angles) * 0.9)], angles[-1], sum(1 for x in angles if x < 0.001), len(angles)))
    return total


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    host_prefix = sys.argv[3] if len(sys.argv) > 3 else '08'
    print('host log: %d launches, joiner log: %d launches, %d + %d explosions without a flight' % (
        sum(s['kind'] == 'L' for s in a), sum(s['kind'] == 'L' for s in b), sum(s['kind'] == 'X' for s in a), sum(s['kind'] == 'X' for s in b)))
    if any(s['who'] == 'free' for s in a + b) and not any(s['who'] == 'own' for s in a + b):
        print('each game fires for itself:')
        free(a, b, host_prefix)
        return
    bad = replicated(a, b, 'host', 'joiner') + replicated(b, a, 'joiner', 'host')
    print('shots: %s' % ('every launch arrived' if not bad else '%d launches differ' % bad))


if __name__ == '__main__':
    main()
