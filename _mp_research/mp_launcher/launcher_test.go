package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// The test binary doubles as a stand-in for starter_x64.exe: it writes its raw command line, working folder and
// environment to started.txt in the folder it runs in.
func TestMain(m *testing.M) {
	if os.Getenv("FAKE_GAME") != "" {
		dir, _ := os.Getwd()
		line := syscall.UTF16ToString(unsafe.Slice(syscall.GetCommandLine(), 4096))
		out := line + "\n" + dir + "\n" + strings.Join(os.Environ(), "\n") + "\n"
		os.WriteFile(filepath.Join(dir, "started.tmp"), []byte(out), 0o644)
		os.Rename(filepath.Join(dir, "started.tmp"), filepath.Join(dir, "started.txt"))
		return
	}
	os.Exit(m.Run())
}

func config() []byte {
	// header, one struct node "i_gamespec:gamespec" with the two side fields, as image.h reads it
	b := make([]byte, 0x40+0x80+2*0x40)
	copy(b, "AZCF")
	binary.LittleEndian.PutUint32(b[4:], 1)
	binary.LittleEndian.PutUint32(b[8:], 0x40)
	copy(b[0x40:], "i_gamespec:gamespec")
	binary.LittleEndian.PutUint32(b[0x40+0x20:], 1)
	binary.LittleEndian.PutUint32(b[0x40+0x24:], 1)
	binary.LittleEndian.PutUint32(b[0x40+0x28:], 2)
	binary.LittleEndian.PutUint32(b[0x40+0x30:], 0xc0)
	copy(b[0xc0:], "\x47\x45\x52\x00")
	copy(b[0xc0+0x20:], "user_side")
	copy(b[0x100:], "USSR")
	copy(b[0x100+0x20:], "enemy_side")
	return b
}

func pack(image []byte, text string) []byte {
	entries := []struct {
		name, kind string
		data       []byte
	}{{"", "", nil}, {"tacgame_image", "tacimage", image}, {"desc", "config", []byte("not this one")}, {"desc", "text", []byte(text)}}
	out := make([]byte, 0x10000)
	copy(out, "AZP3")
	binary.LittleEndian.PutUint32(out[4:], uint32(len(entries)))
	for i, e := range entries {
		o := 0xc0 + i*0x80
		binary.LittleEndian.PutUint64(out[o+8:], uint64(len(out)))
		binary.LittleEndian.PutUint64(out[o+0x10:], uint64(len(e.data)))
		copy(out[o+0x20:], e.name)
		copy(out[o+0x40:], e.kind)
		out = append(out, e.data...)
	}
	return out
}

