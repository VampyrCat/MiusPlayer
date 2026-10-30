package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	_ "embed"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"syscall"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

// The launcher carries the mpmod1 package (tools\embed_mpmod1.ps1 makes it). Installing means what a player
// does by hand: the package is put into <game>\mpmod1 and its Enable_Multiplayer.bat is run, so the checks,
// the notes in mpmod1\state and Disable_Multiplayer.bat work the same either way.

//go:embed mpmod1.zip
var packageZip []byte

const (
	gameFolderName = "Graviteam Tactics Mius-Front"
	filesPrefix    = "mpmod1/files/"
	launcherName   = "mp_launcher.exe"
)

// The exit codes of Enable_Multiplayer.bat that the launcher has an answer to.
const (
	enableCancelled = 2 // another build of the game
	enableRunning   = 4
)

// errOtherBuild: the game is not the build the multiplayer was made for. Installing with force goes ahead anyway.
var errOtherBuild = errors.New("this game is not on the Steam branch public_beta")

type modState int

const (
	modMissing modState = iota
	modOutdated
	modCurrent
)

func packageFiles() ([]*zip.File, error) {
	r, err := zip.NewReader(bytes.NewReader(packageZip), int64(len(packageZip)))
	if err != nil {
		return nil, err
	}
	return r.File, nil
}

func sha(r io.Reader) ([32]byte, error) {
	h := sha256.New()
	var sum [32]byte
	_, err := io.Copy(h, r)
	copy(sum[:], h.Sum(nil))
	return sum, err
}

func sameFile(entry *zip.File, path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	e, err := entry.Open()
	if err != nil {
		return false
	}
	defer e.Close()
	a, errA := sha(f)
	b, errB := sha(e)
	return errA == nil && errB == nil && a == b
}

// installedState compares the game with the package. A steam_appid.txt only has to be there: a game may have
// its own, which installing leaves alone.
func installedState(root string) (modState, error) {
	entries, err := packageFiles()
	if err != nil {
		return modMissing, err
	}
	state := modCurrent
	for _, e := range entries {
		rel, ok := strings.CutPrefix(e.Name, filesPrefix)
		if !ok {
			continue
		}
		path := filepath.Join(root, filepath.FromSlash(rel))
		if _, err := os.Stat(path); err != nil {
			if rel == "bin/rel/x64/basic_network.codelib" {
				return modMissing, nil
			}
			state = modOutdated
		} else if rel != "steam_appid.txt" && !sameFile(e, path) {
			state = modOutdated
		}
	}
	return state, nil
}

func unpack(root string) error {
	entries, err := packageFiles()
	if err != nil {
		return err
	}
	for _, e := range entries {
		if strings.HasSuffix(e.Name, "/") {
			continue
		}
		path := filepath.Join(root, filepath.FromSlash(e.Name))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return err
		}
		in, err := e.Open()
		if err != nil {
			return err
		}
		out, err := os.Create(path)
		if err != nil {
			in.Close()
			return err
		}
		_, err = io.Copy(out, in)
		in.Close()
		if closed := out.Close(); err == nil {
			err = closed
		}
		if err != nil {
			return err
		}
	}
	return nil
}

// addSelf puts this program into the package, for Enable_Multiplayer.bat to copy into the game.
func addSelf(root string) error {
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	target := filepath.Join(root, "mpmod1", "files", launcherName)
	if strings.EqualFold(exe, target) {
		return nil
	}
	in, err := os.Open(exe)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(target)
	if err != nil {
		return err
	}
	if _, err = io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

// runBat runs one of the package's batch files without a console window and returns its exit code and output.
func runBat(root, name, arguments string) (int, string, error) {
	bat := filepath.Join(root, "mpmod1", name)
	// cmd /c "<quoted program> arguments": the outer quotes are what cmd strips, the path may have spaces and brackets
	line := fmt.Sprintf(`cmd.exe /c ""%s" %s"`, bat, arguments)
	cmd := exec.Cmd{
		Path:        filepath.Join(os.Getenv("SystemRoot"), "System32", "cmd.exe"),
		Dir:         filepath.Dir(bat),
		SysProcAttr: &syscall.SysProcAttr{CmdLine: line, CreationFlags: windows.CREATE_NO_WINDOW},
	}
	out, err := cmd.CombinedOutput()
	var exit *exec.ExitError
	if errors.As(err, &exit) {
		return exit.ExitCode(), string(out), nil
	}
	return 0, string(out), err
}

// installMod puts the package into the game and enables it. force: also in another build of the game.
func installMod(root string, force bool) error {
	if err := unpack(root); err != nil {
		return fmt.Errorf("could not write the mpmod1 folder: %w", err)
	}
	if err := addSelf(root); err != nil {
		return fmt.Errorf("could not write the mpmod1 folder: %w", err)
	}
	arguments := "/quiet"
	if force {
		arguments += " /force"
	}
	code, output, err := runBat(root, "Enable_Multiplayer.bat", arguments)
	switch {
	case err != nil:
		return fmt.Errorf("could not run Enable_Multiplayer.bat: %w", err)
	case code == 0:
		return nil
	case code == enableCancelled:
		return errOtherBuild
	case code == enableRunning:
		return errors.New("The game is running. Close it, then start this program again.")
	}
	return fmt.Errorf("Enable_Multiplayer.bat failed (%d):\n%s", code, strings.TrimSpace(output))
}

var libraryPath = regexp.MustCompile(`"path"\s+"([^"]+)"`)

// steamLibraries lists the Steam library folders named in a libraryfolders.vdf.
func steamLibraries(vdf string) []string {
	var out []string
	for _, m := range libraryPath.FindAllStringSubmatch(vdf, -1) {
		out = append(out, strings.ReplaceAll(m[1], `\\`, `\`))
	}
	return out
}

// findInSteam looks for the game in the player's Steam libraries.
func findInSteam() string {
	key, err := registry.OpenKey(registry.CURRENT_USER, `Software\Valve\Steam`, registry.QUERY_VALUE)
	if err != nil {
		return ""
	}
	defer key.Close()
	steam, _, err := key.GetStringValue("SteamPath")
	if err != nil {
		return ""
	}
	steam = filepath.FromSlash(steam)
	libraries := []string{steam}
	if vdf, err := os.ReadFile(filepath.Join(steam, "steamapps", "libraryfolders.vdf")); err == nil {
		libraries = append(libraries, steamLibraries(string(vdf))...)
	}
	for _, library := range libraries {
		game := filepath.Join(library, "steamapps", "common", gameFolderName)
		if isGameFolder(game) {
			return game
		}
	}
	return ""
}
