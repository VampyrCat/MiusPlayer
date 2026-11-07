// The launcher window: the game folder, its build, the installed and the latest version, and
// Install / Update / Uninstall / Play. Checks GitHub on every start; "Update automatically" (default on)
// updates the launcher itself and an installed mod when a newer release is out and the game is not running.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Windows.Forms;

namespace MiusPlayer
{
    public class MainForm : Form
    {
        readonly LauncherOptions opt;
        readonly string[] args;
        readonly Settings settings;

        GameInfo game;
        ReleaseInfo latest;         // GitHub's latest release (null: none / not checked / --zip)
        bool checking, checkedOnce, noRelease;
        string checkError;
        ReleasePackage pkg;         // the release ready to install (downloaded, or the --zip)
        bool launcherDiffers;
        bool busy;

        TextBox gameBox, logBox;
        Button browseBtn, installBtn, updateBtn, uninstallBtn, playBtn, checkBtn, launcherBtn;
        Label buildVal, steamVal, installedVal, latestVal, supportVal, statusLbl;
        CheckBox autoBox;
        ProgressBar bar;

        public MainForm(LauncherOptions opt, string[] args)
        {
            this.opt = opt;
            this.args = args;
            settings = Settings.Load(opt.SettingsPath);
            BuildUi();
            Shown += delegate { Start(); };
            FormClosing += delegate(object s, FormClosingEventArgs e)
            {
                // an install or uninstall half done would leave the game half changed
                if (busy && e.CloseReason == CloseReason.UserClosing)
                {
                    e.Cancel = true;
                    MessageBox.Show(this, "Please wait until this has finished.", Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
                }
            };
        }

        // ---------------------------------------------------------------- layout

        void BuildUi()
        {
            Text = "MiusPlayer Launcher " + Program.Version;
            Font = SystemFonts.MessageBoxFont;
            AutoScaleMode = AutoScaleMode.Font;
            StartPosition = FormStartPosition.CenterScreen;
            int em = Font.Height;
            ClientSize = new Size(em * 46, em * 30);
            MinimumSize = new Size(em * 36, em * 24);

            TableLayoutPanel root = new TableLayoutPanel();
            root.Dock = DockStyle.Fill;
            root.Padding = new Padding(em / 2);
            root.ColumnCount = 1;
            root.RowCount = 6;
            for (int i = 0; i < 5; i++)
                root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            Controls.Add(root);

            Label head = new Label();
            head.Text = "MiusPlayer - multiplayer for Graviteam Tactics: Mius-Front";
            head.Font = new Font(Font.FontFamily, Font.Size * 1.25f, FontStyle.Bold);
            head.AutoSize = true;
            head.Margin = new Padding(3, 3, 3, em / 2);
            root.Controls.Add(head);

            TableLayoutPanel info = new TableLayoutPanel();
            info.Dock = DockStyle.Fill;
            info.AutoSize = true;
            info.ColumnCount = 3;
            info.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            info.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            info.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            root.Controls.Add(info);

            gameBox = new TextBox();
            gameBox.ReadOnly = true;
            gameBox.Dock = DockStyle.Fill;
            browseBtn = MakeButton("Browse...", OnBrowse);
            AddRow(info, "Game folder:", gameBox, browseBtn);
            buildVal = AddRow(info, "Game build:");
            steamVal = AddRow(info, "Steam:");
            installedVal = AddRow(info, "Installed:");
            latestVal = AddRow(info, "Latest version:");
            supportVal = AddRow(info, "Your game build:");

            autoBox = new CheckBox();
            autoBox.Text = "Update automatically (the launcher and the installed mod, when the game is not running)";
            autoBox.AutoSize = true;
            autoBox.Checked = settings.AutoUpdate;
            autoBox.Margin = new Padding(3, em / 2, 3, em / 4);
            autoBox.CheckedChanged += delegate { settings.AutoUpdate = autoBox.Checked; settings.Save(); };
            root.Controls.Add(autoBox);

            FlowLayoutPanel buttons = new FlowLayoutPanel();
            buttons.AutoSize = true;
            buttons.Dock = DockStyle.Fill;
            buttons.WrapContents = true;
            installBtn = MakeButton("Install", OnInstall);
            updateBtn = MakeButton("Update", OnUpdate);
            uninstallBtn = MakeButton("Uninstall", OnUninstall);
            playBtn = MakeButton("Play", OnPlay);
            checkBtn = MakeButton("Check for updates", delegate { StartCheck(false); });
            launcherBtn = MakeButton("Update launcher", delegate { StartSelfUpdate(false); });
            launcherBtn.Visible = false;
            buttons.Controls.AddRange(new Control[] { installBtn, updateBtn, uninstallBtn, playBtn, checkBtn, launcherBtn });
            root.Controls.Add(buttons);

            TableLayoutPanel statusRow = new TableLayoutPanel();
            statusRow.AutoSize = true;
            statusRow.Dock = DockStyle.Fill;
            statusRow.ColumnCount = 2;
            statusRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            statusRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, em * 10));
            statusLbl = new Label();
            statusLbl.AutoSize = true;
            statusLbl.Dock = DockStyle.Fill;
            statusLbl.MaximumSize = new Size(em * 60, 0);
            statusLbl.Font = new Font(Font, FontStyle.Bold);
            statusLbl.Margin = new Padding(3, em / 4, 3, em / 4);
            bar = new ProgressBar();
            bar.Dock = DockStyle.Fill;
            bar.Visible = false;
            statusRow.Controls.Add(statusLbl, 0, 0);
            statusRow.Controls.Add(bar, 1, 0);
            root.Controls.Add(statusRow);

