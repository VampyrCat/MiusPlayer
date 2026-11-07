// MiusPlayer launcher: installs, updates and removes the MiusPlayer multiplayer mod of
// Graviteam Tactics: Mius-Front (Steam app 312980), and starts the game.
//
// Without arguments it opens its window. Command line (for automated tests; no network when --zip is given):
//   MiusPlayerLauncher.exe --game <dir> [--zip <release zip>] (--install | --update | --uninstall | --status) [--quiet]
// It is a window program: from cmd use  start /wait "" MiusPlayerLauncher.exe ...  (or redirect its output) to
// wait for the exit code.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("MiusPlayer Launcher")]
[assembly: AssemblyProduct("MiusPlayer")]
[assembly: AssemblyDescription("Installs, updates and removes the MiusPlayer multiplayer mod of Graviteam Tactics: Mius-Front")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]

namespace MiusPlayer
{
    public class LauncherOptions
    {
        public string GameDir;        // --game
        public string ZipPath;        // --zip: a local release zip instead of GitHub
        public string SettingsPath;   // --settings: another settings file (tests)
        public bool NoSelfUpdate;     // --no-self-update
        public bool SelfUpdated;      // --self-updated: this run follows a self-update
    }

    public class Settings
    {
        public string GameDir;
        public bool AutoUpdate = true;
        string path;

