package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// A mission pack (.mispack) is an AZP3 pack:
//
//	header:  'AZP3', entry count; entries of 0x80 bytes from 0xc0
//	entry:   +0x08 offset, +0x10 size, +0x20 name[0x20], +0x40 kind[0x20]
//
// Its "tacgame_image" entry is the finished battle description, the same file the game's Missions screen
// hands to the battle program; "desc" (kind "text") starts with the mission's title and summary. The
// description is the engine's binary config format, see basic_network\image.h.

const (
	packEntries = 0xc0
	packEntry   = 0x80
	configNode  = 0x80
	configField = 0x40
	sideGermans = 0x00524547
	sideSoviets = 0x52535355
)

// Mission is one mission pack of the game's data folder.
type Mission struct {
	Path, Title, Brief  string
	UserSide, EnemySide uint32 // side codes of the force the mission gives the player and of its enemy
}

func cString(b []byte) string {
	if i := bytes.IndexByte(b, 0); i >= 0 {
		b = b[:i]
	}
	return string(b)
}

// findEntry returns the bytes of an entry, or nil.
func findEntry(pack []byte, name, kind string) []byte {
	if len(pack) < packEntries || string(pack[:4]) != "AZP3" {
		return nil
	}
	count := int(binary.LittleEndian.Uint32(pack[4:]))
	for i := 0; i < count; i++ {
		o := packEntries + i*packEntry
		if o+packEntry > len(pack) {
			break
		}
		if cString(pack[o+0x20:o+0x40]) != name || cString(pack[o+0x40:o+0x60]) != kind {
			continue
		}
		off, size := binary.LittleEndian.Uint64(pack[o+8:]), binary.LittleEndian.Uint64(pack[o+0x10:])
		if off > uint64(len(pack)) || size > uint64(len(pack))-off {
			return nil
		}
		return pack[off : off+size]
	}
	return nil
}

// battleSides reads i_gamespec:gamespec user_side / enemy_side from a battle description.
func battleSides(image []byte) (user, enemy uint32, err error) {
	bad := errors.New("not a battle description")
	if len(image) < 0x40 || string(image[:4]) != "AZCF" {
		return 0, 0, bad
	}
	count, table := binary.LittleEndian.Uint32(image[4:]), uint64(binary.LittleEndian.Uint32(image[8:]))
	for i := uint64(0); i < uint64(count); i++ {
		o := table + i*configNode
		if o+configNode > uint64(len(image)) {
			break
		}
		if cString(image[o:o+0x20]) != "i_gamespec:gamespec" {
			continue
		}
		rows, data := uint64(binary.LittleEndian.Uint32(image[o+0x28:])), uint64(binary.LittleEndian.Uint32(image[o+0x30:]))
		if data+rows*configField > uint64(len(image)) {
			return 0, 0, bad
		}
		var haveUser, haveEnemy bool
		for r := uint64(0); r < rows; r++ {
			f := data + r*configField
			switch cString(image[f+0x20 : f+0x40]) {
			case "user_side":
				user, haveUser = binary.LittleEndian.Uint32(image[f:]), true
			case "enemy_side":
				enemy, haveEnemy = binary.LittleEndian.Uint32(image[f:]), true
			}
		}
		if haveUser && haveEnemy {
			return user, enemy, nil
		}
		return 0, 0, bad
	}
	return 0, 0, bad
}

// describe splits a mission's text into title and summary. What follows the summary (a blank line or the
// "#key text" lines) is shown by the game during the battle.
func describe(text []byte) (title, brief string) {
	lines := strings.Split(strings.ReplaceAll(strings.ToValidUTF8(string(text), "?"), "\r\n", "\n"), "\n")
	title = strings.TrimSpace(lines[0])
	var summary []string
	for _, line := range lines[1:] {
		if line = strings.TrimSpace(line); line == "" || line[0] == '#' {
			break
		}
		summary = append(summary, line)
	}
	return title, strings.Join(summary, " ")
}

func loadMission(path string) (Mission, error) {
	pack, err := os.ReadFile(path)
	if err != nil {
		return Mission{}, err
	}
	image := findEntry(pack, "tacgame_image", "tacimage")
	if image == nil {
		return Mission{}, fmt.Errorf("%s has no battle description", filepath.Base(path))
	}
	m := Mission{Path: path}
	if m.UserSide, m.EnemySide, err = battleSides(image); err != nil {
		return Mission{}, fmt.Errorf("%s: %w", filepath.Base(path), err)
	}
	m.Title, m.Brief = describe(findEntry(pack, "desc", "text"))
	if m.Title == "" {
		m.Title = strings.TrimSuffix(filepath.Base(path), ".mispack")
	}
	return m, nil
}

// missionImage returns the battle description of a mission pack.
func missionImage(path string) ([]byte, error) {
	pack, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	image := findEntry(pack, "tacgame_image", "tacimage")
	if image == nil {
		return nil, fmt.Errorf("%s has no battle description", filepath.Base(path))
	}
	return image, nil
}

// findMissions lists the missions of the game. They are files <pack>\shared\missions\*.mispack, where <pack>
// is a folder up to two levels below data\ (data\oprum, data\common\<id>, ...). A pack that cannot be read
// is left out.
func findMissions(root string) []Mission {
	var out []Mission
	var visit func(dir string, depth int)
	visit = func(dir string, depth int) {
		files, _ := filepath.Glob(filepath.Join(dir, "shared", "missions", "*.mispack"))
		for _, f := range files {
			if m, err := loadMission(f); err == nil {
				out = append(out, m)
			}
		}
		if depth == 0 {
			return
		}
		dirs, _ := os.ReadDir(dir)
		for _, d := range dirs {
			if d.IsDir() && d.Name() != "shared" {
				visit(filepath.Join(dir, d.Name()), depth-1)
			}
		}
	}
	visit(filepath.Join(root, "data"), 2)
	sort.Slice(out, func(a, b int) bool { return strings.ToLower(out[a].Title) < strings.ToLower(out[b].Title) })
	return out
}

func sideName(code uint32) string {
	switch code {
	case sideGermans:
		return "Germans"
	case sideSoviets:
		return "Soviets"
	}
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], code)
	return cString(b[:])
}