            logBox = new TextBox();
            logBox.Multiline = true;
            logBox.ReadOnly = true;
            logBox.ScrollBars = ScrollBars.Vertical;
            logBox.Dock = DockStyle.Fill;
            logBox.BackColor = SystemColors.Window;
            root.Controls.Add(logBox);
        }

        Button MakeButton(string text, EventHandler click)
        {
            Button b = new Button();
            b.Text = text;
            b.AutoSize = true;
            b.AutoSizeMode = AutoSizeMode.GrowOnly;
            b.MinimumSize = new Size(Font.Height * 6, 0);
            b.Padding = new Padding(Font.Height / 3, 2, Font.Height / 3, 2);
            b.Click += click;
            return b;
        }

        Label AddRow(TableLayoutPanel t, string caption)
        {
            Label v = new Label();
            v.AutoSize = true;
            v.Dock = DockStyle.Fill;
            v.TextAlign = ContentAlignment.MiddleLeft;
            v.MaximumSize = new Size(Font.Height * 34, 0);
            AddRow(t, caption, v, null);
            return v;
        }

        void AddRow(TableLayoutPanel t, string caption, Control value, Control extra)
        {
            int row = t.RowCount;
            t.RowCount = row + 1;
            t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            Label c = new Label();
            c.Text = caption;
            c.AutoSize = true;
            c.Dock = DockStyle.Fill;
            c.TextAlign = ContentAlignment.MiddleLeft;
            t.Controls.Add(c, 0, row);
            t.Controls.Add(value, 1, row);
            if (extra != null)
                t.Controls.Add(extra, 2, row);
        }

        // ---------------------------------------------------------------- state

        // runs f on the window's thread; nothing when the window is gone
        void OnUi(MethodInvoker f)
        {
            try
            {
                if (!IsDisposed && IsHandleCreated)
                    BeginInvoke(f);
            }
            catch (InvalidOperationException) { }
        }