func fakeGame(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	write := func(rel string, data []byte) {
		path := filepath.Join(root, rel)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	self, err := os.ReadFile(os.Args[0])
	if err != nil {
		t.Fatal(err)
	}
	write("starter_x64.exe", self)
	for _, f := range []string{`bin\rel\x64\basic_network.codelib`, `root\programs\tacgame.progpack`, `root\programs\opgame.progpack`, "steam_appid.txt"} {
		write(f, nil)
	}
	write(`users\profiles\cur_profile.engcfg`, []byte("//GRAVITECH3 profile system\ni_profiles:profiles()\n{\n\tcur_profile =\tp_x;\n\tcur_id =\tmine;\n}\n"))
	write(`data\common\shared\config\tacgame_image.cfgpack`, []byte("the last battle"))
	return root
}

func started(t *testing.T, root string) string {
	t.Helper()
	for i := 0; i < 200; i++ {
		if b, err := os.ReadFile(filepath.Join(root, "started.txt")); err == nil {
			return string(b)
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatal("the game was not started")
	return ""
}

func addMission(t *testing.T, root, rel string, data []byte) string {
	t.Helper()
	path := filepath.Join(root, rel)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestFindMissions(t *testing.T) {
	root := t.TempDir()
	image := config()
	addMission(t, root, `data\oprum\shared\missions\b.mispack`, pack(image, "Beta\r\nLine one\r\nline two.\r\n\r\n#brief1 not shown"))
	addMission(t, root, `data\common\ID-1\shared\missions\a.mispack`, pack(image, "alpha\r\n#info at once"))
	addMission(t, root, `data\oprum\shared\missions\broken.mispack`, []byte("AZP3"))
	addMission(t, root, `data\oprum\shared\missions\notitle.mispack`, pack(image, ""))
	got := findMissions(root)
	if len(got) != 3 {
		t.Fatalf("%d missions: %+v", len(got), got)
	}
	if got[0].Title != "alpha" || got[0].Brief != "" || got[1].Title != "Beta" || got[1].Brief != "Line one line two." || got[2].Title != "notitle" {
		t.Fatalf("%+v", got)
	}
	if sideName(got[0].UserSide) != "Germans" || sideName(got[0].EnemySide) != "Soviets" {
		t.Fatalf("sides %x %x", got[0].UserSide, got[0].EnemySide)
	}
}

func TestBadPacks(t *testing.T) {
	bad := pack(config(), "x")
	binary.LittleEndian.PutUint64(bad[0xc0+0x80+8:], 1<<62) // entry beyond the end of the file
	if findEntry(bad, "tacgame_image", "tacimage") != nil {
		t.Fatal("entry outside the file accepted")
	}
	if _, _, err := battleSides([]byte("AZCF" + strings.Repeat("\xff", 0x40))); err == nil {
		t.Fatal("broken description accepted")
	}
}

func TestHostMission(t *testing.T) {
	if !steamRunning() {
		t.Skip("needs Steam running")
	}
	root := fakeGame(t)
	image := config()
	path := addMission(t, root, `data\oprum\shared\missions\m.mispack`, pack(image, "M"))
	m, err := loadMission(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("FAKE_GAME", "1")
	t.Setenv("MP_PEER", "left over")
	t.Setenv("MP_AUTOJOIN", "left over")
	if err := hostMission(root, m, true); err != nil {
		t.Fatal(err)
	}
	out := started(t, root)
	line, _, _ := strings.Cut(out, "\n")
	want := `"` + root + `\starter_x64.exe" root\programs\tacgame.progpack, data\common\shared\config\tacgame_image.cfgpack, users\profiles, mine, *:*, 0, 1`
	if strings.TrimSpace(line) != want {
		t.Fatalf("command line\n%s\nwant\n%s", line, want)
	}
	for _, s := range []string{"MP_ROLE=host\n", "MP_HOST_MODE=mission\n", "MP_SWAP=1\n", "\n" + root + "\n"} {
		if !strings.Contains(out, s) {
			t.Errorf("missing %q", s)
		}
	}
	if strings.Contains(out, "MP_PEER") || strings.Contains(out, "MP_AUTOJOIN") {
		t.Error("an old session's setting reached the game")
	}
	written, _ := os.ReadFile(filepath.Join(root, imagePath))
	if !bytes.Equal(written, image) {
		t.Error("the mission's description was not written")
	}
}

func TestJoinGame(t *testing.T) {
	if !steamRunning() {
		t.Skip("needs Steam running")
	}
	root := fakeGame(t)
	t.Setenv("FAKE_GAME", "1")
	t.Setenv("MP_ROLE", "host")
	if err := joinGame(root, " 203.0.113.7:1943 "); err != nil {
		t.Fatal(err)
	}
	out := started(t, root)
	line, _, _ := strings.Cut(out, "\n")
	if strings.TrimSpace(line) != `"`+root+`\starter_x64.exe"` {
		t.Fatalf("command line %s", line)
	}
	if !strings.Contains(out, "MP_AUTOJOIN=203.0.113.7:1943\n") || strings.Contains(out, "MP_ROLE") {
		t.Fatalf("environment:\n%s", out)
	}
	if lastHost(root) != "203.0.113.7:1943" {
		t.Fatalf("last host %q", lastHost(root))
	}
}

func TestChecks(t *testing.T) {
	root := fakeGame(t)
	if err := joinGame(root, ""); err == nil {
		t.Error("empty address accepted")
	}
	if err := joinGame(root, strings.Repeat("a", 64)); err == nil {
		t.Error("an address too long for the game's field accepted")
	}
	if err := joinGame(root, "a b"); err == nil {
		t.Error("address with a space accepted")
	}
	os.Remove(filepath.Join(root, `bin\rel\x64\basic_network.codelib`))
	if err := checkReady(root); err == nil || !strings.Contains(err.Error(), "not installed") {
		t.Errorf("mod missing: %v", err)
	}
	if _, err := gameFolder(t.TempDir()); err == nil {
		t.Error("a folder without the game accepted")
	}
	if id := profileID(t.TempDir()); id != "cloud" {
		t.Errorf("default profile %q", id)
	}
}

// Every mission of the installed game parses, and its description is what the game itself would start from.
func TestInstalledGame(t *testing.T) {
	root := `C:\Program Files (x86)\Steam\steamapps\common\Graviteam Tactics Mius-Front`
	if _, err := os.Stat(filepath.Join(root, "starter_x64.exe")); err != nil {
		t.Skip("game not installed here")
	}
	all, _ := filepath.Glob(root + `\data\*\shared\missions\*.mispack`)
	more, _ := filepath.Glob(root + `\data\common\*\shared\missions\*.mispack`)
	got := findMissions(root)
	if len(got) != len(all)+len(more) {
		t.Fatalf("%d of %d missions", len(got), len(all)+len(more))
	}
	for _, m := range got {
		t.Logf("%-28s %s vs %s  %.50s", m.Title, sideName(m.UserSide), sideName(m.EnemySide), m.Brief)
	}
}

func gameFiles(t *testing.T, root string) []string {
	t.Helper()
	var out []string
	filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			rel, _ := filepath.Rel(root, path)
			if !strings.HasPrefix(rel, "mpmod1") {
				out = append(out, rel)
			}
		}
		return nil
	})
	return out
}

func noGameRunning(t *testing.T) {
	t.Helper()
	out, _ := exec.Command("tasklist", "/fi", "imagename eq starter_x64.exe", "/nh").Output()
	if bytes.Contains(bytes.ToLower(out), []byte("starter_x64.exe")) {
		t.Skip("the game is running: Enable_Multiplayer.bat refuses to work")
	}
}

// A game folder with nothing of the multiplayer, in a path like the usual Steam one.
func cleanGame(t *testing.T) string {
	t.Helper()
	root := filepath.Join(t.TempDir(), "Program Files (x86)", gameFolderName)
	if err := os.MkdirAll(filepath.Join(root, `bin\rel\x64`), 0o755); err != nil {
		t.Fatal(err)
	}
	os.MkdirAll(filepath.Join(root, "users"), 0o755)
	for _, f := range []string{"starter_x64.exe", `bin\rel\x64\adv_game.codelib`} {
		if err := os.WriteFile(filepath.Join(root, f), []byte("stock"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

func state(t *testing.T, root string) modState {
	t.Helper()
	s, err := installedState(root)
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func TestInstallAndRemove(t *testing.T) {
	noGameRunning(t)
	root := cleanGame(t)
	before := gameFiles(t, root)
	if state(t, root) != modMissing {
		t.Fatal("an empty game counts as installed")
	}
	// the stand-in game is not build 3598: refused until forced, and nothing in the game has changed
	if err := installMod(root, false); !errors.Is(err, errOtherBuild) {
		t.Fatalf("other build: %v", err)
	}
	if got := gameFiles(t, root); len(got) != len(before) {
		t.Fatalf("files added by a refused install: %v", got)
	}
	if err := installMod(root, true); err != nil {
		t.Fatal(err)
	}
	if state(t, root) != modCurrent {
		t.Fatal("not current after installing")
	}
	self, _ := os.ReadFile(os.Args[0])
	if copied, _ := os.ReadFile(filepath.Join(root, launcherName)); !bytes.Equal(self, copied) {
		t.Error("the launcher did not copy itself into the game")
	}
	if err := checkInstalledFiles(root); err != nil {
		t.Error(err)
	}

	// another version in the game is noticed and replaced; the game's own steam_appid.txt is no reason to
	os.WriteFile(filepath.Join(root, "steam_appid.txt"), []byte("mine"), 0o644)
	if state(t, root) != modCurrent {
		t.Error("a game's own steam_appid.txt counts as another version")
	}
	os.WriteFile(filepath.Join(root, `bin\rel\x64\basic_network.codelib`), []byte("old"), 0o644)
	if state(t, root) != modOutdated {
		t.Fatal("another module not noticed")
	}
	if err := installMod(root, true); err != nil {
		t.Fatal(err)
	}
	if state(t, root) != modCurrent {
		t.Fatal("not replaced")
	}

	code, output, err := runBat(root, "Disable_Multiplayer.bat", "/quiet")
	if err != nil || code != 0 {
		t.Fatalf("disable: %d %v\n%s", code, err, output)
	}
	for _, f := range gameFiles(t, root) {
		if strings.Contains(f, "basic_network") || strings.Contains(f, "progpack") || f == launcherName || f == "mp_join.exe" {
			t.Errorf("left in the game: %s", f)
		}
	}
}

// The game's own steam_appid.txt stays when the multiplayer comes and goes.
func TestOwnAppID(t *testing.T) {
	noGameRunning(t)
	root := cleanGame(t)
	own := filepath.Join(root, "steam_appid.txt")
	os.WriteFile(own, []byte("mine"), 0o644)
	if err := installMod(root, true); err != nil {
		t.Fatal(err)
	}
	if code, output, err := runBat(root, "Disable_Multiplayer.bat", "/quiet"); err != nil || code != 0 {
		t.Fatalf("disable: %d %v\n%s", code, err, output)
	}
	if b, _ := os.ReadFile(own); string(b) != "mine" {
		t.Errorf("steam_appid.txt is now %q", b)
	}
}

func checkInstalledFiles(root string) error {
	for _, f := range []string{`bin\rel\x64\basic_network.codelib`, `root\programs\tacgame.progpack`, `root\programs\opgame.progpack`, "steam_appid.txt", "mp_join.exe"} {
		if _, err := os.Stat(filepath.Join(root, f)); err != nil {
			return err
		}
	}
	return nil
}

func TestSteamLibraries(t *testing.T) {
	vdf := "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\Program Files (x86)\\Steam\"\n\t\t\"label\"\t\t\"\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\Games\\SteamLibrary\"\n\t}\n}\n"
	got := steamLibraries(vdf)
	if len(got) != 2 || got[0] != `C:\Program Files (x86)\Steam` || got[1] != `D:\Games\SteamLibrary` {
		t.Fatalf("%q", got)
	}
}

func TestFindGame(t *testing.T) {
	if root := findInSteam(); root != "" && !isGameFolder(root) {
		t.Fatalf("%q is no game folder", root)
	}
	if _, err := gameFolder(t.TempDir()); err == nil {
		t.Fatal("a folder without the game accepted")
	}
}

// The real stock files pass Enable_Multiplayer.bat's build check. Only on the Steam branch the package was made for.
func TestStockBuild(t *testing.T) {
	game := `C:\Program Files (x86)\Steam\steamapps\common\` + gameFolderName
	entries, err := packageFiles()
	if err != nil {
		t.Fatal(err)
	}
	var list *zip.File
	for _, e := range entries {
		if e.Name == "mpmod1/game_build_3598.sha256" {
			list = e
		}
	}
	if list == nil {
		t.Fatal("no hash list in the package")
	}
	in, err := list.Open()
	if err != nil {
		t.Fatal(err)
	}
	defer in.Close()
	text, _ := io.ReadAll(in)
	noGameRunning(t)
	root := cleanGame(t)
	lines := strings.Fields(string(text))
	if len(lines) != 40 {
		t.Fatalf("%d fields in the hash list", len(lines))
	}
	for i := 1; i < len(lines); i += 2 {
		data, err := os.ReadFile(filepath.Join(game, lines[i]))
		if err != nil {
			t.Skip("game not installed here")
		}
		path := filepath.Join(root, lines[i])
		os.MkdirAll(filepath.Dir(path), 0o755)
		if err := os.WriteFile(path, data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if sum := sha256.Sum256(mustRead(t, filepath.Join(root, "starter_x64.exe"))); !strings.EqualFold(hex.EncodeToString(sum[:]), hashOf(lines, "starter_x64.exe")) {
		t.Skip("this game is on another build")
	}
	if err := installMod(root, false); err != nil {
		t.Fatalf("stock files refused: %v", err)
	}
	if state(t, root) != modCurrent {
		t.Fatal("not installed")
	}
}

func mustRead(t *testing.T, path string) []byte {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func hashOf(fields []string, file string) string {
	for i := 1; i < len(fields); i += 2 {
		if fields[i] == file {
			return fields[i-1]
		}
	}
	return ""
}
