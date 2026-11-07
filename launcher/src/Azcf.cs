// Port of _mp_research\tools\azcf.py: reader/patcher for GRAVITECH3 binary configs (AZCF) and program
// packs (AZP3). Must produce exactly the bytes the Python produces.
//
// Layout (all offsets absolute within one AZCF blob):
//   header 0x40:  'AZCF', node count, node table offset (0x40), ?, hash region offset, name[0x20]
//   node   0x80:  name[0x20], type (1 struct / 2 table), columns, rows, type-string offset,
//                 data offset, child-index offset, child count, hash size, hash offset
//   struct row:   cell[0x20] + field name[0x20]
//   table  row:   `columns` cells of 0x20
//   cell:         u32 len, u32 value-or-string-offset, u32, u32, u32 kind (2 = string) ...
using System;
using System.Collections.Generic;
using System.Text;

namespace MiusPlayer
{
    public class AzcfException : Exception
    {
        public AzcfException(string message) : base(message) { }
    }

    public class PackEntry
    {
        public string Name;
        public string Kind;
        public long Offset;
        public long Size;
    }

    public class AzcfNode
    {
        public int Index;
        public int Pos;
        public string Name;
        public uint Type, Cols, Rows, Types, Data, Kids, NKids, HSize, Hash;
    }

    public class Azcf
    {
        const int NODE = 0x80;
        const int CELL = 0x20;
        static readonly Encoding Latin1 = Encoding.GetEncoding(28591);
        static readonly Encoding Utf8Strict = new UTF8Encoding(false, true);

        byte[] b;
        int len;
        uint count, table;

        // Entries of an AZP3 pack: (name, kind, offset, size).
        public static List<PackEntry> PackEntries(byte[] d)
        {
            if (d.Length < 8 || d[0] != 'A' || d[1] != 'Z' || d[2] != 'P' || d[3] != '3')
                throw new AzcfException("not an AZP3 program pack");
            uint n = BitConverter.ToUInt32(d, 4);
            List<PackEntry> list = new List<PackEntry>();
            for (uint i = 0; i < n; i++)
            {
                long o = 0xc0 + (long)i * 0x80;
                if (o + 0x60 > d.Length)
                    throw new AzcfException("program pack entry table is cut short");
                PackEntry e = new PackEntry();
                e.Offset = (long)BitConverter.ToUInt64(d, (int)o + 8);
                e.Size = (long)BitConverter.ToUInt64(d, (int)o + 16);
                e.Name = ZString(d, (int)o + 0x20, 0x20);
                e.Kind = ZString(d, (int)o + 0x40, 0x20);
                list.Add(e);
            }
            return list;
        }

        // The first entry of that name, as an Azcf (Python: next(e for e in pack_entries(pack) if e[0] == name)).
        public static Azcf Stock(byte[] pack, string name)
        {
            foreach (PackEntry e in PackEntries(pack))
            {
                if (e.Name != name)
                    continue;
                if (e.Offset < 0 || e.Size < 0 || e.Offset + e.Size > pack.Length)
                    throw new AzcfException("program '" + name + "' lies outside the pack");
                byte[] blob = new byte[e.Size];
                Buffer.BlockCopy(pack, (int)e.Offset, blob, 0, (int)e.Size);
                return new Azcf(blob);
            }
            throw new AzcfException("the game's programs.flatdata has no program '" + name + "'");
        }

        // bytes[o:o+n].split(b'\0')[0].decode()
        static string ZString(byte[] d, int o, int n)
        {
            int end = o;
            while (end < o + n && end < d.Length && d[end] != 0)
                end++;
            return Utf8Strict.GetString(d, o, end - o);
        }

        public Azcf(byte[] blob)
        {
            if (blob.Length < 12 || blob[0] != 'A' || blob[1] != 'Z' || blob[2] != 'C' || blob[3] != 'F')
                throw new AzcfException("not an AZCF config");
            b = new byte[Math.Max(blob.Length * 2, 1024)];
            Buffer.BlockCopy(blob, 0, b, 0, blob.Length);
            len = blob.Length;
            count = U32(4);
            table = U32(8);
        }