        void Log(string line)
        {
            if (IsDisposed)
                return;
            if (InvokeRequired)
            {
                OnUi(delegate { Log(line); });
                return;
            }
            logBox.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + line + Environment.NewLine);
        }

        void Progress(long done, long total)
        {
            OnUi(delegate
            {
                bar.Visible = true;
                if (total > 0)
                {
                    bar.Style = ProgressBarStyle.Continuous;
                    bar.Value = (int)Math.Max(0, Math.Min(100, done * 100 / total));
                }
                else
                    bar.Style = ProgressBarStyle.Marquee;
            });
        }

        void Start()
        {
            string dir = null;
            if (!string.IsNullOrEmpty(opt.GameDir))
                dir = opt.GameDir;
            else if (GameInfo.LooksLikeGame(settings.GameDir))
                dir = settings.GameDir;
            else
            {
                dir = Steam.FindGame();
                if (dir != null)
                    Log("found the game through Steam: " + dir);
            }
            SetGame(dir);
            if (opt.SelfUpdated)
                Log("the launcher was updated to " + Program.Version);
            StartCheck(true);
        }

        void SetGame(string dir)
        {
            game = null;
            if (dir != null)
            {
                try { game = GameInfo.Read(dir); }
                catch (Exception e) { Log("could not read the game folder: " + e.Message); }
            }
            gameBox.Text = game != null ? game.Dir : "";
            ShowState();
        }

        void RefreshGame()
        {
            if (game != null)
                SetGame(game.Dir);
            else
                ShowState();
        }

        bool GameOk { get { return game != null && game.Exists; } }

        string LatestVersion
        {
            get
            {
                if (opt.ZipPath != null)
                    return pkg != null ? pkg.Manifest.Version : null;
                return latest != null ? latest.Version : null;
            }
        }

        bool NewerThanInstalled
        {
            get
            {
                return GameOk && game.Record != null && LatestVersion != null
                    && Util.CompareVersions(LatestVersion, game.Record.Version) > 0;
            }
        }

        bool NeedsUpdate
        {
            get
            {
                return GameOk && game.Record != null
                    && (NewerThanInstalled || game.InstallStale() || !game.Record.Complete || !Installer.AllPresent(game));
            }
        }

        // a release has to be fetched for this game (to install it or to see whether it supports this build)
        bool WantsPackage
        {
            get { return GameOk && (game.Record == null || NeedsUpdate); }
        }

        void ShowState()
        {
            if (game == null)
            {
                buildVal.Text = "-";
                steamVal.Text = "-";
                installedVal.Text = "-";
            }
            else if (!game.Exists)
            {
                buildVal.Text = "no game in this folder (starter_x64.exe is missing)";
                steamVal.Text = "-";
                installedVal.Text = "-";
            }
            else
            {
                buildVal.Text = game.BuildText();
                steamVal.Text = game.Steam != null ? game.Steam.Describe() : "not in a Steam library";
                if (game.Record != null)
                {
                    string s = game.Record.Version + (game.Record.Network.HasValue ? "  (network " + game.Record.Network.Value + ")" : "");
                    if (game.InstallStale())
                        s += "  - made for " + game.Record.Build + ": the game was updated since, the mod must be installed again";
                    else if (!game.Record.Complete || !Installer.AllPresent(game))
                        s += "  - incomplete: install it again";
                    installedVal.Text = s;
                }
                else if (game.RecordError != null)
                    installedVal.Text = "unknown (mp_installed.json is damaged)";
                else if (game.LegacyMod)
                    installedVal.Text = "an older multiplayer version (not installed by this launcher)";
                else
                    installedVal.Text = "not installed";
            }

            if (checking)
                latestVal.Text = "checking...";
            else if (opt.ZipPath != null)
                latestVal.Text = pkg != null ? pkg.Manifest.Version + "  (from " + Path.GetFileName(opt.ZipPath) + ")"
                                             : "-  (" + (checkError ?? "the release file could not be read") + ")";
            else if (noRelease)
                latestVal.Text = "no release has been published yet";
            else if (checkError != null)
                latestVal.Text = "could not check: " + checkError;
            else if (latest != null)
                latestVal.Text = latest.Version + (NewerThanInstalled ? "  - newer than the installed one" : "");
            else
                latestVal.Text = checkedOnce ? "-" : "not checked yet";

            if (!GameOk)
                supportVal.Text = "-";
            else if (pkg != null)
                supportVal.Text = pkg.Manifest.Supports(game.BuildLabel)
                    ? "supported by version " + pkg.Manifest.Version
                    : "this version of the mod (" + pkg.Manifest.Version + ") does not support your game build yet";
            else if (game.Record != null && !game.InstallStale())
                supportVal.Text = "supported by the installed version";
            else if (game.Build == null)
                supportVal.Text = "unknown build: probably not supported yet";
            else
                supportVal.Text = "-";

            statusLbl.Text = Advice();
            UpdateButtons();
        }

        string Advice()
        {
            if (busy || checking)
                return statusLbl.Text;
            if (game == null)
                return "The game was not found. Click Browse and pick the game folder (the one with starter_x64.exe).";
            if (!game.Exists)
                return "This folder has no starter_x64.exe. Click Browse and pick the game folder.";
            if (game.PackageEnabled != null)
                return "The " + game.PackageEnabled + " package is enabled in this game: run " + game.PackageEnabled
                     + "\\Disable_Multiplayer.bat before using the launcher.";
            bool supported = pkg != null && pkg.Manifest.Supports(game.BuildLabel);
            if (game.Record == null)
            {
                if (pkg == null)
                    return noRelease ? "No release to install yet." : "Waiting for a release to install.";
                return supported ? "Click Install to add multiplayer to this game."
                                 : "This version of the mod does not support your game build yet.";
            }
            if (NeedsUpdate)
            {
                if (pkg == null)
                    return game.InstallStale() ? "The game was updated since the mod was installed: update the mod, or uninstall it."
                                               : "An update is needed.";
                return supported ? "Click Update." : "The newest version of the mod does not support your game build yet: uninstall the mod to play the game without it.";
            }
            return "MiusPlayer is installed and up to date. The main menu has a MULTIPLAYER entry. Click Play.";
        }

        void UpdateButtons()
        {
            bool free = !busy && !checking;
            bool canAct = free && GameOk && game.PackageEnabled == null;
            // a release to install: downloaded and supporting this build, or not downloaded yet (checked on install)
            bool installable = pkg != null ? pkg.Manifest.Supports(game != null ? game.BuildLabel : null)
                                           : (latest != null && latest.Zip != null);
            browseBtn.Enabled = free;
            installBtn.Enabled = canAct && game.Record == null && installable;
            updateBtn.Enabled = canAct && NeedsUpdate && installable;
            uninstallBtn.Enabled = canAct && game.Record != null;
            playBtn.Enabled = free && GameOk;
            checkBtn.Enabled = free;
            launcherBtn.Visible = launcherDiffers;
            launcherBtn.Enabled = free && launcherDiffers;
        }

        void SetBusy(bool on, string text)
        {
            busy = on;
            if (text != null)
                statusLbl.Text = text;
            if (!on)
                bar.Visible = false;
            UpdateButtons();
        }

        // Runs work on a background thread; then (on the window's thread) done(error or null).
        void Background(ThreadStart work, Action<Exception> done)
        {
            Thread t = new Thread(delegate()
            {
                Exception err = null;
                try { work(); }
                catch (Exception e) { err = e; }
                OnUi(delegate { done(err); });
            });
            t.IsBackground = true;
            t.Start();
        }

        // ---------------------------------------------------------------- update check

        void StartCheck(bool automatic)
        {
            if (busy || checking)
                return;
            checking = true;
            checkError = null;
            statusLbl.Text = "Checking for updates...";
            ShowState();
            ReleaseInfo info = null;
            ReleasePackage found = null;
            bool none = false, differs = false;
            GameInfo g = GameOk ? game : null;
            bool selfCheck = !opt.NoSelfUpdate && !opt.SelfUpdated && opt.ZipPath == null;
            Background(delegate
            {
                if (opt.ZipPath != null)
                {
                    found = ReleasePackage.Load(opt.ZipPath);
                    return;
                }
                info = GitHub.Latest();
                if (info == null)
                {
                    none = true;
                    return;
                }
                if (selfCheck && info.Launcher != null)
                {
                    bool? d = SelfUpdate.Differs(info);
                    if (!d.HasValue)
                    {
                        // GitHub gave no digest: fetch the launcher to compare
                        string tmp = Path.Combine(GitHub.TempDir(), "MiusPlayerLauncher.check");
                        info.Launcher.Sha256 = GitHub.Download(info.Launcher, tmp, null);
                        try { Util.DeleteFile(tmp); } catch (Exception) { }
                        d = SelfUpdate.Differs(info);
                    }
                    differs = d.HasValue && d.Value;
                }
                // the zip is fetched when there is something to install (it also tells which builds it supports)
                bool want = g != null && (g.Record == null || Util.CompareVersions(info.Version, g.Record.Version) > 0
                                          || g.InstallStale() || !g.Record.Complete || !Installer.AllPresent(g));
                if (want && info.Zip != null)
                    found = GitHub.DownloadPackage(info, Progress);
            }, delegate(Exception err)
            {
                checking = false;
                checkedOnce = true;
                bar.Visible = false;
                noRelease = none;
                if (opt.ZipPath == null)
                    latest = info;
                if (found != null || opt.ZipPath != null)
                    pkg = found;
                else if (info != null && pkg != null && pkg.Manifest.Version != info.Version)
                    pkg = null;
                launcherDiffers = differs;
                if (err != null)
                {
                    checkError = err.Message;
                    Log("update check: " + err.Message);
                }
                else if (none)
                    Log("no release of MiusPlayer has been published yet");
                else
                    Log("latest version: " + LatestVersion + (opt.ZipPath != null ? " (" + opt.ZipPath + ")" : ""));
                if (launcherDiffers)
                    Log("a new launcher is available");
                ShowState();
                if (automatic && settings.AutoUpdate && err == null)
                    AutoActions();
            });
        }

        void AutoActions()
        {
            if (launcherDiffers && !opt.NoSelfUpdate && !opt.SelfUpdated)
            {
                // the new launcher updates the mod after its restart; when the launcher cannot update, this one does
                StartSelfUpdate(true);
                return;
            }
            AutoModUpdate();
        }

        void AutoModUpdate()
        {
            if (!GameOk || game.Record == null || game.PackageEnabled != null || !NeedsUpdate)
                return;
            if (pkg == null || !pkg.Manifest.Supports(game.BuildLabel))
                return;
            if (Processes.RunningIn(game.Dir, Processes.GameNames).Count > 0)
            {
                Log("an update is ready, but the game is running: it is installed on the next start (or click Update)");
                return;
            }
            Log("updating automatically");
            RunInstall(true);
        }

        void StartSelfUpdate(bool thenMod)
        {
            if (latest == null || latest.Launcher == null)
                return;
            SetBusy(true, "Updating the launcher...");
            bool applied = false;
            ReleaseInfo info = latest;
            Background(delegate { applied = SelfUpdate.Apply(info, Progress); }, delegate(Exception err)
            {
                SetBusy(false, null);
                if (err != null || !applied)
                {
                    if (err != null)
                        Log("the launcher could not update itself: " + err.Message);
                    else
                        launcherDiffers = false;
                    ShowState();
                    if (thenMod)
                        AutoModUpdate();
                    return;
                }
                Log("launcher updated; restarting");
                try
                {
                    SelfUpdate.Restart(args);
                    Close();
                }
                catch (Exception e) { Log("could not restart the launcher: " + e.Message + " - start it again by hand"); }
            });
        }

        // ---------------------------------------------------------------- actions

        void OnBrowse(object sender, EventArgs e)
        {
            using (FolderBrowserDialog d = new FolderBrowserDialog())
            {
                d.Description = "Pick the folder of Graviteam Tactics: Mius-Front (the one with starter_x64.exe).";
                d.ShowNewFolderButton = false;
                if (game != null && Directory.Exists(game.Dir))
                    d.SelectedPath = game.Dir;
                if (d.ShowDialog(this) != DialogResult.OK)
                    return;
                if (!GameInfo.LooksLikeGame(d.SelectedPath))
                {
                    MessageBox.Show(this, "That folder has no starter_x64.exe. Pick the game's own folder\n"
                        + "(Steam: right-click the game, Manage, Browse local files).", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }
                settings.GameDir = Util.FullDir(d.SelectedPath);
                settings.Save();
                SetGame(settings.GameDir);
                Log("game folder: " + game.Dir);
                if (opt.ZipPath == null && WantsPackage && pkg == null)
                    StartCheck(false);
            }
        }

        void OnInstall(object sender, EventArgs e) { RunInstall(false); }

        void OnUpdate(object sender, EventArgs e) { RunInstall(true); }

        void RunInstall(bool update)
        {
            if (!GameOk)
                return;
            GameInfo g = game;
            ReleasePackage have = pkg;
            ReleaseInfo info = latest;
            SetBusy(true, update ? "Updating..." : "Installing...");
            ReleasePackage used = null;
            bool changed = true;
            Background(delegate
            {
                used = have;
                if (used == null)
                {
                    if (info == null || info.Zip == null)
                        throw new ReleaseException("there is no release to install");
                    Log("downloading " + info.Zip.Name);
                    used = GitHub.DownloadPackage(info, Progress);
                }
                Installer.Log log = delegate(string line) { Log(line); };
                if (update)
                    changed = Installer.Update(GameInfo.Read(g.Dir), used, log);
                else
                    Installer.Install(GameInfo.Read(g.Dir), used, log);
            }, delegate(Exception err)
            {
                if (used != null)
                    pkg = used;
                SetBusy(false, null);
                RefreshGame();
                if (err != null)
                {
                    Log("failed: " + err.Message);
                    MessageBox.Show(this, err.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
                else if (changed)
                    Log("MiusPlayer " + used.Manifest.Version + (update ? " updated" : " installed") + ".");
                else
                    Log("already up to date (" + (game.Record != null ? game.Record.Version : "?") + ")");
            });
        }

        void OnUninstall(object sender, EventArgs e)
        {
            if (!GameOk || game.Record == null)
                return;
            if (MessageBox.Show(this, "Remove MiusPlayer from this game?\n\n" + game.Dir + "\n\nThe game is then exactly as it was before.",
                                Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return;
            GameInfo g = game;
            SetBusy(true, "Removing...");
            Background(delegate { Installer.Uninstall(GameInfo.Read(g.Dir), delegate(string line) { Log(line); }); }, delegate(Exception err)
            {
                SetBusy(false, null);
                RefreshGame();
                if (err != null)
                {
                    Log("failed: " + err.Message);
                    MessageBox.Show(this, err.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
                else
                    Log("MiusPlayer removed.");
            });
        }

        void OnPlay(object sender, EventArgs e)
        {
            if (!GameOk)
                return;
            RefreshGame();
            if (game.Record != null && game.InstallStale())
            {
                if (MessageBox.Show(this, "The game was updated since MiusPlayer was installed, so the installed mod does not fit it."
                        + " Update or uninstall the mod first.\n\nStart the game anyway?", Text,
                        MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                    return;
            }
            try
            {
                if (game.Steam != null)
                {
                    Process.Start("steam://rungameid/" + Steam.AppId);
                    Log("starting the game through Steam");
                }
                else
                {
                    ProcessStartInfo si = new ProcessStartInfo(Path.Combine(game.Dir, "starter_x64.exe"));
                    si.WorkingDirectory = game.Dir;
                    si.UseShellExecute = false;
                    Process.Start(si);
                    Log("starting " + si.FileName);
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "The game could not be started: " + ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }
    }
}
