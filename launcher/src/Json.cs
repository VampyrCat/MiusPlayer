// JSON: parsing through the framework's JavaScriptSerializer, writing through a small ordered writer (so the
// files we write keep their key order and stay readable).
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Web.Script.Serialization;

namespace MiusPlayer
{
    // An ordered JSON object for writing.
    public class JObj
    {
        public readonly List<KeyValuePair<string, object>> Items = new List<KeyValuePair<string, object>>();

        public JObj Add(string key, object value)
        {
            Items.Add(new KeyValuePair<string, object>(key, value));
            return this;
        }
    }

    public static class Json
    {
        public static object Parse(string text)
        {
            JavaScriptSerializer s = new JavaScriptSerializer();
            s.MaxJsonLength = 64 * 1024 * 1024;
            s.RecursionLimit = 64;
            return s.DeserializeObject(text);
        }

        public static IDictionary<string, object> ParseObject(string text)
        {
            IDictionary<string, object> d = Parse(text) as IDictionary<string, object>;
            if (d == null)
                throw new FormatException("not a JSON object");
            return d;
        }

        public static object Get(IDictionary<string, object> d, string key)
        {
            object v;
            if (d == null || !d.TryGetValue(key, out v))
                return null;
            return v;
        }

        public static string Str(IDictionary<string, object> d, string key)
        {
            object v = Get(d, key);
            if (v == null)
                return null;
            if (v is string)
                return (string)v;
            if (v is IDictionary || v is object[])
                return null;
            return Convert.ToString(v, CultureInfo.InvariantCulture);
        }

        public static long? Num(IDictionary<string, object> d, string key)
        {
            object v = Get(d, key);
            if (v == null || v is string || v is bool || v is IDictionary || v is object[])
                return null;
            try { return Convert.ToInt64(v, CultureInfo.InvariantCulture); }
            catch (Exception) { return null; }
        }

        public static bool? Bool(IDictionary<string, object> d, string key)
        {
            object v = Get(d, key);
            if (v is bool)
                return (bool)v;
            return null;
        }

        public static IDictionary<string, object> Obj(IDictionary<string, object> d, string key)
        {
            return Get(d, key) as IDictionary<string, object>;
        }

        public static object[] Arr(IDictionary<string, object> d, string key)
        {
            object v = Get(d, key);
            if (v is object[])
                return (object[])v;
            ArrayList al = v as ArrayList;
            if (al != null)
                return al.ToArray();
            return null;
        }

        public static string Write(object value)
        {
            StringBuilder sb = new StringBuilder();
            WriteValue(sb, value, 0);
            sb.Append("\r\n");
            return sb.ToString();
        }

        static void Indent(StringBuilder sb, int level)
        {
            sb.Append("\r\n");
            sb.Append(' ', level * 2);
        }

        static void WriteValue(StringBuilder sb, object v, int level)
        {
            if (v == null)
                sb.Append("null");
            else if (v is string)
                WriteString(sb, (string)v);
            else if (v is bool)
                sb.Append((bool)v ? "true" : "false");
            else if (v is JObj)
            {
                JObj o = (JObj)v;
                if (o.Items.Count == 0) { sb.Append("{}"); return; }
                sb.Append('{');
                for (int i = 0; i < o.Items.Count; i++)
                {
                    if (i > 0) sb.Append(',');
                    Indent(sb, level + 1);
                    WriteString(sb, o.Items[i].Key);
                    sb.Append(": ");
                    WriteValue(sb, o.Items[i].Value, level + 1);
                }
                Indent(sb, level);
                sb.Append('}');
            }
            else if (v is IEnumerable)
            {
                List<object> items = new List<object>();
                foreach (object x in (IEnumerable)v)
                    items.Add(x);
                if (items.Count == 0) { sb.Append("[]"); return; }
                bool simple = true;
                foreach (object x in items)
                    if (x is JObj || (x is IEnumerable && !(x is string)))
                        simple = false;
                sb.Append('[');
                for (int i = 0; i < items.Count; i++)
                {
                    if (i > 0) sb.Append(simple ? ", " : ",");
                    if (!simple) Indent(sb, level + 1);
                    WriteValue(sb, items[i], level + 1);
                }
                if (!simple) Indent(sb, level);
                sb.Append(']');
            }
            else if (v is int || v is long || v is uint || v is short || v is byte)
                sb.Append(Convert.ToInt64(v).ToString(CultureInfo.InvariantCulture));
            else if (v is double || v is float || v is decimal)
                sb.Append(Convert.ToDouble(v).ToString("R", CultureInfo.InvariantCulture));
            else
                WriteString(sb, v.ToString());
        }

        static void WriteString(StringBuilder sb, string s)
        {
            sb.Append('"');
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20)
                            sb.Append("\\u").Append(((int)c).ToString("x4"));
                        else
                            sb.Append(c);
                        break;
                }
            }
            sb.Append('"');
        }
    }
}
