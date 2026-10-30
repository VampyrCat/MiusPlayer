// mp_launcher.exe: a small window for starting a multiplayer game without going through the game's menus.
// Host: pick a mission, the game starts straight into its battle. Join: type the host's address, the game
// connects by itself. It can be started from anywhere: it finds the game (mp_launcher -game <folder> names
// another copy), and if the game does not have this multiplayer yet it offers to install it (install.go).
//
// Rebuild rsrc_windows_amd64.syso (the manifest: visual styles, which walk needs, and high DPI) after a change to
// mp_launcher.manifest:  go run github.com/akavel/rsrc@v0.10.2 -manifest mp_launcher.manifest -o rsrc_windows_amd64.syso
package main

import (
	"errors"
	"flag"
	"fmt"

	"github.com/tailscale/walk"
	. "github.com/tailscale/walk/declarative"
)

const title = "Mius-Front Multiplayer"

type window struct {
	*walk.MainWindow
	root     string
	missions []Mission
	list     *walk.ListBox
	brief    *walk.TextEdit
	sides    *walk.Label
	swap     *walk.CheckBox
	address  *walk.LineEdit
}

func (w *window) selected() *Mission {
	if i := w.list.CurrentIndex(); i >= 0 && i < len(w.missions) {
		return &w.missions[i]
	}
	return nil
}

func (w *window) showMission() {
	m := w.selected()
	if m == nil {
		return
	}
	you, other := m.UserSide, m.EnemySide
	if w.swap.Checked() {
		you, other = other, you
	}
	w.brief.SetText(m.Brief)
	w.sides.SetText(fmt.Sprintf("You command the %s, the other player the %s.", sideName(you), sideName(other)))
}

func (w *window) alert(text string) {
	walk.MsgBox(w, title, text, walk.MsgBoxIconError)
}

// finish closes the window once the game is on its way, or says what went wrong.
func (w *window) finish(err error) {
	if err != nil {
		w.alert(err.Error())
		return
	}
	w.Close()
}

func (w *window) host() {
	m := w.selected()
	if m == nil {
		w.alert("Pick a mission first.")
		return
	}
	w.finish(hostMission(w.root, *m, w.swap.Checked()))
}

func (w *window) join() { w.finish(joinGame(w.root, w.address.Text())) }

// askGameFolder is for a game that is not where it can be found; "" when the player gives up.
func askGameFolder() string {
	walk.MsgBox(nil, title, "The game was not found. Please choose the game folder, the one with starter_x64.exe.", walk.MsgBoxIconInformation)
	for {
		dialog := walk.FileDialog{Title: "Game folder"}
		if ok, err := dialog.ShowBrowseFolder(nil); err != nil || !ok {
			return ""
		}
		if isGameFolder(dialog.FilePath) {
			return dialog.FilePath
		}
		walk.MsgBox(nil, title, "That folder has no starter_x64.exe.", walk.MsgBoxIconError)
	}
}

func confirm(text string) bool {
	return walk.MsgBox(nil, title, text, walk.MsgBoxYesNo|walk.MsgBoxIconQuestion) == walk.DlgCmdYes
}

// ensureInstalled offers to install the multiplayer when the game does not have this version of it.
func ensureInstalled(root string) {
	state, err := installedState(root)
	if err != nil {
		walk.MsgBox(nil, title, err.Error(), walk.MsgBoxIconError)
		return
	}
	switch state {
	case modCurrent:
		return
	case modMissing:
		if !confirm("Multiplayer is not installed in\n" + root + "\n\nInstall it now?") {
			return
		}
	case modOutdated:
		if !confirm("The game has another version of the multiplayer in\n" + root + "\nBoth players need the same one.\n\nReplace it with the version of this program?") {
			return
		}
	}
	err = installMod(root, false)
	if errors.Is(err, errOtherBuild) && confirm("This game is not on the Steam branch public_beta (version 6.00, build 3598). The multiplayer was made for that build only and will most likely not work with this one.\n\nTo switch: in Steam, right-click the game, Properties, Betas, public_beta. Then start this program again.\n\nInstall it anyway?") {
		err = installMod(root, true)
	}
	switch {
	case errors.Is(err, errOtherBuild):
	case err != nil:
		walk.MsgBox(nil, title, err.Error(), walk.MsgBoxIconError)
	default:
		walk.MsgBox(nil, title, "Multiplayer is installed.\nmpmod1\\Disable_Multiplayer.bat in the game folder takes it out again.", walk.MsgBoxIconInformation)
	}
}

func main() {
	game := flag.String("game", "", "game folder (default: find it)")
	flag.Parse()
	app, err := walk.InitApp()
	if err != nil {
		return
	}

	root, err := gameFolder(*game)
	if err == nil && root == "" {
		if root = askGameFolder(); root == "" {
			return
		}
	}
	if err != nil {
		walk.MsgBox(nil, title, err.Error(), walk.MsgBoxIconError)
		return
	}
	ensureInstalled(root)
	w := &window{root: root, missions: findMissions(root)}
	titles := make([]string, len(w.missions))
	for i, m := range w.missions {
		titles[i] = m.Title
	}
	err = MainWindow{
		AssignTo: &w.MainWindow,
		Title:    title,
		Size:     Size{Width: 480, Height: 600},
		MinSize:  Size{Width: 400, Height: 500},
		Layout:   VBox{},
		Children: []Widget{
			GroupBox{
				Title:  "Host a mission",
				Layout: VBox{},
				Children: []Widget{
					ListBox{AssignTo: &w.list, Model: titles, OnCurrentIndexChanged: w.showMission, OnItemActivated: w.host},
					TextEdit{AssignTo: &w.brief, ReadOnly: true, VScroll: true, MinSize: Size{Height: 60}, MaxSize: Size{Height: 60}},
					Label{AssignTo: &w.sides},
					CheckBox{AssignTo: &w.swap, Text: "Swap sides", Alignment: AlignHNearVCenter, OnCheckedChanged: w.showMission},
					PushButton{Text: "Host", OnClicked: w.host},
				},
			},
			GroupBox{
				Title:  "Join a game",
				Layout: VBox{},
				Children: []Widget{
					Label{Text: "Host address (port 1943 unless you add :port)"},
					LineEdit{AssignTo: &w.address, Text: lastHost(root), MaxLength: 63, OnKeyPress: func(key walk.Key) {
						if key == walk.KeyReturn {
							w.join()
						}
					}},
					PushButton{Text: "Join", OnClicked: w.join},
				},
			},
		},
	}.Create()
	if err != nil {
		walk.MsgBox(nil, title, err.Error(), walk.MsgBoxIconError)
		return
	}
	w.ToolBar().SetVisible(false) // an empty tool bar would sit on top of the tabs
	if len(w.missions) > 0 {
		w.list.SetCurrentIndex(0)
	}
	app.Run()
}