        public byte[] Bytes()
        {
            byte[] r = new byte[len];
            Buffer.BlockCopy(b, 0, r, 0, len);
            return r;
        }

        public int Length { get { return len; } }

        uint U32(long off)
        {
            if (off < 0 || off + 4 > len)
                throw new AzcfException("config read outside its data at 0x" + off.ToString("x"));
            return BitConverter.ToUInt32(b, (int)off);
        }

        void PutU32(long off, uint v)
        {
            if (off < 0 || off + 4 > len)
                throw new AzcfException("config write outside its data at 0x" + off.ToString("x"));
            b[off] = (byte)v;
            b[off + 1] = (byte)(v >> 8);
            b[off + 2] = (byte)(v >> 16);
            b[off + 3] = (byte)(v >> 24);
        }

        void Append(byte[] data)
        {
            Append(data, 0, data.Length);
        }

        void Append(byte[] data, int off, int n)
        {
            if (len + n > b.Length)
            {
                byte[] nb = new byte[Math.Max(b.Length * 2, len + n + 1024)];
                Buffer.BlockCopy(b, 0, nb, 0, len);
                b = nb;
            }
            Buffer.BlockCopy(data, off, b, len, n);
            len += n;
        }

        void AppendU32(uint v)
        {
            Append(BitConverter.GetBytes(v));
        }

        // while len(self.b) % 16: self.b.append(0)
        void Pad16()
        {
            while (len % 16 != 0)
                Append(new byte[] { 0 });
        }

        byte[] Slice(long off, long n)
        {
            if (off < 0 || n < 0 || off + n > len)
                throw new AzcfException("config table lies outside its data");
            byte[] r = new byte[n];
            Buffer.BlockCopy(b, (int)off, r, 0, (int)n);
            return r;
        }

        public AzcfNode Node(int i)
        {
            long o = table + (long)i * NODE;
            if (o < 0 || o + 0x48 > len)
                throw new AzcfException("config node table lies outside its data");
            AzcfNode n = new AzcfNode();
            n.Index = i;
            n.Pos = (int)o;
            n.Name = ZString(b, (int)o, 0x20);
            n.Type = U32(o + 0x20);
            n.Cols = U32(o + 0x24);
            n.Rows = U32(o + 0x28);
            n.Types = U32(o + 0x2c);
            n.Data = U32(o + 0x30);
            n.Kids = U32(o + 0x34);
            n.NKids = U32(o + 0x38);
            n.HSize = U32(o + 0x3c);
            n.Hash = U32(o + 0x40);
            return n;
        }

        public AzcfNode Find(string name)
        {
            for (int i = 0; i < count; i++)
            {
                AzcfNode n = Node(i);
                if (n.Name == name)
                    return n;
            }
            throw new AzcfException("the program has no table '" + name + "'");
        }

        string CStr(long off)
        {
            if (off < 0 || off >= len)
                throw new AzcfException("string outside the config data");
            int end = (int)off;
            while (end < len && b[end] != 0)
                end++;
            if (end >= len)
                throw new AzcfException("unterminated string in the config data");
            return Latin1.GetString(b, (int)off, end - (int)off);
        }

        // A cell's text; null for a cell that is not a string (Python formats those as "<len=...>", which can
        // never equal a name we look for).
        string Cell(long off)
        {
            uint val = U32(off + 4);
            uint kind = U32(off + 16);
            if (kind == 2)
                return val != 0 ? CStr(val) : "";
            return null;
        }

