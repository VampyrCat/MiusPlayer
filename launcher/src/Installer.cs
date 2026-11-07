// Install / update / uninstall of the mod in one game folder, and the install record <game>\mp_installed.json.
//
// Every file the launcher writes is listed in the record with its SHA-256; uninstall removes exactly those,
// puts back any file of the same name that was there before (kept in <game>\mp_backup), removes the folders
// the install made when they are empty again, and the record. The two program files are generated here from
// the game's own root\programs.flatdata (see Progpack.cs), never shipped.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace MiusPlayer
{
    public class InstallException : Exception
    {
        public readonly int Code;
        public InstallException(int code, string message) : base(message) { Code = code; }
    }

    public static class ExitCodes
    {
        public const int Ok = 0;
        public const int Failed = 1;
        public const int Usage = 2;
        public const int NoGame = 3;
        public const int GameRunning = 4;
        public const int BuildNotSupported = 5;
        public const int BadRelease = 6;
        public const int ProgramsFailed = 7;
        public const int NotInstalled = 8;
        public const int PackageEnabled = 9;
        public const int NoRelease = 10;
    }

    public class RecordFile
    {
        public string Path;
        public string Sha256;
    }

    public class RecordBackup
    {
        public string Path;      // the game file that was there before the install
        public string Backup;    // where it is kept meanwhile (mp_backup/...)
    }

    public class InstallRecord
    {
        public const string FileName = "mp_installed.json";

        public string Mod = "MiusPlayer";
        public string Version;
        public long? Network;
        public string Build;
        public string InstalledUtc;
        public string Launcher;
        public string FlatdataSha256;
        public bool Complete;
        public List<RecordFile> Files = new List<RecordFile>();
        public List<string> Generated = new List<string>();
        public List<string> CreatedDirs = new List<string>();
        public List<RecordBackup> Backups = new List<RecordBackup>();

        public RecordFile FindFile(string rel)
        {
            foreach (RecordFile f in Files)
                if (string.Equals(f.Path, rel, StringComparison.OrdinalIgnoreCase))
                    return f;
            return null;
        }

        public RecordBackup FindBackup(string rel)
        {
            foreach (RecordBackup b in Backups)
                if (string.Equals(b.Path, rel, StringComparison.OrdinalIgnoreCase))
                    return b;
            return null;
        }

        public static InstallRecord Load(string path)
        {
            IDictionary<string, object> d = Json.ParseObject(File.ReadAllText(path, Encoding.UTF8));
            InstallRecord r = new InstallRecord();
            r.Mod = Json.Str(d, "mod");
            r.Version = Json.Str(d, "version");
            r.Network = Json.Num(d, "network");
            r.Build = Json.Str(d, "build");
            r.InstalledUtc = Json.Str(d, "installed_utc");
            r.Launcher = Json.Str(d, "launcher");
            r.FlatdataSha256 = Json.Str(d, "flatdata_sha256");
            bool? c = Json.Bool(d, "complete");
            r.Complete = !c.HasValue || c.Value;
            object[] files = Json.Arr(d, "files");
            if (files == null)
                throw new FormatException("no file list");
            foreach (object o in files)
            {
                IDictionary<string, object> f = o as IDictionary<string, object>;
                string p = f != null ? Json.Str(f, "path") : null;
                if (p == null || Util.SafeRel(p) == null)
                    throw new FormatException("a file path that is not a plain relative path: " + p);
                RecordFile rf = new RecordFile();
                rf.Path = Util.NormRel(p);
                rf.Sha256 = Json.Str(f, "sha256");
                r.Files.Add(rf);
            }
            object[] gen = Json.Arr(d, "generated");
            if (gen != null)
                foreach (object o in gen)
                    if (o is string) r.Generated.Add(Util.NormRel((string)o));
            object[] dirs = Json.Arr(d, "created_dirs");
            if (dirs != null)
                foreach (object o in dirs)
                    if (o is string && Util.SafeRel((string)o) != null) r.CreatedDirs.Add(Util.NormRel((string)o));
            object[] backups = Json.Arr(d, "backups");
            if (backups != null)
                foreach (object o in backups)
                {
                    IDictionary<string, object> b = o as IDictionary<string, object>;
                    if (b == null) continue;
                    RecordBackup rb = new RecordBackup();
                    rb.Path = Json.Str(b, "path");
                    rb.Backup = Json.Str(b, "backup");
                    if (rb.Path == null || rb.Backup == null || Util.SafeRel(rb.Path) == null || Util.SafeRel(rb.Backup) == null)
                        throw new FormatException("a backup entry that is not a plain relative path");
                    rb.Path = Util.NormRel(rb.Path);
                    rb.Backup = Util.NormRel(rb.Backup);
                    r.Backups.Add(rb);
                }
            return r;
        }

        public void Save(string path)
        {
            JObj o = new JObj();
            o.Add("mod", Mod ?? "MiusPlayer");
            o.Add("version", Version);
            o.Add("network", Network.HasValue ? (object)Network.Value : null);
            o.Add("build", Build);
            o.Add("installed_utc", InstalledUtc);
            o.Add("launcher", Launcher);
            o.Add("flatdata_sha256", FlatdataSha256);
            o.Add("complete", Complete);
            List<object> files = new List<object>();
            foreach (RecordFile f in Files)
                files.Add(new JObj().Add("path", f.Path).Add("sha256", f.Sha256));
            o.Add("files", files);
            o.Add("generated", Generated);
            o.Add("created_dirs", CreatedDirs);
            List<object> backups = new List<object>();
            foreach (RecordBackup b in Backups)
                backups.Add(new JObj().Add("path", b.Path).Add("backup", b.Backup));
            o.Add("backups", backups);
            Util.WriteFileSafe(path, new UTF8Encoding(false).GetBytes(Json.Write(o)));
        }
    }

    public static class Installer
    {
        public const string BackupDir = "mp_backup";
        // files an older version of the mod left in the game folder; an install removes them
        public static readonly string[] LegacyLeftovers = { "Multiplayer_README.txt", "Remove_Multiplayer.bat" };
        public const string ProgramsDir = "root/programs";

        public delegate void Log(string line);

        public static bool IsReservedPath(string rel)
        {
            string r = Util.NormRel(rel);
            foreach (string n in Progpack.Names)
                if (string.Equals(r, Progpack.RelPathOf(n), StringComparison.OrdinalIgnoreCase))
                    return true;
            return string.Equals(r, InstallRecord.FileName, StringComparison.OrdinalIgnoreCase)
                || r.StartsWith(BackupDir + "/", StringComparison.OrdinalIgnoreCase)
                || string.Equals(r, BackupDir, StringComparison.OrdinalIgnoreCase);
        }

        static string Abs(string game, string rel)
        {
            string s = Util.SafeRel(rel);
            if (s == null)
                throw new InstallException(ExitCodes.Failed, "not a plain relative path: " + rel);
            return Path.Combine(game, s);
        }

        // An older multiplayer version (copied in by hand, no record): its files may be replaced and become ours.
        public static bool LegacyEvidence(string game)
        {
            foreach (string f in LegacyLeftovers)
                if (File.Exists(Path.Combine(game, f)))
                    return true;
            if (File.Exists(Path.Combine(game, @"bin\rel\x64\basic_network.codelib")))
                return true;
            string tac = Path.Combine(game, @"root\programs\tacgame.progpack");
            try
            {
                if (File.Exists(tac) && Contains(File.ReadAllBytes(tac), Encoding.ASCII.GetBytes("basic_network.codelib")))
                    return true;
            }
            catch (Exception) { }
            return false;
        }

        static bool Contains(byte[] hay, byte[] needle)
        {
            for (int i = 0; i + needle.Length <= hay.Length; i++)
            {
                int j = 0;
                while (j < needle.Length && hay[i + j] == needle[j]) j++;
                if (j == needle.Length) return true;
            }
            return false;
        }

        public static void CheckGameFolder(GameInfo g)
        {
            if (!g.Exists)
                throw new InstallException(ExitCodes.NoGame, "There is no game in \"" + g.Dir + "\" (starter_x64.exe is missing).");
        }

        public static void CheckNotRunning(GameInfo g)
        {
            List<RunningProcess> running = Processes.RunningIn(g.Dir, Processes.GameNames);
            if (running.Count > 0)
                throw new InstallException(ExitCodes.GameRunning,
                    "The game is running from this folder (" + Processes.Describe(running) + "). Close it first.");
        }

        public static string NotSupportedText(GameInfo g, ReleaseManifest m)
        {
            return "This version of the mod (" + m.Version + ") does not support your game build yet: "
                 + g.BuildText() + ". Supported: " + (m.Builds.Count > 0 ? string.Join(", ", m.Builds.ToArray()) : "none")
                 + ". Wait for a newer version of the mod (or switch the game to a supported Steam branch).";
        }

        class Write
        {
            public string Rel;
            public byte[] Data;
            public bool Generated;
        }

        public static void Install(GameInfo g, ReleasePackage pkg, Log log)
        {
            CheckGameFolder(g);
            CheckNotRunning(g);
            if (g.PackageEnabled != null)
                throw new InstallException(ExitCodes.PackageEnabled,
                    "The multiplayer package in the folder \"" + g.PackageEnabled + "\" is enabled in this game. It uses the same file names: run "
                    + g.PackageEnabled + "\\Disable_Multiplayer.bat first, then install again.");
            ReleaseManifest m = pkg.Manifest;
            if (!m.Supports(g.BuildLabel))
                throw new InstallException(ExitCodes.BuildNotSupported, NotSupportedText(g, m));

            string flatPath = Path.Combine(g.Dir, @"root\programs.flatdata");
            if (!File.Exists(flatPath))
                throw new InstallException(ExitCodes.ProgramsFailed, "The game has no root\\programs.flatdata - is the game complete? (Steam: verify the game files.)");
            byte[] flat = File.ReadAllBytes(flatPath);
            List<string> notes = new List<string>();
            List<GeneratedProgram> programs;
            try { programs = Progpack.Generate(flat, notes); }
            catch (AzcfException e)
            {
                throw new InstallException(ExitCodes.ProgramsFailed, "The game's program definitions could not be prepared for multiplayer: " + e.Message);
            }
            foreach (string n in notes)
                log(n);

            InstallRecord old = g.Record;
            bool legacy = old == null && (g.LegacyMod || g.RecordError != null);
            if (g.RecordError != null)
                log("mp_installed.json was unreadable (" + g.RecordError + "); the mod's files are replaced and a new record written");
            else if (legacy)
                log("an older version of the multiplayer is in the game: it is replaced");

            // the module(s) first, then the other files in the manifest's order, the programs that ask for the
            // module last: a copy that stops half-way leaves a working game
            List<Write> writes = new List<Write>();
            for (int pass = 0; pass < 2; pass++)
                foreach (ManifestFile f in m.Files)
                {
                    bool module = f.Path.EndsWith(".codelib", StringComparison.OrdinalIgnoreCase);
                    if (module != (pass == 0))
                        continue;
                    Write w = new Write();
                    w.Rel = f.Path;
                    w.Data = pkg.Contents[f.Path];
                    writes.Add(w);
                }
            foreach (GeneratedProgram p in programs)
            {
                Write w = new Write();
                w.Rel = p.RelPath;
                w.Data = p.Data;
                w.Generated = true;
                writes.Add(w);
            }

            InstallRecord rec = new InstallRecord();
            rec.Version = m.Version;
            rec.Network = m.Network;
            rec.Build = g.BuildLabel;
            rec.InstalledUtc = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ");
            rec.Launcher = Program.Version;
            rec.FlatdataSha256 = Util.Sha256Hex(flat);
            if (old != null)
            {
                rec.CreatedDirs.AddRange(old.CreatedDirs);
                rec.Backups.AddRange(old.Backups);
            }

            string progDir = Path.Combine(g.Dir, @"root\programs");
            if (!Directory.Exists(progDir))
                AddDir(rec, ProgramsDir);
            else if (legacy && OnlyOurPrograms(progDir))
                AddDir(rec, ProgramsDir);

            string recPath = Path.Combine(g.Dir, InstallRecord.FileName);
            HashSet<string> newSet = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (Write w in writes)
                newSet.Add(w.Rel);
            try
            {
                foreach (Write w in writes)
                {
                    string target = Abs(g.Dir, w.Rel);
                    bool ours = legacy || (old != null && old.FindFile(w.Rel) != null);
                    if (File.Exists(target) && !ours)
                    {
                        if (string.Equals(w.Rel, "steam_appid.txt", StringComparison.OrdinalIgnoreCase)
                            && File.ReadAllText(target).Trim() == Steam.AppId)
                        {
                            log("kept the game folder's own steam_appid.txt (it already says " + Steam.AppId + ")");
                            continue;
                        }
                        BackUp(g.Dir, rec, w.Rel, log);
                    }
                    Util.WriteFileSafe(target, w.Data);
                    string sha = Util.Sha256File(target);
                    if (sha != Util.Sha256Hex(w.Data))
                        throw new IOException(w.Rel + " did not read back as written");
                    RecordFile rf = new RecordFile();
                    rf.Path = w.Rel;
                    rf.Sha256 = sha;
                    rec.Files.Add(rf);
                    if (w.Generated)
                        rec.Generated.Add(w.Rel);
                    log((w.Generated ? "generated " : "installed ") + w.Rel);
                }
                // files of the version installed before that this one no longer has
                if (old != null)
                    foreach (RecordFile f in old.Files)
                    {
                        if (newSet.Contains(f.Path))
                            continue;
                        Util.DeleteFile(Abs(g.Dir, f.Path));
                        log("removed " + f.Path + " (not part of this version)");
                        RestoreBackup(g.Dir, rec, f.Path, log);
                    }
                foreach (string f in LegacyLeftovers)
                {
                    string p = Path.Combine(g.Dir, f);
                    if (File.Exists(p))
                    {
                        Util.DeleteFile(p);
                        log("removed " + f + " (left by an older version)");
                    }
                }
                rec.Complete = true;
                rec.Save(recPath);
            }
            catch (Exception e)
            {
                if (e is InstallException) throw;
                // keep a record of everything that may be ours now, so Uninstall can still clean up
                if (old != null)
                    foreach (RecordFile f in old.Files)
                        if (rec.FindFile(f.Path) == null && File.Exists(Abs(g.Dir, f.Path)))
                            rec.Files.Add(f);
                rec.Complete = false;
                try { rec.Save(recPath); } catch (Exception) { }
                throw new InstallException(ExitCodes.Failed, "A file could not be written into the game folder: " + e.Message
                    + " The mod may be only partly installed now: install again to complete it, or uninstall to remove it.");
            }
        }

        static void AddDir(InstallRecord rec, string rel)
        {
            foreach (string d in rec.CreatedDirs)
                if (string.Equals(d, rel, StringComparison.OrdinalIgnoreCase))
                    return;
            rec.CreatedDirs.Add(rel);
        }

        static bool OnlyOurPrograms(string dir)
        {
            foreach (string f in Directory.GetFileSystemEntries(dir))
            {
                string n = Path.GetFileName(f);
                bool ours = false;
                foreach (string p in Progpack.Names)
                    if (string.Equals(n, p + ".progpack", StringComparison.OrdinalIgnoreCase)) ours = true;
                if (!ours) return false;
            }
            return true;
        }

        // A file of ours would replace one that is not ours: keep that one in mp_backup until uninstall.
        static void BackUp(string game, InstallRecord rec, string rel, Log log)
        {
            if (rec.FindBackup(rel) != null)
            {
                // the original is already kept; this one is a stray copy - replace it
                return;
            }
            string backupRel = BackupDir + "/" + rel;
            string src = Abs(game, rel), dst = Abs(game, backupRel);
            if (!Directory.Exists(Path.Combine(game, BackupDir)))
                AddDir(rec, BackupDir);
            Directory.CreateDirectory(Path.GetDirectoryName(dst));
            Util.DeleteFile(dst);
            File.Move(src, dst);
            RecordBackup b = new RecordBackup();
            b.Path = rel;
            b.Backup = backupRel;
            rec.Backups.Add(b);
            log("the game folder already had its own " + rel + ": kept in " + backupRel + " and put back on uninstall");
        }

        static void RestoreBackup(string game, InstallRecord rec, string rel, Log log)
        {
            RecordBackup b = rec.FindBackup(rel);
            if (b == null)
                return;
            string src = Abs(game, b.Backup), dst = Abs(game, b.Path);
            if (File.Exists(src))
            {
                Util.DeleteFile(dst);
                Directory.CreateDirectory(Path.GetDirectoryName(dst));
                File.Move(src, dst);
                log("put back the game folder's own " + b.Path);
            }
            rec.Backups.Remove(b);
        }

        public static void Uninstall(GameInfo g, Log log)
        {
            CheckGameFolder(g);
            CheckNotRunning(g);
            string recPath = Path.Combine(g.Dir, InstallRecord.FileName);
            if (g.Record == null)
            {
                if (g.RecordError != null)
                    throw new InstallException(ExitCodes.Failed, "mp_installed.json in the game folder is damaged (" + g.RecordError
                        + "), so it is not known which files are the mod's. Install again (that writes a new record), then uninstall.");
                if (g.LegacyMod)
                    throw new InstallException(ExitCodes.NotInstalled, "The mod was not installed by this launcher, but an older multiplayer version seems to be"
                        + " in the game. Install (it replaces the old one), then uninstall.");
                throw new InstallException(ExitCodes.NotInstalled, "The mod is not installed in this game.");
            }
            InstallRecord rec = g.Record;
            try
            {
                foreach (RecordFile f in rec.Files)
                {
                    string p = Abs(g.Dir, f.Path);
                    if (!File.Exists(p))
                    {
                        log("already gone: " + f.Path);
                        continue;
                    }
                    bool changed = f.Sha256 != null && Util.Sha256File(p) != f.Sha256;
                    Util.DeleteFile(p);
                    log("removed " + f.Path + (changed ? " (it had been changed since the install)" : ""));
                }
                foreach (RecordBackup b in new List<RecordBackup>(rec.Backups))
                    RestoreBackup(g.Dir, rec, b.Path, log);
                // folders the install made, when nothing else is in them now (mp_backup's own sub-folders too)
                foreach (string d in rec.CreatedDirs)
                {
                    string p = Abs(g.Dir, d);
                    if (!Directory.Exists(p))
                        continue;
                    if (string.Equals(d, BackupDir, StringComparison.OrdinalIgnoreCase))
                        RemoveEmptyTree(p);
                    else if (Directory.GetFileSystemEntries(p).Length == 0)
                        Directory.Delete(p);
                    if (!Directory.Exists(p))
                        log("removed the folder " + d + " (empty)");
                }
                Util.DeleteFile(recPath);
            }
            catch (Exception e)
            {
                if (e is InstallException) throw;
                throw new InstallException(ExitCodes.Failed, "Not everything could be removed: " + e.Message + " Uninstall again once the cause is gone.");
            }
        }

        static void RemoveEmptyTree(string dir)
        {
            foreach (string sub in Directory.GetDirectories(dir))
                RemoveEmptyTree(sub);
            if (Directory.GetFileSystemEntries(dir).Length == 0)
                Directory.Delete(dir);
        }

        // true when something was done; false = already up to date
        public static bool Update(GameInfo g, ReleasePackage pkg, Log log)
        {
            CheckGameFolder(g);
            if (g.Record == null)
                throw new InstallException(ExitCodes.NotInstalled, "The mod is not installed in this game (nothing to update) - install it first.");
            int cmp = Util.CompareVersions(pkg.Manifest.Version, g.Record.Version);
            if (cmp <= 0 && !g.InstallStale() && g.Record.Complete && AllPresent(g))
                return false;
            if (cmp < 0)
                throw new InstallException(ExitCodes.Failed, "The release (" + pkg.Manifest.Version + ") is older than the installed version ("
                    + g.Record.Version + "); use install to go back to it.");
            if (cmp == 0)
                log(g.InstallStale() ? "the game was updated since the mod was installed: the mod is installed again for it"
                                     : "the installed files are incomplete: the mod is installed again");
            Install(g, pkg, log);
            return true;
        }

        public static bool AllPresent(GameInfo g)
        {
            if (g.Record == null)
                return false;
            foreach (RecordFile f in g.Record.Files)
                if (!File.Exists(Abs(g.Dir, f.Path)))
                    return false;
            return true;
        }
    }
}
