// A release of the mod: its manifest, the verified files of its zip, and GitHub Releases.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Text;

namespace MiusPlayer
{
    public class ReleaseException : Exception
    {
        public ReleaseException(string message) : base(message) { }
    }

    public class ManifestFile
    {
        public string Path;     // "bin/rel/x64/basic_network.codelib" (relative to the game folder)
        public string Sha256;   // lower-case hex
    }

    public class ReleaseManifest
    {
        public string Version;
        public long? Network;
        public List<string> Builds = new List<string>();
        public List<ManifestFile> Files = new List<ManifestFile>();

        public bool Supports(string buildLabel)
        {
            return buildLabel != null && Builds.Contains(buildLabel);
        }

        public static bool IsHex64(string s)
        {
            if (s == null || s.Length != 64)
                return false;
            foreach (char c in s)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    return false;
            return true;
        }

        public static ReleaseManifest Parse(string text)
        {
            IDictionary<string, object> d;
            try { d = Json.ParseObject(text); }
            catch (Exception e) { throw new ReleaseException("manifest.json is not valid JSON: " + e.Message); }
            ReleaseManifest m = new ReleaseManifest();
            m.Version = Util.CleanVersion(Json.Str(d, "version"));
            if (Util.ParseVersion(m.Version) == null)
                throw new ReleaseException("manifest.json has no valid version");
            m.Network = Json.Num(d, "network");
            object[] builds = Json.Arr(d, "builds");
            if (builds != null)
                foreach (object b in builds)
                    if (b is string)
                        m.Builds.Add((string)b);
            object[] files = Json.Arr(d, "files");
            if (files == null || files.Length == 0)
                throw new ReleaseException("manifest.json lists no files");
            HashSet<string> seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (object o in files)
            {
                IDictionary<string, object> f = o as IDictionary<string, object>;
                ManifestFile mf = new ManifestFile();
                mf.Path = f != null ? Json.Str(f, "path") : null;
                mf.Sha256 = f != null ? Json.Str(f, "sha256") : null;
                if (mf.Sha256 != null) mf.Sha256 = mf.Sha256.Trim().ToLowerInvariant();
                if (mf.Path == null || Util.SafeRel(mf.Path) == null)
                    throw new ReleaseException("manifest.json has a file path that is not a plain relative path: " + mf.Path);
                mf.Path = Util.NormRel(mf.Path);
                if (!IsHex64(mf.Sha256))
                    throw new ReleaseException("manifest.json has no valid sha256 for " + mf.Path);
                if (Installer.IsReservedPath(mf.Path))
                    throw new ReleaseException("manifest.json lists " + mf.Path + ", which the launcher makes itself");
                if (!seen.Add(mf.Path))
                    throw new ReleaseException("manifest.json lists " + mf.Path + " twice");
                m.Files.Add(mf);
            }
            return m;
        }
    }

    // A release zip, opened and checked: every manifest file is there and has the manifest's SHA-256.
    public class ReleasePackage
    {
        public ReleaseManifest Manifest;
        public string Source;    // the zip's path
        public readonly Dictionary<string, byte[]> Contents = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);

        const long MaxEntry = 256L * 1024 * 1024;

        public static ReleasePackage Load(string zipPath)
        {
            ReleasePackage p = new ReleasePackage();
            p.Source = zipPath;
            try
            {
                using (FileStream fs = new FileStream(zipPath, FileMode.Open, FileAccess.Read, FileShare.Read))
                using (ZipArchive zip = new ZipArchive(fs, ZipArchiveMode.Read))
                {
                    Dictionary<string, ZipArchiveEntry> entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
                    foreach (ZipArchiveEntry e in zip.Entries)
                    {
                        string n = e.FullName.Replace('\\', '/');
                        if (!entries.ContainsKey(n))
                            entries[n] = e;
                    }
                    ZipArchiveEntry me;
                    if (!entries.TryGetValue("mod/manifest.json", out me))
                        throw new ReleaseException("the release zip has no mod/manifest.json");
                    p.Manifest = ReleaseManifest.Parse(Encoding.UTF8.GetString(ReadEntry(me)));
                    foreach (ManifestFile f in p.Manifest.Files)
                    {
                        ZipArchiveEntry fe;
                        if (!entries.TryGetValue("mod/" + f.Path, out fe))
                            throw new ReleaseException("the release zip has no mod/" + f.Path + " (listed in its manifest)");
                        byte[] data = ReadEntry(fe);
                        string sha = Util.Sha256Hex(data);
                        if (sha != f.Sha256)
                            throw new ReleaseException("mod/" + f.Path + " in the release zip does not have the SHA-256 its manifest lists"
                                                       + " (damaged download?): " + sha + " instead of " + f.Sha256);
                        p.Contents[f.Path] = data;
                    }
                }
            }
            catch (InvalidDataException e) { throw new ReleaseException("the release zip is damaged: " + e.Message); }
            catch (IOException e) { throw new ReleaseException("the release zip could not be read: " + e.Message); }
            catch (UnauthorizedAccessException e) { throw new ReleaseException("the release zip could not be read: " + e.Message); }
            return p;
        }