        // The first cell of every row (tables), or the field name of every row (structs).
        public List<string> FirstColumn(AzcfNode n)
        {
            List<string> r = new List<string>();
            for (uint i = 0; i < n.Rows; i++)
            {
                if (n.Type == 1)
                    r.Add(ZString(b, (int)(n.Data + i * 0x40 + CELL), 0x20));
                else
                    r.Add(Cell(n.Data + (long)i * CELL * n.Cols));
            }
            return r;
        }

        // Append one string row to a single-column table by relocating the table to the end of the blob.
        public void AppendStringRow(string name, string text)
        {
            AzcfNode n = Find(name);
            if (n.Type != 2 || n.Cols != 1)
                throw new AzcfException("table '" + name + "' is not a single-column table");
            byte[] old = Slice(n.Data, (long)n.Rows * CELL);
            Pad16();
            uint sOff = (uint)len;
            byte[] raw = Latin1.GetBytes(text);
            Append(raw);
            Append(new byte[] { 0 });
            Pad16();
            uint nw = (uint)len;
            Append(old);
            AppendU32((uint)raw.Length);
            AppendU32(sOff);
            AppendU32(0);
            AppendU32(0);
            AppendU32(2);
            AppendU32(0);
            AppendU32(0);
            AppendU32(0);
            PutU32(n.Pos + 0x28, n.Rows + 1);
            PutU32(n.Pos + 0x30, nw);
        }

        // Set a numeric/bool field of a struct node. Floats are stored as their bit pattern.
        public void SetField(string node, string field, int value)
        {
            AzcfNode n = Find(node);
            if (n.Type != 1)
                throw new AzcfException("'" + node + "' is not a struct");
            for (uint r = 0; r < n.Rows; r++)
            {
                long o = n.Data + (long)r * 0x40;
                if (ZString(b, (int)(o + CELL), 0x20) != field)
                    continue;
                uint kind = U32(o + 16);
                if (kind != 0 && kind != 4 && kind != 5)
                    throw new AzcfException("'" + node + "." + field + "' is not a numeric field");
                if (kind == 0)
                    PutU32(o, BitConverter.ToUInt32(BitConverter.GetBytes((float)value), 0));
                else
                    PutU32(o, (uint)value);
                return;
            }
            throw new AzcfException("'" + node + "' has no field '" + field + "'");
        }

        // Insert a row into a table: a copy of row `template` with the string cells in `strings` {col: text}
        // replaced. The table is relocated to the end of the blob.
        public void InsertRow(string name, int index, int template, SortedDictionary<int, string> strings)
        {
            AzcfNode n = Find(name);
            if (n.Type != 2)
                throw new AzcfException("'" + name + "' is not a table");
            int width = CELL * (int)n.Cols;
            byte[] old = Slice(n.Data, (long)n.Rows * width);
            if (template < 0 || (template + 1) * width > old.Length)
                throw new AzcfException("table '" + name + "' has no row " + template);
            byte[] row = new byte[width];
            Buffer.BlockCopy(old, template * width, row, 0, width);
            foreach (KeyValuePair<int, string> kv in strings)
            {
                Pad16();
                uint sOff = (uint)len;
                byte[] raw = Latin1.GetBytes(kv.Value);
                Append(raw);
                Append(new byte[] { 0 });
                if (BitConverter.ToUInt32(row, kv.Key * CELL + 16) != 2)
                    throw new AzcfException("template cell is not a string");
                Buffer.BlockCopy(BitConverter.GetBytes((uint)raw.Length), 0, row, kv.Key * CELL, 4);
                Buffer.BlockCopy(BitConverter.GetBytes(sOff), 0, row, kv.Key * CELL + 4, 4);
            }
            Pad16();
            uint nw = (uint)len;
            if (index < 0)
                index = (int)n.Rows;
            if (index > n.Rows)
                index = (int)n.Rows;   // Python slicing clamps the same way
            Append(old, 0, index * width);
            Append(row);
            Append(old, index * width, old.Length - index * width);
            PutU32(n.Pos + 0x28, n.Rows + 1);
            PutU32(n.Pos + 0x30, nw);
        }
    }
}
