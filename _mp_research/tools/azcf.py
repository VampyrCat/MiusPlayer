"""Reader/patcher for GRAVITECH3 binary configs (AZCF) and program packs (AZP3).

Layout (all offsets absolute within one AZCF blob):
  header 0x40:  'AZCF', node count, node table offset (0x40), ?, hash region offset, name[0x20]
  node   0x80:  name[0x20], type (1 struct / 2 table), columns, rows, type-string offset,
                data offset, child-index offset, child count, hash size, hash offset
  struct row:   cell[0x20] + field name[0x20]
  table  row:   `columns` cells of 0x20
  cell:         u32 len, u32 value-or-string-offset, u32, u32, u32 kind (2 = string) ...
"""
import struct
import sys

NODE = 0x80
CELL = 0x20


def pack_entries(d):
    """Entries of an AZP3 pack: (name, kind, offset, size)."""
    assert d[:4] == b'AZP3'
    n = struct.unpack_from('<I', d, 4)[0]
    out = []
    for i in range(n):
        o = 0xc0 + i * 0x80
        off, size = struct.unpack_from('<QQ', d, o + 8)
        name = d[o + 0x20:o + 0x40].split(b'\0')[0].decode()
        kind = d[o + 0x40:o + 0x60].split(b'\0')[0].decode()
        out.append((name, kind, off, size))
    return out


class Azcf:
    def __init__(self, blob):
        assert blob[:4] == b'AZCF'
        self.b = bytearray(blob)
        self.count, self.table = struct.unpack_from('<II', self.b, 4)

    def node(self, i):
        o = self.table + i * NODE
        f = struct.unpack_from('<10I', self.b, o + 0x20)
        return dict(index=i, pos=o, name=self.b[o:o + 0x20].split(b'\0')[0].decode(),
                    type=f[0], cols=f[1], rows=f[2], types=f[3], data=f[4],
                    kids=f[5], nkids=f[6], hsize=f[7], hash=f[8])

    def find(self, name):
        for i in range(self.count):
            n = self.node(i)
            if n['name'] == name:
                return n
        raise KeyError(name)

    def cstr(self, off):
        return self.b[off:self.b.index(b'\0', off)].decode('latin1')

    def cell(self, off):
        ln, val, a, b, kind = struct.unpack_from('<5I', self.b, off)
        if kind == 2:
            return self.cstr(val) if val else ''
        return '<len=%d val=0x%x %x %x kind=%d>' % (ln, val, a, b, kind)

    def rows(self, n):
        out = []
        if n['type'] == 1:
            for r in range(n['rows']):
                o = n['data'] + r * 0x40
                out.append((self.b[o + CELL:o + 0x40].split(b'\0')[0].decode(), self.cell(o)))
        else:
            for r in range(n['rows']):
                o = n['data'] + r * CELL * n['cols']
                out.append(tuple(self.cell(o + c * CELL) for c in range(n['cols'])))
        return out

    def dump(self, out=sys.stdout):
        for i in range(self.count):
            n = self.node(i)
            print('[%d] %s  type=%d cols=%d rows=%d types=%r kids=%d' % (
                i, n['name'], n['type'], n['cols'], n['rows'], self.cstr(n['types']), n['nkids']), file=out)
            for r in self.rows(n):
                print('     ', r, file=out)

    def append_string_row(self, name, text):
        """Append one string row to a single-column table by relocating the table to the end of the blob."""
        n = self.find(name)
        assert n['type'] == 2 and n['cols'] == 1
        old = bytes(self.b[n['data']:n['data'] + n['rows'] * CELL])
        while len(self.b) % 16:
            self.b.append(0)
        s_off = len(self.b)
        raw = text.encode('latin1')
        self.b += raw + b'\0'
        while len(self.b) % 16:
            self.b.append(0)
        new = len(self.b)
        self.b += old + struct.pack('<8I', len(raw), s_off, 0, 0, 2, 0, 0, 0)
        struct.pack_into('<I', self.b, n['pos'] + 0x28, n['rows'] + 1)
        struct.pack_into('<I', self.b, n['pos'] + 0x30, new)


    def set_field(self, node, field, value):
        """Set a numeric/bool field of a struct node. Floats are stored as their bit pattern."""
        n = self.find(node)
        assert n['type'] == 1
        for r in range(n['rows']):
            o = n['data'] + r * 0x40
            if self.b[o + CELL:o + 0x40].split(b'\0')[0].decode() == field:
                kind = struct.unpack_from('<I', self.b, o + 16)[0]
                assert kind in (0, 4, 5), 'not a numeric field'
                struct.pack_into('<f' if kind == 0 else '<I', self.b, o, value)
                return
        raise KeyError(field)

    def insert_row(self, name, index, template, strings):
        """Insert a row into a table: a copy of row `template` with the string cells in `strings` {col: text} replaced.
        The table is relocated to the end of the blob."""
        n = self.find(name)
        assert n['type'] == 2
        width = CELL * n['cols']
        old = bytes(self.b[n['data']:n['data'] + n['rows'] * width])
        row = bytearray(old[template * width:(template + 1) * width])
        for col, text in strings.items():
            while len(self.b) % 16:
                self.b.append(0)
            s_off = len(self.b)
            raw = text.encode('latin1')
            self.b += raw + b'\0'
            assert struct.unpack_from('<I', row, col * CELL + 16)[0] == 2, 'template cell is not a string'
            struct.pack_into('<II', row, col * CELL, len(raw), s_off)
        while len(self.b) % 16:
            self.b.append(0)
        new = len(self.b)
        if index < 0:
            index = n['rows']
        self.b += old[:index * width] + bytes(row) + old[index * width:]
        struct.pack_into('<I', self.b, n['pos'] + 0x28, n['rows'] + 1)
        struct.pack_into('<I', self.b, n['pos'] + 0x30, new)


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    if d[:4] == b'AZP3':
        for name, kind, off, size in pack_entries(d):
            print('==== %s (%s) @0x%x size 0x%x' % (name, kind, off, size))
            if d[off:off + 4] == b'AZCF':
                Azcf(d[off:off + size]).dump()
    else:
        Azcf(d).dump()