        public static string DefaultPath()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), @"MiusPlayer\launcher.json");
        }

        public static Settings Load(string path)
        {
            Settings s = new Settings();
            s.path = path ?? DefaultPath();
            try
            {
                if (File.Exists(s.path))
                {
                    IDictionary<string, object> d = Json.ParseObject(File.ReadAllText(s.path, Encoding.UTF8));
                    s.GameDir = Json.Str(d, "game_dir");
                    bool? au = Json.Bool(d, "auto_update");
                    if (au.HasValue) s.AutoUpdate = au.Value;
                }
            }
            catch (Exception) { }
            return s;
        }

        public void Save()
        {
            try
            {
                JObj o = new JObj();
                o.Add("game_dir", GameDir);
                o.Add("auto_update", AutoUpdate);
                Util.WriteFileSafe(path, new UTF8Encoding(false).GetBytes(Json.Write(o)));
            }
            catch (Exception) { }
        }
    }

    public static class SelfUpdate
    {
        public static string ExePath()
        {
            return Path.GetFullPath(Assembly.GetEntryAssembly().Location);
        }

        // A leftover of an earlier self-update (the old exe could not be deleted while it ran).
        public static void CleanupOld()
        {
            string old = ExePath() + ".old";
            if (!File.Exists(old))
                return;
            Thread t = new Thread(delegate()
            {
                for (int i = 0; i < 20 && File.Exists(old); i++)
                {
                    try { Util.DeleteFile(old); }
                    catch (Exception) { Thread.Sleep(500); }
                }
            });
            t.IsBackground = true;
            t.Start();
        }

        // Is the release's launcher another one than this? null = cannot tell without downloading it.
        public static bool? Differs(ReleaseInfo info)
        {
            if (info == null || info.Launcher == null)
                return false;
            if (info.Launcher.Sha256 == null)
                return null;
            return info.Launcher.Sha256 != Util.Sha256File(ExePath());
        }

        // Downloads the release's launcher and puts it in place of this one. Returns false when it is the same
        // file after all. After true the caller restarts (Restart) and exits.
        public static bool Apply(ReleaseInfo info, GitHub.Progress progress)
        {
            string exe = ExePath();
            string fresh = exe + ".new", old = exe + ".old";
            string sha = GitHub.Download(info.Launcher, fresh, progress);
            try
            {
                if (sha == Util.Sha256File(exe))
                    return false;
                byte[] head = new byte[2];
                using (FileStream fs = File.OpenRead(fresh))
                    if (fs.Read(head, 0, 2) != 2 || head[0] != 'M' || head[1] != 'Z')
                        throw new ReleaseException("the downloaded launcher is not a program file");
                Util.DeleteFile(old);
                File.Move(exe, old);           // a running exe can be renamed, not overwritten
                try { File.Move(fresh, exe); }
                catch
                {
                    File.Move(old, exe);
                    throw;
                }
                return true;
            }
            finally
            {
                try { Util.DeleteFile(fresh); } catch (Exception) { }
            }
        }

        public static void Restart(string[] args)
        {
            StringBuilder sb = new StringBuilder();
            foreach (string a in args)
            {
                if (a == "--self-updated") continue;
                sb.Append(Quote(a)).Append(' ');
            }
            sb.Append("--self-updated");
            Process.Start(new ProcessStartInfo(ExePath(), sb.ToString()));
        }

        // one argument for CommandLineToArgvW-style parsing
        static string Quote(string a)
        {
            if (a.Length > 0 && a.IndexOfAny(new char[] { ' ', '\t', '"' }) < 0)
                return a;
            StringBuilder sb = new StringBuilder("\"");
            int bs = 0;
            foreach (char c in a)
            {
                if (c == '\\') { bs++; continue; }
                if (c == '"') { sb.Append('\\', bs * 2 + 1).Append('"'); bs = 0; continue; }
                sb.Append('\\', bs).Append(c);
                bs = 0;
            }
            sb.Append('\\', bs * 2).Append('"');
            return sb.ToString();
        }
    }

    public static class Program
    {
        public static string Version
        {
            get
            {
                Version v = Assembly.GetExecutingAssembly().GetName().Version;
                return v.Major + "." + v.Minor + "." + v.Build;
            }
        }

        [DllImport("kernel32.dll")]
        static extern bool AttachConsole(int pid);

        [DllImport("kernel32.dll")]
        static extern IntPtr GetStdHandle(int which);

        [DllImport("user32.dll")]
        static extern bool SetProcessDPIAware();

        static bool quiet;

        static void Out(string line)
        {
            if (!quiet)
                Console.Out.WriteLine(line);
        }

        static void Err(string line)
        {
            Console.Error.WriteLine(line);
        }

        [STAThread]
        static int Main(string[] args)
        {
            LauncherOptions opt = new LauncherOptions();
            string action = null;
            for (int i = 0; i < args.Length; i++)
            {
                string a = args[i].ToLowerInvariant();
                if ((a == "--game" || a == "--zip" || a == "--settings") && i + 1 < args.Length)
                {
                    string v = args[++i];
                    if (a == "--game") opt.GameDir = v;
                    else if (a == "--zip") opt.ZipPath = v;
                    else opt.SettingsPath = v;
                }
                else if (a == "--install" || a == "--update" || a == "--uninstall" || a == "--status")
                {
                    if (action != null && action != a)
                        return Usage("only one of --install, --update, --uninstall, --status");
                    action = a;
                }
                else if (a == "--quiet") quiet = true;
                else if (a == "--no-self-update") opt.NoSelfUpdate = true;
                else if (a == "--self-updated") opt.SelfUpdated = true;
                else if (a == "--help" || a == "-h" || a == "/?") return Usage(null);
                else return Usage("unknown argument: " + args[i]);
            }
            if (action == null)
            {
                if (quiet) return Usage("--quiet needs one of --install, --update, --uninstall, --status");
                SelfUpdate.CleanupOld();
                try { SetProcessDPIAware(); } catch (Exception) { }
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new MainForm(opt, args));
                return 0;
            }
            return RunCommand(opt, action);
        }

        static void SetupConsole()
        {
            IntPtr h = GetStdHandle(-11);
            if (h == IntPtr.Zero || h == new IntPtr(-1))
                AttachConsole(-1);
        }

        static int Usage(string error)
        {
            SetupConsole();
            if (error != null)
                Err("error: " + error);
            Console.Out.WriteLine("MiusPlayer launcher " + Version);
            Console.Out.WriteLine("  MiusPlayerLauncher.exe                     opens the launcher window");
            Console.Out.WriteLine("  MiusPlayerLauncher.exe --game <dir> [--zip <release zip>] (--install | --update | --uninstall | --status) [--quiet]");
            Console.Out.WriteLine("    --zip     use this release zip (MiusPlayer-X.Y.Z.zip) instead of GitHub's latest release; no network");
            Console.Out.WriteLine("    --status  prints the game build, the installed version (and the zip's version)");
            Console.Out.WriteLine("    --quiet   prints errors only");
            Console.Out.WriteLine("  exit codes: 0 ok, 1 failed, 2 usage, 3 no game, 4 game running, 5 build not supported,");
            Console.Out.WriteLine("              6 bad release, 7 programs not made, 8 not installed, 9 mpmod package enabled, 10 no release");
            return error != null ? ExitCodes.Usage : ExitCodes.Ok;
        }

        static int RunCommand(LauncherOptions opt, string action)
        {
            SetupConsole();
            if (string.IsNullOrEmpty(opt.GameDir))
                return Usage(action + " needs --game <game folder>");
            try
            {
                GameInfo g = GameInfo.Read(opt.GameDir);
                if (action == "--status")
                    return Status(g, opt);
                Installer.CheckGameFolder(g);
                Installer.Log log = delegate(string line) { Out("  " + line); };
                if (action == "--uninstall")
                {
                    Installer.Uninstall(g, log);
                    Out("MiusPlayer removed from " + g.Dir);
                    return ExitCodes.Ok;
                }
                if (action == "--update" && g.Record == null)
                    throw new InstallException(ExitCodes.NotInstalled, "The mod is not installed in this game (nothing to update) - install it first.");
                ReleasePackage pkg = GetPackage(opt, g, action == "--update");
                if (pkg == null)
                {
                    Out("already up to date (" + g.Record.Version + ")");
                    return ExitCodes.Ok;
                }
                if (action == "--install")
                {
                    Installer.Install(g, pkg, log);
                    Out("MiusPlayer " + pkg.Manifest.Version + " installed in " + g.Dir);
                }
                else
                {
                    if (Installer.Update(g, pkg, log))
                        Out("MiusPlayer updated to " + pkg.Manifest.Version + " in " + g.Dir);
                    else
                        Out("already up to date (" + g.Record.Version + ")");
                }
                return ExitCodes.Ok;
            }
            catch (InstallException e) { Err("error: " + e.Message); return e.Code; }
            catch (ReleaseException e) { Err("error: " + e.Message); return ExitCodes.BadRelease; }
            catch (Exception e) { Err("error: " + e.Message); return ExitCodes.Failed; }
        }

        // The release to install: the --zip, or GitHub's latest (null for --update when nothing newer is there).
        static ReleasePackage GetPackage(LauncherOptions opt, GameInfo g, bool update)
        {
            if (opt.ZipPath != null)
                return ReleasePackage.Load(opt.ZipPath);
            Out("checking " + GitHub.LatestUrl);
            ReleaseInfo info = GitHub.Latest();
            if (info == null)
                throw new InstallException(ExitCodes.NoRelease, "No release of MiusPlayer has been published yet.");
            Out("latest release: " + info.Version);
            if (update && Util.CompareVersions(info.Version, g.Record.Version) <= 0 && !g.InstallStale() && g.Record.Complete
                && Installer.AllPresent(g))
                return null;
            Out("downloading " + (info.Zip != null ? info.Zip.Name : "?"));
            return GitHub.DownloadPackage(info, null);
        }

        static int Status(GameInfo g, LauncherOptions opt)
        {
            Console.Out.WriteLine("game: " + g.Dir);
            if (!g.Exists)
            {
                Console.Out.WriteLine("build: none (no starter_x64.exe in this folder)");
                return ExitCodes.NoGame;
            }
            Console.Out.WriteLine("build: " + (g.BuildLabel ?? "unknown"));
            Console.Out.WriteLine("build_name: " + g.BuildText());
            Console.Out.WriteLine("steam: " + (g.Steam != null ? g.Steam.Describe() : "not in a Steam library"));
            if (g.Record != null)
            {
                Console.Out.WriteLine("installed: " + g.Record.Version);
                Console.Out.WriteLine("installed_detail: network " + (g.Record.Network.HasValue ? g.Record.Network.Value.ToString() : "?")
                    + ", for " + g.Record.Build + ", " + g.Record.Files.Count + " files" + (g.Record.Complete ? "" : ", INCOMPLETE")
                    + (Installer.AllPresent(g) ? "" : ", files missing"));
                if (g.InstallStale())
                    Console.Out.WriteLine("warning: the game changed since the mod was installed (now " + (g.BuildLabel ?? "unknown")
                        + "): install or update again");
            }
            else
                Console.Out.WriteLine("installed: none" + (g.LegacyMod ? " (an older multiplayer version without a record is in the game)" : "")
                    + (g.RecordError != null ? " (mp_installed.json is damaged: " + g.RecordError + ")" : ""));
            if (g.PackageEnabled != null)
                Console.Out.WriteLine("package: " + g.PackageEnabled + " is enabled in this game");
            List<RunningProcess> running = Processes.RunningIn(g.Dir, Processes.GameNames);
            Console.Out.WriteLine("running: " + (running.Count > 0 ? Processes.Describe(running) : "no"));
            if (opt.ZipPath != null)
            {
                try
                {
                    ReleasePackage pkg = ReleasePackage.Load(opt.ZipPath);
                    Console.Out.WriteLine("release: " + pkg.Manifest.Version + " (network "
                        + (pkg.Manifest.Network.HasValue ? pkg.Manifest.Network.Value.ToString() : "?") + ")");
                    Console.Out.WriteLine("release_supports_build: " + (pkg.Manifest.Supports(g.BuildLabel) ? "yes" : "no"));
                }
                catch (ReleaseException e)
                {
                    Console.Out.WriteLine("release: invalid (" + e.Message + ")");
                    return ExitCodes.BadRelease;
                }
            }
            return ExitCodes.Ok;
        }
    }
}
