// The game folder: which build it is (link timestamps of two engine modules), whether it sits in a Steam
// library (and that library's appmanifest), finding it through Steam, and whether the game is running.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;

namespace MiusPlayer
{
    public static class Util
    {
        public static string Sha256Hex(byte[] data)
        {
            using (SHA256 h = new SHA256CryptoServiceProvider())
                return Hex(h.ComputeHash(data));
        }

        public static string Sha256File(string path)
        {
            using (SHA256 h = new SHA256CryptoServiceProvider())
            using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                return Hex(h.ComputeHash(fs));
        }

        static string Hex(byte[] b)
        {
            StringBuilder sb = new StringBuilder(b.Length * 2);
            foreach (byte x in b)
                sb.Append(x.ToString("x2"));
            return sb.ToString();
        }

        public static string FullDir(string dir)
        {
            string f = Path.GetFullPath(dir);
            return f.TrimEnd('\\', '/');
        }

        public static bool SameDir(string a, string b)
        {
            if (a == null || b == null)
                return false;
            try { return string.Equals(FullDir(a), FullDir(b), StringComparison.OrdinalIgnoreCase); }
            catch (Exception) { return false; }
        }

        // A relative path from a manifest or record ("bin/rel/x64/x.codelib") as a path inside the game folder,
        // or null when it is not a plain relative path (rooted, "..", drive letters, empty parts ...).
        public static string SafeRel(string rel)
        {
            if (string.IsNullOrEmpty(rel) || rel.Length > 200)
                return null;
            string[] parts = rel.Replace('\\', '/').Split('/');
            foreach (string p in parts)
            {
                if (p.Length == 0 || p == "." || p == ".." || p.Trim() != p || p.EndsWith("."))
                    return null;
                if (p.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || p.IndexOf(':') >= 0)
                    return null;
            }
            return string.Join("\\", parts);
        }

        public static string NormRel(string rel)
        {
            return rel.Replace('\\', '/');
        }

        // Writes a file through a temporary next to it, so a failed write never leaves half a file.
        public static void WriteFileSafe(string path, byte[] data)
        {
            string dir = Path.GetDirectoryName(path);
            if (!Directory.Exists(dir))
                Directory.CreateDirectory(dir);
            string tmp = path + ".mp_new";
            File.WriteAllBytes(tmp, data);
            try
            {
                if (File.Exists(path))
                {
                    File.SetAttributes(path, FileAttributes.Normal);
                    File.Delete(path);
                }
                File.Move(tmp, path);
            }
            catch
            {
                try { File.Delete(tmp); } catch (Exception) { }
                throw;
            }
        }

        public static void DeleteFile(string path)
        {
            if (!File.Exists(path))
                return;
            File.SetAttributes(path, FileAttributes.Normal);
            File.Delete(path);
        }

        public static int[] ParseVersion(string v)
        {
            if (v == null)
                return null;
            v = v.Trim();
            if (v.StartsWith("v") || v.StartsWith("V"))
                v = v.Substring(1);
            int cut = v.IndexOfAny(new char[] { '-', '+', ' ' });
            if (cut >= 0)
                v = v.Substring(0, cut);
            string[] parts = v.Split('.');
            int[] r = new int[parts.Length];
            for (int i = 0; i < parts.Length; i++)
            {
                if (!int.TryParse(parts[i], System.Globalization.NumberStyles.None,
                                  System.Globalization.CultureInfo.InvariantCulture, out r[i]))
                    return null;
            }
            return r;
        }

        // numeric comparison: 1.10.0 > 1.9.3, 1.2 == 1.2.0. Unparsable versions compare as lowest.
        public static int CompareVersions(string a, string b)
        {
            int[] x = ParseVersion(a), y = ParseVersion(b);
            if (x == null || y == null)
                return (x == null ? 0 : 1) - (y == null ? 0 : 1);
            for (int i = 0; i < Math.Max(x.Length, y.Length); i++)
            {
                int p = i < x.Length ? x[i] : 0, q = i < y.Length ? y[i] : 0;
                if (p != q)
                    return p < q ? -1 : 1;
            }
            return 0;
        }

