package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// makeProject creates a temp dir, switches into it, and returns it. Tests run
// the commands against the working directory, so each test is isolated.
func makeProject(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	t.Chdir(dir)
	return dir
}

// makeDepDir writes a minimal dependency package (lib.aev + aevix.toml).
func makeDepDir(t *testing.T, root, name, version string) string {
	t.Helper()
	dir := filepath.Join(root, name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "lib.aev"), []byte("func f(): int { return 1; }\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	manifest := "[package]\nname = \"" + name + "\"\nversion = \"" + version + "\"\n"
	if err := os.WriteFile(filepath.Join(dir, "aevix.toml"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestInitCreatesManifestAndLock(t *testing.T) {
	dir := makeProject(t)
	if err := cmdInit([]string{"app"}); err != nil {
		t.Fatalf("init: %v", err)
	}
	if !fileExists(filepath.Join(dir, manifestName)) {
		t.Fatal("init did not write aevix.toml")
	}
	lock, err := readLock(dir)
	if err != nil {
		t.Fatalf("readLock: %v", err)
	}
	if lock.Name != "app" || lock.Version != "0.1.0" {
		t.Fatalf("lock header = %+v, want app/0.1.0", lock)
	}
	if len(lock.Dependencies) != 0 {
		t.Fatalf("fresh lock has %d deps, want 0", len(lock.Dependencies))
	}
}

func TestInitTwiceFails(t *testing.T) {
	makeProject(t)
	if err := cmdInit([]string{"app"}); err != nil {
		t.Fatalf("first init: %v", err)
	}
	if err := cmdInit([]string{"app"}); err == nil {
		t.Fatal("second init should fail")
	}
}

func TestManifestRoundtrip(t *testing.T) {
	dir := makeProject(t)
	orig := &Manifest{
		Name:    "app",
		Version: "1.0.0",
		Deps: map[string]Dep{
			"calc": {Path: "calc", Version: "1.2.3"},
			"util": {Path: "../util"},
		},
	}
	if err := writeManifest(dir, orig); err != nil {
		t.Fatalf("writeManifest: %v", err)
	}
	got, err := readManifest(dir)
	if err != nil {
		t.Fatalf("readManifest: %v", err)
	}
	if got.Name != orig.Name || got.Version != orig.Version {
		t.Fatalf("header = %s/%s, want %s/%s", got.Name, got.Version, orig.Name, orig.Version)
	}
	if got.Deps["calc"].Path != "calc" || got.Deps["calc"].Version != "1.2.3" {
		t.Fatalf("calc = %+v", got.Deps["calc"])
	}
	if got.Deps["util"].Path != "../util" || got.Deps["util"].Version != "" {
		t.Fatalf("util = %+v", got.Deps["util"])
	}
}

func TestAddDetectsVersionAndWritesLock(t *testing.T) {
	dir := makeProject(t)
	if err := cmdInit([]string{"app"}); err != nil {
		t.Fatal(err)
	}
	makeDepDir(t, dir, "calc", "1.2.3")
	if err := cmdAdd([]string{"calc", "--path", "./calc"}); err != nil {
		t.Fatalf("add: %v", err)
	}

	m, err := readManifest(dir)
	if err != nil {
		t.Fatal(err)
	}
	if m.Deps["calc"].Version != "1.2.3" {
		t.Fatalf("manifest dep version = %q, want 1.2.3 from dep manifest", m.Deps["calc"].Version)
	}

	lock, err := readLock(dir)
	if err != nil {
		t.Fatal(err)
	}
	ld, ok := lock.Dependencies["calc"]
	if !ok {
		t.Fatal("lock missing calc")
	}
	if ld.Version != "1.2.3" || ld.Path != "calc" {
		t.Fatalf("lock calc = %+v, want version 1.2.3 path calc", ld)
	}
}

func TestAddExplicitVersionOverridesDetection(t *testing.T) {
	dir := makeProject(t)
	cmdInit([]string{"app"})
	makeDepDir(t, dir, "calc", "1.2.3")
	if err := cmdAdd([]string{"calc", "--path", "./calc", "--version", "9.9.9"}); err != nil {
		t.Fatal(err)
	}
	lock, _ := readLock(dir)
	if lock.Dependencies["calc"].Version != "9.9.9" {
		t.Fatalf("explicit version = %q, want 9.9.9", lock.Dependencies["calc"].Version)
	}
}

func TestAddMissingPathFails(t *testing.T) {
	makeProject(t)
	cmdInit([]string{"app"})
	if err := cmdAdd([]string{"calc"}); err == nil {
		t.Fatal("add without --path should fail")
	}
}

func TestAddMissingDirFails(t *testing.T) {
	makeProject(t)
	cmdInit([]string{"app"})
	if err := cmdAdd([]string{"calc", "--path", "./nope"}); err == nil {
		t.Fatal("add of a missing directory should fail")
	}
}

func TestAddDirWithoutAevFails(t *testing.T) {
	dir := makeProject(t)
	cmdInit([]string{"app"})
	empty := filepath.Join(dir, "empty")
	os.MkdirAll(empty, 0o755)
	if err := cmdAdd([]string{"calc", "--path", "./empty"}); err == nil {
		t.Fatal("add of a directory with no .aev source should fail")
	}
}

func TestRemoveDeletesDependency(t *testing.T) {
	dir := makeProject(t)
	cmdInit([]string{"app"})
	makeDepDir(t, dir, "calc", "1.0.0")
	cmdAdd([]string{"calc", "--path", "./calc"})

	if err := cmdRemove([]string{"calc"}); err != nil {
		t.Fatalf("remove: %v", err)
	}
	lock, _ := readLock(dir)
	if _, ok := lock.Dependencies["calc"]; ok {
		t.Fatal("lock still lists calc after remove")
	}
	m, _ := readManifest(dir)
	if _, ok := m.Deps["calc"]; ok {
		t.Fatal("manifest still lists calc after remove")
	}
}

func TestRemoveUndeclaredFails(t *testing.T) {
	makeProject(t)
	cmdInit([]string{"app"})
	if err := cmdRemove([]string{"nope"}); err == nil {
		t.Fatal("remove of an undeclared dependency should fail")
	}
}

func TestLockIsDeterministicJSON(t *testing.T) {
	dir := makeProject(t)
	cmdInit([]string{"app"})
	makeDepDir(t, dir, "b", "1.0.0")
	makeDepDir(t, dir, "a", "2.0.0")
	cmdAdd([]string{"b", "--path", "./b"})
	cmdAdd([]string{"a", "--path", "./a"})

	first, err := os.ReadFile(filepath.Join(dir, lockName))
	if err != nil {
		t.Fatal(err)
	}
	// Recompute from scratch; the bytes must not change.
	m, _ := readManifest(dir)
	if err := recomputeLock(dir, m); err != nil {
		t.Fatal(err)
	}
	second, _ := os.ReadFile(filepath.Join(dir, lockName))
	if string(first) != string(second) {
		t.Fatalf("lock is not deterministic:\nfirst:\n%s\nsecond:\n%s", first, second)
	}
	// Keys must be sorted so diffs stay minimal.
	var probe map[string]json.RawMessage
	json.Unmarshal(first, &probe)
	deps := probe["dependencies"]
	var byName map[string]json.RawMessage
	json.Unmarshal(deps, &byName)
	if _, ok := byName["a"]; !ok {
		t.Fatal("lock dependencies missing 'a'")
	}
}

func TestHelpers(t *testing.T) {
	if v := unquote(`"hi"`); v != "hi" {
		t.Fatalf("unquote = %q", v)
	}
	if b, a, ok := cut("k = v", "="); !ok || b != "k " || a != " v" {
		t.Fatalf("cut = %q,%q,%v", b, a, ok)
	}
	parts := splitTop(`x = "a,b", y = "c"`, ',')
	if len(parts) != 2 {
		t.Fatalf("splitTop = %#v, want 2 parts (comma inside quotes kept)", parts)
	}
	if parts[0] != `x = "a,b"` || parts[1] != ` y = "c"` {
		t.Fatalf("splitTop parts = %#v", parts)
	}
}
