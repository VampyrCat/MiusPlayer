"""Asks a multiplayer host what it is doing: probe_host.py [address] [port]
Prints 'menus' when the host is listening but not in a battle, 'battle <bytes>' when it serves a battle,
'none' when nothing answers within three seconds."""
import socket
import struct
import sys

addr = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
port = int(sys.argv[2]) if len(sys.argv) > 2 else 1943
MAGIC, VERSION = 0x504d5447, 7
hello = struct.pack('<IBBH12sIIBBBB24s', MAGIC, 1, 0, 0, b'GTMF_TA_A1', VERSION, 0, 0, 255, 0, 0, b'probe')
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(0.5)
for _ in range(6):
    s.sendto(hello, (addr, port))
    try:
        data, _ = s.recvfrom(2048)
    except (socket.timeout, ConnectionResetError):
        continue
    magic, kind = struct.unpack_from('<IB', data)
    if magic == MAGIC and kind == 2:
        _, _, _, _, proto, version, size, image_hash, user_side, enemy_side, in_battle, mode = struct.unpack_from('<IBBH12sIIIIIBB', data)
        print(('battle %d' % size if in_battle else 'menus') + (' operation' if mode == 2 else ' mission' if mode == 3 else ' skirmish'))
        break
else:
    print('none')