        public static string CleanVersion(string v)
        {
            if (v == null)
                return null;
            v = v.Trim();
            if (v.StartsWith("v") || v.StartsWith("V"))
                v = v.Substring(1);
            return v;
        }
    }

    public class KnownBuild
    {
        public string Label;     // "public-15814271"
        public string Name;      // "Steam default branch"
        public uint Shell;       // TimeDateStamp of bin\rel\x64\base_shell.codelib
        public uint Main;        // ... of base_main.codelib

        public KnownBuild(string label, string name, uint shell, uint main)
        {
            Label = label; Name = name; Shell = shell; Main = main;
        }
    }

    public static class Builds
    {
        public static readonly KnownBuild[] Known = {
            new KnownBuild("public_beta-16070931", "older Steam beta build", 0x670d5899, 0x670d5943),
            new KnownBuild("public-15814271", "Steam default branch", 0x66ed7c47, 0x66f2d70e),
            // Steam's beta branch since 2026-11-08 (it replaced public_beta-26494568, 0x6aee3958 / 0x6aee394a)
            new KnownBuild("public_beta-26514238", "Steam beta branch", 0x6aefc0ff, 0x6aefc0f2),
        };

        public static KnownBuild Find(uint shell, uint main)
        {
            foreach (KnownBuild k in Known)
                if (k.Shell == shell && k.Main == main)
                    return k;
            return null;
        }

