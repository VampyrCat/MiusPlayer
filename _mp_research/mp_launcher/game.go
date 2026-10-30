package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"syscall"

	"golang.org/x/sys/windows/registry"
)

// The launcher only prepares what the multiplayer module reads when the game starts (session_start and
// ui_start in basic_network): the environment, and for a host the battle description on disk.

const imagePath = `data\common\shared\config\tacgame_image.cfgpack`

// same limit as the address field of the game's own join screen (g_address in ui.cpp)
var validAddress = regexp.MustCompile(`^[A-Za-z0-9.:-]{1,63}$`)

func isGameFolder(dir string) bool {
	_, err := os.Stat(filepath.Join(dir, "starter_x64.exe"))
	return err == nil
}

// gameFolder finds the game: the folder given on the command line, else this program's folder or the one
// above it (mpmod1 sits in the game folder), else the player's Steam libraries. "" when it is in none of them.
func gameFolder(given string) (string, error) {
	if given != "" {
		if !isGameFolder(given) {
			return "", fmt.Errorf("no starter_x64.exe in %s.", given)
		}
		return given, nil
	}
	exe, err := os.Executable()
	if err != nil {
		return "", err
	}
	here := filepath.Dir(exe)
	for _, dir := range []string{here, filepath.Dir(here)} {
		if isGameFolder(dir) {
			return dir, nil
		}
	}
	return findInSteam(), nil
}

// checkReady says why the game cannot start a multiplayer session from here, if it cannot.
func checkReady(root string) error {
	for _, f := range []string{`bin\rel\x64\basic_network.codelib`, `root\programs\tacgame.progpack`, `root\programs\opgame.progpack`, "steam_appid.txt"} {
		if _, err := os.Stat(filepath.Join(root, f)); err != nil {
			return errors.New("Multiplayer is not installed in this game. Start this program again to install it.")
		}
	}
	if !steamRunning() {
		return errors.New("Steam is not running. Start Steam first.")
	}
	return nil
}

func steamRunning() bool {
	key, err := registry.OpenKey(registry.CURRENT_USER, `Software\Valve\Steam\ActiveProcess`, registry.QUERY_VALUE)
	if err != nil {
		return false
	}
	defer key.Close()
	pid, _, err := key.GetIntegerValue("pid")
	return err == nil && pid != 0
}

// profileID reads the current profile's folder name, as the module does.
func profileID(root string) string {
	text, err := os.ReadFile(filepath.Join(root, `users\profiles\cur_profile.engcfg`))
	if err == nil {
		if _, rest, ok := strings.Cut(string(text), "cur_id"); ok {
			if _, value, ok := strings.Cut(rest, "="); ok {
				value = strings.TrimLeft(value, " \t")
				if n := strings.IndexAny(value, ";\r\n"); n > 0 {
					return value[:n]
				}
			}
		}
	}
	return "cloud"
}

func lastHost(root string) string {
	text, err := os.ReadFile(filepath.Join(root, `users\mp_last_host.txt`))
	if err != nil {
		return ""
	}
	line, _, _ := strings.Cut(string(text), "\n")
	return strings.TrimSpace(line)
}

// environment is this program's environment with every setting of an earlier multiplayer session taken out.
func environment(settings ...string) []string {
	var env []string
	for _, kv := range os.Environ() {
		if !strings.HasPrefix(strings.ToUpper(kv), "MP_") {
			env = append(env, kv)
		}
	}
	return append(env, settings...)
}

// startGame starts the game. The engine reads its arguments from the raw command line, so that is passed as it is.
func startGame(root, arguments string, env []string) error {
	exe := filepath.Join(root, "starter_x64.exe")
	line := `"` + exe + `"`
	if arguments != "" {
		line += " " + arguments
	}
	cmd := exec.Cmd{Path: exe, Dir: root, Env: env, SysProcAttr: &syscall.SysProcAttr{CmdLine: line}}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("could not start the game: %w", err)
	}
	return cmd.Process.Release()
}

// hostMission starts the mission's battle directly, as host. The module listens as soon as the battle program
// runs; a joining game downloads the description from there and enters the deployment. swap: the host
// commands the mission's enemy force (the module turns the description round, as for SWAP SIDES in the lobby).
func hostMission(root string, m Mission, swap bool) error {
	if err := checkReady(root); err != nil {
		return err
	}
	image, err := missionImage(m.Path)
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(root, imagePath), image, 0o644); err != nil {
		return fmt.Errorf("could not write the battle description: %w", err)
	}
	flag := "0"
	if swap {
		flag = "1"
	}
	arguments := fmt.Sprintf(`root\programs\tacgame.progpack, %s, users\profiles, %s, *:*, 0, 1`, imagePath, profileID(root))
	return startGame(root, arguments, environment("MP_ROLE=host", "MP_HOST_MODE=mission", "MP_SWAP="+flag))
}

// joinGame starts the game normally. On reaching the main menu the module connects to the address by itself and
// is ready at once; whatever the host has started (a mission, a skirmish, an operation) follows from there.
func joinGame(root, address string) error {
	address = strings.TrimSpace(address)
	if !validAddress.MatchString(address) {
		return errors.New("Enter the host's address (for example 203.0.113.7 or 203.0.113.7:1943).")
	}
	if err := checkReady(root); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(root, `users\mp_last_host.txt`), []byte(address+"\n"), 0o644); err != nil {
		return fmt.Errorf("could not save the address: %w", err)
	}
	return startGame(root, "", environment("MP_AUTOJOIN="+address))
}