        static byte[] ReadEntry(ZipArchiveEntry e)
        {
            if (e.Length > MaxEntry)
                throw new ReleaseException(e.FullName + " in the release zip is too large");
            using (Stream s = e.Open())
            using (MemoryStream ms = new MemoryStream())
            {
                byte[] buf = new byte[65536];
                int n;
                while ((n = s.Read(buf, 0, buf.Length)) > 0)
                {
                    ms.Write(buf, 0, n);
                    if (ms.Length > MaxEntry)
                        throw new ReleaseException(e.FullName + " in the release zip is too large");
                }
                return ms.ToArray();
            }
        }
    }

    public class ReleaseAsset
    {
        public string Name;
        public string Url;
        public long Size;
        public string Sha256;   // from GitHub's "digest" ("sha256:..."), null when GitHub gives none
    }

    public class ReleaseInfo
    {
        public string Tag;
        public string Version;
        public string PageUrl;
        public ReleaseAsset Zip;
        public ReleaseAsset Launcher;
    }

    public static class GitHub
    {
        public const string Repo = "VampyrCat/MiusPlayer";
        public const string LatestUrl = "https://api.github.com/repos/" + Repo + "/releases/latest";
        public const string UserAgent = "MiusPlayerLauncher";
        public const string LauncherAssetName = "MiusPlayerLauncher.exe";

        static bool tlsSet;

        static void SetupTls()
        {
            if (tlsSet)
                return;
            // .NET Framework 4.x may otherwise offer only TLS 1.0; GitHub needs 1.2 or newer.
            ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
            tlsSet = true;
        }

        static HttpWebRequest Request(string url, string accept)
        {
            SetupTls();
            HttpWebRequest r = (HttpWebRequest)WebRequest.Create(url);
            r.UserAgent = UserAgent;
            r.Accept = accept;
            r.Timeout = 30000;
            r.ReadWriteTimeout = 60000;
            r.AllowAutoRedirect = true;
            r.AutomaticDecompression = DecompressionMethods.GZip | DecompressionMethods.Deflate;
            return r;
        }

        // The latest release, or null when the repository has none yet (404). Throws ReleaseException otherwise.
        public static ReleaseInfo Latest()
        {
            string text;
            try
            {
                HttpWebRequest r = Request(LatestUrl, "application/vnd.github+json");
                r.Headers["X-GitHub-Api-Version"] = "2022-11-28";
                using (HttpWebResponse resp = (HttpWebResponse)r.GetResponse())
                using (StreamReader sr = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                    text = sr.ReadToEnd();
            }
            catch (WebException e)
            {
                HttpWebResponse resp = e.Response as HttpWebResponse;
                if (resp != null && resp.StatusCode == HttpStatusCode.NotFound)
                    return null;
                if (resp != null && (int)resp.StatusCode == 403)
                    throw new ReleaseException("GitHub refused the update check (too many checks from this address?); try again later");
                if (resp != null)
                    throw new ReleaseException("GitHub answered " + (int)resp.StatusCode + " " + resp.StatusDescription);
                throw new ReleaseException("could not reach GitHub: " + e.Message);
            }
            IDictionary<string, object> d;
            try { d = Json.ParseObject(text); }
            catch (Exception e) { throw new ReleaseException("GitHub's answer is not valid JSON: " + e.Message); }
            ReleaseInfo info = new ReleaseInfo();
            info.Tag = Json.Str(d, "tag_name");
            info.Version = Util.CleanVersion(info.Tag);
            info.PageUrl = Json.Str(d, "html_url");
            if (Util.ParseVersion(info.Version) == null)
                throw new ReleaseException("the latest release's tag \"" + info.Tag + "\" is not a version (vX.Y.Z)");
            object[] assets = Json.Arr(d, "assets");
            List<ReleaseAsset> list = new List<ReleaseAsset>();
            if (assets != null)
                foreach (object o in assets)
                {
                    IDictionary<string, object> a = o as IDictionary<string, object>;
                    if (a == null) continue;
                    ReleaseAsset ra = new ReleaseAsset();
                    ra.Name = Json.Str(a, "name");
                    ra.Url = Json.Str(a, "browser_download_url");
                    long? size = Json.Num(a, "size");
                    ra.Size = size.HasValue ? size.Value : -1;
                    string digest = Json.Str(a, "digest");
                    if (digest != null && digest.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase))
                    {
                        string h = digest.Substring(7).Trim().ToLowerInvariant();
                        if (ReleaseManifest.IsHex64(h)) ra.Sha256 = h;
                    }
                    if (ra.Name != null && ra.Url != null && ra.Url.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
                        list.Add(ra);
                }
            string zipName = "MiusPlayer-" + info.Version + ".zip";
            foreach (ReleaseAsset a in list)
                if (string.Equals(a.Name, zipName, StringComparison.OrdinalIgnoreCase)) info.Zip = a;
            if (info.Zip == null)
                foreach (ReleaseAsset a in list)
                    if (a.Name.StartsWith("MiusPlayer-", StringComparison.OrdinalIgnoreCase)
                        && a.Name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) { info.Zip = a; break; }
            foreach (ReleaseAsset a in list)
                if (string.Equals(a.Name, LauncherAssetName, StringComparison.OrdinalIgnoreCase)) info.Launcher = a;
            return info;
        }

        public delegate void Progress(long done, long total);

        // Downloads an asset to a file; checks GitHub's SHA-256 when it gave one. Returns the file's SHA-256.
        public static string Download(ReleaseAsset a, string dest, Progress progress)
        {
            string tmp = dest + ".part";
            try
            {
                HttpWebRequest r = Request(a.Url, "application/octet-stream");
                using (HttpWebResponse resp = (HttpWebResponse)r.GetResponse())
                using (Stream s = resp.GetResponseStream())
                using (FileStream fs = new FileStream(tmp, FileMode.Create, FileAccess.Write))
                {
                    long total = resp.ContentLength > 0 ? resp.ContentLength : a.Size;
                    byte[] buf = new byte[65536];
                    long done = 0;
                    int n;
                    while ((n = s.Read(buf, 0, buf.Length)) > 0)
                    {
                        fs.Write(buf, 0, n);
                        done += n;
                        if (done > 512L * 1024 * 1024)
                            throw new ReleaseException(a.Name + " is far larger than expected");
                        if (progress != null) progress(done, total);
                    }
                }
                string sha = Util.Sha256File(tmp);
                if (a.Sha256 != null && sha != a.Sha256)
                    throw new ReleaseException(a.Name + " was not downloaded correctly (its SHA-256 differs from GitHub's)");
                Util.DeleteFile(dest);
                File.Move(tmp, dest);
                return sha;
            }
            catch (WebException e)
            {
                throw new ReleaseException("could not download " + a.Name + ": " + e.Message);
            }
            finally
            {
                try { Util.DeleteFile(tmp); } catch (Exception) { }
            }
        }

        public static string TempDir()
        {
            string d = Path.Combine(Path.GetTempPath(), "MiusPlayerLauncher");
            Directory.CreateDirectory(d);
            return d;
        }

        // Downloads and opens the release's zip (checked against GitHub's digest and its own manifest).
        public static ReleasePackage DownloadPackage(ReleaseInfo info, Progress progress)
        {
            if (info.Zip == null)
                throw new ReleaseException("release " + info.Tag + " has no MiusPlayer-" + info.Version + ".zip");
            string path = Path.Combine(TempDir(), info.Zip.Name);
            Download(info.Zip, path, progress);
            try { return ReleasePackage.Load(path); }
            finally { try { Util.DeleteFile(path); } catch (Exception) { } }
        }
    }
}