        // PE header TimeDateStamp, or null when the file is missing or not a PE file.
        public static uint? PeStamp(string path)
        {
            try
            {
                using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    byte[] h = new byte[4096];
                    int n = fs.Read(h, 0, h.Length);
                    if (n < 0x40 || h[0] != 'M' || h[1] != 'Z')
                        return null;
                    int pe = BitConverter.ToInt32(h, 0x3c);
                    if (pe < 0 || pe + 12 > n || h[pe] != 'P' || h[pe + 1] != 'E' || h[pe + 2] != 0 || h[pe + 3] != 0)
                        return null;
                    return BitConverter.ToUInt32(h, pe + 8);
                }
            }
            catch (Exception) { return null; }
        }
    }

    // A parsed Valve KeyValues text file (libraryfolders.vdf, appmanifest_*.acf).
    public class Vdf
    {
        public readonly List<KeyValuePair<string, object>> Items = new List<KeyValuePair<string, object>>();

        public string Str(string key)
        {
            foreach (KeyValuePair<string, object> kv in Items)
                if (string.Equals(kv.Key, key, StringComparison.OrdinalIgnoreCase) && kv.Value is string)
                    return (string)kv.Value;
            return null;
        }

        public Vdf Kid(string key)
        {
            foreach (KeyValuePair<string, object> kv in Items)
                if (string.Equals(kv.Key, key, StringComparison.OrdinalIgnoreCase) && kv.Value is Vdf)
                    return (Vdf)kv.Value;
            return null;
        }

        public static Vdf Load(string path)
        {
            try { return Parse(File.ReadAllText(path)); }
            catch (Exception) { return null; }
        }

        public static Vdf Parse(string text)
        {
            List<string> tokens = new List<string>();
            List<bool> quoted = new List<bool>();
            int i = 0;
            while (i < text.Length)
            {
                char c = text[i];
                if (char.IsWhiteSpace(c)) { i++; continue; }
                if (c == '/' && i + 1 < text.Length && text[i + 1] == '/')
                {
                    while (i < text.Length && text[i] != '\n') i++;
                    continue;
                }
                if (c == '{' || c == '}')
                {
                    tokens.Add(c.ToString()); quoted.Add(false); i++; continue;
                }
                StringBuilder sb = new StringBuilder();
                if (c == '"')
                {
                    i++;
                    while (i < text.Length && text[i] != '"')
                    {
                        if (text[i] == '\\' && i + 1 < text.Length)
                        {
                            char e = text[i + 1];
                            sb.Append(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                            i += 2;
                            continue;
                        }
                        sb.Append(text[i]);
                        i++;
                    }
                    i++;
                    tokens.Add(sb.ToString()); quoted.Add(true);
                }
                else
                {
                    while (i < text.Length && !char.IsWhiteSpace(text[i]) && text[i] != '{' && text[i] != '}' && text[i] != '"')
                        sb.Append(text[i++]);
                    tokens.Add(sb.ToString()); quoted.Add(false);
                }
            }
            int pos = 0;
            Vdf root = new Vdf();
            ParseInto(root, tokens, quoted, ref pos);
            return root;
        }

        static void ParseInto(Vdf node, List<string> t, List<bool> q, ref int pos)
        {
            while (pos < t.Count)
            {
                if (t[pos] == "}" && !q[pos]) { pos++; return; }
                string key = t[pos++];
                if (pos >= t.Count) return;
                if (t[pos] == "{" && !q[pos])
                {
                    pos++;
                    Vdf kid = new Vdf();
                    ParseInto(kid, t, q, ref pos);
                    node.Items.Add(new KeyValuePair<string, object>(key, kid));
                }
                else
                    node.Items.Add(new KeyValuePair<string, object>(key, t[pos++]));
            }
        }
    }

    public class SteamInfo
    {
        public string Library;     // the Steam library folder (the one holding steamapps)
        public string BuildId;
        public string BetaKey;     // "" = default branch
        public string ManifestPath;

        public string Describe()
        {
            string branch = string.IsNullOrEmpty(BetaKey) ? "default branch" : "branch \"" + BetaKey + "\"";
            return "Steam build " + (string.IsNullOrEmpty(BuildId) ? "?" : BuildId) + ", " + branch;
        }
    }

    public static class Steam
    {
        public const string AppId = "312980";

        public static string Root()
        {
            string[,] keys = {
                { "HKCU", @"Software\Valve\Steam", "SteamPath" },
                { "HKLM", @"SOFTWARE\Wow6432Node\Valve\Steam", "InstallPath" },
                { "HKLM", @"SOFTWARE\Valve\Steam", "InstallPath" },
            };
            for (int i = 0; i < keys.GetLength(0); i++)
            {
                try
                {
                    RegistryKey hive = keys[i, 0] == "HKCU" ? Registry.CurrentUser : Registry.LocalMachine;
                    using (RegistryKey k = hive.OpenSubKey(keys[i, 1]))
                    {
                        if (k == null) continue;
                        string p = k.GetValue(keys[i, 2]) as string;
                        if (!string.IsNullOrEmpty(p))
                        {
                            p = p.Replace('/', '\\');
                            if (Directory.Exists(p))
                                return Util.FullDir(p);
                        }
                    }
                }
                catch (Exception) { }
            }
            return null;
        }

        public static List<string> Libraries(string root)
        {
            List<string> libs = new List<string>();
            if (root == null)
                return libs;
            libs.Add(root);
            foreach (string vdfPath in new string[] { Path.Combine(root, @"steamapps\libraryfolders.vdf"),
                                                      Path.Combine(root, @"config\libraryfolders.vdf") })
            {
                Vdf v = Vdf.Load(vdfPath);
                if (v == null) continue;
                Vdf lf = v.Kid("libraryfolders") ?? v.Kid("LibraryFolders");
                if (lf == null) continue;
                foreach (KeyValuePair<string, object> kv in lf.Items)
                {
                    string p = kv.Value as string;           // old format: "1" "D:\\SteamLibrary"
                    if (p == null && kv.Value is Vdf)
                        p = ((Vdf)kv.Value).Str("path");     // new format: "1" { "path" "..." }
                    else if (p != null)
                    {
                        int dummy;
                        if (!int.TryParse(kv.Key, out dummy)) p = null;
                    }
                    if (string.IsNullOrEmpty(p)) continue;
                    try
                    {
                        string full = Util.FullDir(p);
                        bool seen = false;
                        foreach (string l in libs)
                            if (Util.SameDir(l, full)) seen = true;
                        if (!seen && Directory.Exists(full))
                            libs.Add(full);
                    }
                    catch (Exception) { }
                }
            }
            return libs;
        }

        // The game folder through Steam, or null.
        public static string FindGame()
        {
            foreach (string lib in Libraries(Root()))
            {
                Vdf acf = Vdf.Load(Path.Combine(lib, @"steamapps\appmanifest_" + AppId + ".acf"));
                if (acf == null) continue;
                Vdf st = acf.Kid("AppState");
                string dir = st != null ? st.Str("installdir") : null;
                if (string.IsNullOrEmpty(dir) || Util.SafeRel(dir) == null) continue;
                string game = Path.Combine(lib, @"steamapps\common", dir);
                if (File.Exists(Path.Combine(game, "starter_x64.exe")))
                    return Util.FullDir(game);
            }
            return null;
        }

        // When the folder is <library>\steamapps\common\<installdir> of this app: its appmanifest's build/branch.
        public static SteamInfo InfoFor(string gameDir)
        {
            try
            {
                DirectoryInfo game = new DirectoryInfo(Util.FullDir(gameDir));
                DirectoryInfo common = game.Parent;
                if (common == null || !string.Equals(common.Name, "common", StringComparison.OrdinalIgnoreCase)) return null;
                DirectoryInfo steamapps = common.Parent;
                if (steamapps == null || !string.Equals(steamapps.Name, "steamapps", StringComparison.OrdinalIgnoreCase)) return null;
                string acfPath = Path.Combine(steamapps.FullName, "appmanifest_" + AppId + ".acf");
                Vdf acf = Vdf.Load(acfPath);
                if (acf == null) return null;
                Vdf st = acf.Kid("AppState");
                if (st == null || !string.Equals(st.Str("installdir"), game.Name, StringComparison.OrdinalIgnoreCase)) return null;
                SteamInfo si = new SteamInfo();
                si.Library = steamapps.Parent != null ? steamapps.Parent.FullName : null;
                si.ManifestPath = acfPath;
                si.BuildId = st.Str("buildid");
                Vdf uc = st.Kid("UserConfig"), mc = st.Kid("MountedConfig");
                si.BetaKey = (uc != null ? uc.Str("BetaKey") : null) ?? (mc != null ? mc.Str("BetaKey") : null) ?? "";
                return si;
            }
            catch (Exception) { return null; }
        }
    }

    public class RunningProcess
    {
        public string Name;
        public int Pid;
        public string Path;   // null when it could not be read
    }

    public static class Processes
    {
        public static readonly string[] GameNames = { "starter_x64", "mp_join" };

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr h);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool QueryFullProcessImageName(IntPtr h, int flags, StringBuilder name, ref int size);

        const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

        public static string ImagePath(int pid)
        {
            IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if (h == IntPtr.Zero)
                return null;
            try
            {
                StringBuilder sb = new StringBuilder(1024);
                int size = sb.Capacity;
                if (QueryFullProcessImageName(h, 0, sb, ref size))
                    return sb.ToString(0, size);
                return null;
            }
            finally { CloseHandle(h); }
        }

        // Processes of these names that run from this folder, plus those whose folder could not be read
        // (counted as running there, to be safe).
        public static List<RunningProcess> RunningIn(string dir, string[] names)
        {
            List<RunningProcess> r = new List<RunningProcess>();
            foreach (string name in names)
            {
                Process[] ps;
                try { ps = Process.GetProcessesByName(name); }
                catch (Exception) { continue; }
                foreach (Process p in ps)
                {
                    try
                    {
                        string path = ImagePath(p.Id);
                        if (path == null || Util.SameDir(Path.GetDirectoryName(path), dir))
                        {
                            RunningProcess rp = new RunningProcess();
                            rp.Name = name;
                            rp.Pid = p.Id;
                            rp.Path = path;
                            r.Add(rp);
                        }
                    }
                    catch (Exception) { }
                    finally { p.Dispose(); }
                }
            }
            return r;
        }

        public static string Describe(List<RunningProcess> list)
        {
            List<string> s = new List<string>();
            foreach (RunningProcess p in list)
                s.Add(p.Name + ".exe (process " + p.Pid + (p.Path == null ? ", folder unknown" : "") + ")");
            return string.Join(", ", s.ToArray());
        }
    }

    // What the launcher knows about one game folder.
    public class GameInfo
    {
        public string Dir;
        public bool Exists;            // starter_x64.exe is there
        public uint? ShellStamp, MainStamp;
        public KnownBuild Build;       // null = unknown build
        public SteamInfo Steam;        // null = not in a Steam library
        public InstallRecord Record;   // null = not installed by the launcher
        public string RecordError;     // mp_installed.json there but unreadable
        public bool LegacyMod;         // an older multiplayer version without a record
        public string PackageEnabled;  // "mpmod1"/"mpmod2" when that package says it is enabled here
        public string FlatdataSha;

        public string BuildLabel { get { return Build != null ? Build.Label : null; } }

        public string BuildText()
        {
            if (!Exists)
                return "no game in this folder";
            if (Build != null)
                return Build.Name + " (" + Build.Label + ")";
            return "unknown build (base_shell " + StampText(ShellStamp) + ", base_main " + StampText(MainStamp) + ")";
        }

        static string StampText(uint? s)
        {
            return s.HasValue ? "0x" + s.Value.ToString("x8") : "missing";
        }

        public static bool LooksLikeGame(string dir)
        {
            return !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "starter_x64.exe"));
        }

        public static GameInfo Read(string dir)
        {
            GameInfo g = new GameInfo();
            g.Dir = Util.FullDir(dir);
            g.Exists = LooksLikeGame(g.Dir);
            if (!g.Exists)
                return g;
            g.ShellStamp = Builds.PeStamp(Path.Combine(g.Dir, @"bin\rel\x64\base_shell.codelib"));
            g.MainStamp = Builds.PeStamp(Path.Combine(g.Dir, @"bin\rel\x64\base_main.codelib"));
            if (g.ShellStamp.HasValue && g.MainStamp.HasValue)
                g.Build = Builds.Find(g.ShellStamp.Value, g.MainStamp.Value);
            g.Steam = MiusPlayer.Steam.InfoFor(g.Dir);
            string flat = Path.Combine(g.Dir, @"root\programs.flatdata");
            try { if (File.Exists(flat)) g.FlatdataSha = Util.Sha256File(flat); }
            catch (Exception) { }
            string rec = Path.Combine(g.Dir, InstallRecord.FileName);
            if (File.Exists(rec))
            {
                try { g.Record = InstallRecord.Load(rec); }
                catch (Exception e) { g.RecordError = e.Message; }
            }
            g.LegacyMod = g.Record == null && Installer.LegacyEvidence(g.Dir);
            foreach (string pkg in new string[] { "mpmod1", "mpmod2" })
                if (File.Exists(Path.Combine(g.Dir, pkg + @"\state\enabled.txt")))
                    g.PackageEnabled = pkg;
            return g;
        }

        // The installed mod was made for another game build / another programs.flatdata: Steam updated the game.
        public bool InstallStale()
        {
            if (Record == null)
                return false;
            if (Record.Build != BuildLabel)
                return true;
            return Record.FlatdataSha256 != null && FlatdataSha != null && Record.FlatdataSha256 != FlatdataSha;
        }
    }
}
