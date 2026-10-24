// ============================================================================
// aevix-pm: the Aevix package manager.
//
// This is the "smart" half of the package-management contract. The compiler
// (both frontends) is deliberately dumb and read-only: it consumes aevix.lock
// to resolve `pkg:module` imports. aevix-pm is the only writer of aevix.toml
// (human intent) and aevix.lock (pinned, resolved graph).
//
// The whole tool is self-contained on purpose. It depends only on the Go
// standard library and never imports compiler code, so it can be lifted out
// into a standalone repository without touching the compiler. See
// notes/package-manager.md for the full contract.
// ============================================================================
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Manifest mirrors aevix.toml: the declared intent a human edits. It is kept
// minimal on purpose — a package name, a version, and a set of dependencies.
type Manifest struct {
	Name    string
	Version string
	Deps    map[string]Dep
}

// Dep is one declared dependency. P0 supports local path dependencies only;
// version-based registry fetching lands with the resolver in a later slice.
type Dep struct {
	Path    string // directory of the package, relative to the manifest
	Version string // optional; informational for path deps
}

// Lock mirrors aevix.lock: the resolved, reproducible graph the compiler reads.
// It is JSON to match the format both frontends already parse today.
type Lock struct {
	Name         string            `json:"name"`
	Version      string            `json:"version"`
	Dependencies map[string]LockDep `json:"dependencies"`
}

// LockDep is one pinned dependency in the lock. Only one of Path/Version is
// meaningful: a path dep points at a working tree, a version dep at the cache.
type LockDep struct {
	Version string `json:"version,omitempty"`
	Path    string `json:"path,omitempty"`
}

const manifestName = "aevix.toml"
const lockName = "aevix.lock"

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	switch os.Args[1] {
	case "init":
		run(cmdInit(os.Args[2:]))
	case "add":
		run(cmdAdd(os.Args[2:]))
	case "remove", "rm":
		run(cmdRemove(os.Args[2:]))
	case "list":
		run(cmdList(os.Args[2:]))
	case "vendor":
		run(cmdVendor(os.Args[2:]))
	case "update":
		run(cmdUpdate(os.Args[2:]))
	case "tree":
		run(cmdTree(os.Args[2:]))
	case "why":
		run(cmdWhy(os.Args[2:]))
	default:
		fmt.Fprintf(os.Stderr, "aevix-pm: unknown command %q\n\n", os.Args[1])
		usage()
		os.Exit(2)
	}
}

// run prints a command error and exits non-zero; nil is a clean success.
func run(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, "aevix-pm:", err)
		os.Exit(1)
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, "Usage: aevix-pm <command> [arguments]")
	fmt.Fprintln(os.Stderr)
	fmt.Fprintln(os.Stderr, "Commands:")
	fmt.Fprintln(os.Stderr, "  init [name]              Create aevix.toml and an empty aevix.lock")
	fmt.Fprintln(os.Stderr, "  add <name> --path <dir>  Add a local path dependency and update the lock")
	fmt.Fprintln(os.Stderr, "  remove <name>            Remove a dependency and update the lock")
	fmt.Fprintln(os.Stderr, "  update [name]            Re-resolve deps to match the manifest")
	fmt.Fprintln(os.Stderr, "  list                     List declared dependencies")
	fmt.Fprintln(os.Stderr, "  vendor                   Copy dependencies into lib/")
	fmt.Fprintln(os.Stderr, "  tree                     Print the resolved dependency graph")
	fmt.Fprintln(os.Stderr, "  why <name>               Explain why a package is in the graph")
}

// findProjectRoot walks up from the current directory looking for a manifest or
// a lock, mirroring the compiler's own project-root detection so both tools
// agree on where a project begins.
func findProjectRoot() (string, error) {
	d, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for {
		if fileExists(filepath.Join(d, manifestName)) || fileExists(filepath.Join(d, lockName)) {
			return d, nil
		}
		parent := filepath.Dir(d)
		if parent == d {
			return "", fmt.Errorf("not inside an aevix project (no %s found)", manifestName)
		}
		d = parent
	}
}

func fileExists(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

// writeManifest emits aevix.toml in a fixed, canonical layout so repeated runs
// produce byte-identical output (reviewable diffs, no churn).
func writeManifest(root string, m *Manifest) error {
	var b strings.Builder
	b.WriteString("[package]\n")
	b.WriteString(fmt.Sprintf("name = %s\n", quote(m.Name)))
	b.WriteString(fmt.Sprintf("version = %s\n", quote(m.Version)))
	b.WriteString("\n[dependencies]\n")
	for _, name := range sortedKeys(m.Deps) {
		d := m.Deps[name]
		parts := []string{fmt.Sprintf("path = %s", quote(d.Path))}
		if d.Version != "" {
			parts = append(parts, fmt.Sprintf("version = %s", quote(d.Version)))
		}
		b.WriteString(fmt.Sprintf("%s = { %s }\n", name, strings.Join(parts, ", ")))
	}
	return os.WriteFile(filepath.Join(root, manifestName), []byte(b.String()), 0o644)
}

// writeLock emits aevix.lock as indented JSON with sorted dependency keys, so
// the compiler sees a stable file and diffs stay minimal.
func writeLock(root string, l *Lock) error {
	if l.Dependencies == nil {
		l.Dependencies = map[string]LockDep{}
	}
	data, err := json.MarshalIndent(l, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(root, lockName), append(data, '\n'), 0o644)
}

func readLock(root string) (*Lock, error) {
	data, err := os.ReadFile(filepath.Join(root, lockName))
	if err != nil {
		return nil, err
	}
	var l Lock
	if err := json.Unmarshal(data, &l); err != nil {
		return nil, fmt.Errorf("cannot parse %s: %w", lockName, err)
	}
	if l.Dependencies == nil {
		l.Dependencies = map[string]LockDep{}
	}
	return &l, nil
}

// readManifest parses the small TOML subset writeManifest emits. It is a
// focused line-based reader rather than a general TOML library so the tool
// stays dependency-free and easy to extract.
func readManifest(root string) (*Manifest, error) {
	data, err := os.ReadFile(filepath.Join(root, manifestName))
	if err != nil {
		return nil, err
	}
	m := &Manifest{Deps: map[string]Dep{}}
	section := ""
	for _, raw := range strings.Split(string(data), "\n") {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			section = strings.TrimSpace(line[1 : len(line)-1])
			continue
		}
		key, val, ok := cut(line, "=")
		if !ok {
			continue
		}
		key = strings.TrimSpace(key)
		val = strings.TrimSpace(val)
		switch section {
		case "package":
			switch key {
			case "name":
				m.Name = unquote(val)
			case "version":
				m.Version = unquote(val)
			}
		case "dependencies":
			if dep, ok := parseInlineTable(val); ok {
				m.Deps[key] = dep
			}
		}
	}
	return m, nil
}

// parseInlineTable reads `{ path = "...", version = "..." }` into a Dep.
func parseInlineTable(val string) (Dep, bool) {
	val = strings.TrimSpace(val)
	if !strings.HasPrefix(val, "{") || !strings.HasSuffix(val, "}") {
		return Dep{}, false
	}
	inner := strings.TrimSpace(val[1 : len(val)-1])
	var d Dep
	for _, part := range splitTop(inner, ',') {
		k, v, ok := cut(part, "=")
		if !ok {
			continue
		}
		k = strings.TrimSpace(k)
		v = strings.TrimSpace(v)
		switch k {
		case "path":
			d.Path = unquote(v)
		case "version":
			d.Version = unquote(v)
		}
	}
	if d.Path == "" {
		return Dep{}, false
	}
	return d, true
}

func cmdInit(args []string) error {
	name := ""
	if len(args) > 0 {
		name = args[0]
	}
	root, err := os.Getwd()
	if err != nil {
		return err
	}
	if name == "" {
		name = filepath.Base(root)
	}
	if fileExists(filepath.Join(root, manifestName)) {
		return fmt.Errorf("%s already exists", manifestName)
	}
	m := &Manifest{Name: name, Version: "0.1.0", Deps: map[string]Dep{}}
	if err := writeManifest(root, m); err != nil {
		return err
	}
	l := &Lock{Name: name, Version: "0.1.0", Dependencies: map[string]LockDep{}}
	if err := writeLock(root, l); err != nil {
		return err
	}
	fmt.Printf("Initialized package %q\n", name)
	return nil
}

func cmdAdd(args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: aevix-pm add <name> --path <dir> [--version <v>]")
	}
	name := args[0]
	var path, version string
	for i := 1; i < len(args); i++ {
		switch args[i] {
		case "--path":
			if i+1 >= len(args) {
				return fmt.Errorf("--path requires a directory")
			}
			path = args[i+1]
			i++
		case "--version":
			if i+1 >= len(args) {
				return fmt.Errorf("--version requires a value")
			}
			version = args[i+1]
			if !validRange(version) {
				return fmt.Errorf("invalid version range: %s", version)
			}
			i++
		default:
			return fmt.Errorf("unexpected argument %q", args[i])
		}
	}
	if path == "" {
		return fmt.Errorf("add requires --path <dir> (P0 supports local path dependencies)")
	}

	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	m, err := readManifest(root)
	if err != nil {
		return fmt.Errorf("cannot read %s: %w", manifestName, err)
	}

	abs, err := resolveDepDir(root, path)
	if err != nil {
		return err
	}
	if version == "" {
		version = detectVersion(abs)
	}

	// Store paths relative to the project root so the lock stays relocatable;
	// the compiler joins a relative path against the lock's own directory.
	rel, err := filepath.Rel(root, abs)
	if err != nil {
		rel = abs
	}
	m.Deps[name] = Dep{Path: rel, Version: version}
	if err := writeManifest(root, m); err != nil {
		return err
	}
	if err := recomputeLock(root, m); err != nil {
		return err
	}
	fmt.Printf("Added %s (%s) -> %s\n", name, version, rel)
	return nil
}

// resolveDepDir validates a dependency directory and returns its absolute path.
func resolveDepDir(root, path string) (string, error) {
	abs := path
	if !filepath.IsAbs(abs) {
		abs = filepath.Join(root, path)
	}
	st, err := os.Stat(abs)
	if err != nil || !st.IsDir() {
		return "", fmt.Errorf("dependency directory not found: %s", path)
	}
	if !hasAevSource(abs) {
		return "", fmt.Errorf("no .aev source found in %s", path)
	}
	return filepath.Abs(abs)
}

// hasAevSource reports whether a package directory has an entry point (lib.aev)
// or any top-level .aev file — the minimum for the compiler to import it.
func hasAevSource(dir string) bool {
	if fileExists(filepath.Join(dir, "lib.aev")) {
		return true
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return false
	}
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".aev") {
			return true
		}
	}
	return false
}

// detectVersion reads a dependency's own aevix.toml for its declared version,
// falling back to 0.0.0 so a path dep is always pinned to something.
func detectVersion(dir string) string {
	if m, err := readManifest(dir); err == nil && m.Version != "" {
		return m.Version
	}
	return "0.0.0"
}

// recomputeLock rebuilds aevix.lock from the manifest. Every dependency is
// resolved to a pinned entry, so the lock is always a pure function of the
// manifest plus the paths on disk — reproducible and diff-stable.
func recomputeLock(root string, m *Manifest) error {
	l := &Lock{Name: m.Name, Version: m.Version, Dependencies: map[string]LockDep{}}
	for _, name := range sortedKeys(m.Deps) {
		d := m.Deps[name]
		p := d.Path
		if !filepath.IsAbs(p) {
			p = filepath.Join(root, p)
		}
		v := d.Version
		if v == "" {
			v = detectVersion(p)
		}
		l.Dependencies[name] = LockDep{Version: v, Path: d.Path}
	}
	return writeLock(root, l)
}

func cmdRemove(args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: aevix-pm remove <name>")
	}
	name := args[0]
	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	m, err := readManifest(root)
	if err != nil {
		return fmt.Errorf("cannot read %s: %w", manifestName, err)
	}
	if _, ok := m.Deps[name]; !ok {
		return fmt.Errorf("dependency %q is not declared in %s", name, manifestName)
	}
	delete(m.Deps, name)
	if err := writeManifest(root, m); err != nil {
		return err
	}
	if err := recomputeLock(root, m); err != nil {
		return err
	}
	fmt.Printf("Removed %s\n", name)
	return nil
}

func cmdList(args []string) error {
	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	m, err := readManifest(root)
	if err != nil {
		return fmt.Errorf("cannot read %s: %w", manifestName, err)
	}
	if len(m.Deps) == 0 {
		fmt.Printf("%s (%s) has no dependencies\n", m.Name, m.Version)
		return nil
	}
	for _, name := range sortedKeys(m.Deps) {
		d := m.Deps[name]
		if d.Version != "" {
			fmt.Printf("%s %s -> %s\n", name, d.Version, d.Path)
		} else {
			fmt.Printf("%s -> %s\n", name, d.Path)
		}
	}
	return nil
}

func cmdVendor(args []string) error {
	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	l, err := readLock(root)
	if err != nil {
		return fmt.Errorf("cannot read %s: %w", lockName, err)
	}
	if len(l.Dependencies) == 0 {
		fmt.Println("no dependencies to vendor")
		return nil
	}
	vendorDir := filepath.Join(root, "lib", "vendor")
	if err := os.RemoveAll(vendorDir); err != nil {
		return err
	}
	if err := os.MkdirAll(vendorDir, 0o755); err != nil {
		return err
	}
	for _, name := range sortedLockKeys(l.Dependencies) {
		dep := l.Dependencies[name]
		if dep.Path == "" {
			continue
		}
		target := filepath.Join(vendorDir, name)
		srcPath := dep.Path
		if !filepath.IsAbs(srcPath) {
			srcPath = filepath.Join(root, srcPath)
		}
		if err := copyTree(srcPath, target); err != nil {
			return fmt.Errorf("vendor %s: %w", name, err)
		}
		fmt.Printf("vendored %s -> %s\n", name, target)
	}
	return nil
}

func copyTree(src, dst string) error {
	info, err := os.Stat(src)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return fmt.Errorf("not a directory: %s", src)
	}
	return filepath.WalkDir(src, func(p string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, p)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		_, err = os.Stat(target)
		if err != nil && !os.IsNotExist(err) {
			return err
		}
		data, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		return os.WriteFile(target, data, 0o644)
	})
}

func cmdUpdate(args []string) error {
	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	m, err := readManifest(root)
	if err != nil {
		return fmt.Errorf("cannot read %s: %w", manifestName, err)
	}
	var only string
	if len(args) > 0 {
		only = args[0]
	}
	if only != "" {
		if _, ok := m.Deps[only]; !ok {
			return fmt.Errorf("dependency %q is not declared in %s", only, manifestName)
		}
	}
	if err := recomputeLock(root, m); err != nil {
		return err
	}
	if only != "" {
		fmt.Printf("Updated %s\n", only)
	} else {
		fmt.Printf("Updated %d dependencies\n", len(m.Deps))
	}
	return nil
}

func cmdTree(args []string) error {
	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	l, err := readLock(root)
	if err != nil {
		return fmt.Errorf("cannot read %s: %w", lockName, err)
	}
	if len(l.Dependencies) == 0 {
		fmt.Printf("%s (%s) has no dependencies\n", l.Name, l.Version)
		return nil
	}
	fmt.Printf("%s (%s)\n", l.Name, l.Version)
	for _, name := range sortedLockKeys(l.Dependencies) {
		dep := l.Dependencies[name]
		if dep.Path != "" {
			fmt.Printf("└── %s (%s) -> %s\n", name, dep.Version, dep.Path)
		} else {
			fmt.Printf("└── %s (%s)\n", name, dep.Version)
		}
	}
	return nil
}

func sortedLockKeys(m map[string]LockDep) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

func cmdWhy(args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: aevix-pm why <name>")
	}
	target := args[0]
	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	m, err := readManifest(root)
	if err != nil {
		return fmt.Errorf("cannot read %s: %w", manifestName, err)
	}
	if d, ok := m.Deps[target]; ok {
		if d.Path != "" {
			fmt.Printf("%s is a direct dependency of %s: -> %s\n", target, m.Name, d.Path)
		} else {
			fmt.Printf("%s is a direct dependency of %s: version %s\n", target, m.Name, d.Version)
		}
		return nil
	}
	return fmt.Errorf("%s is not a dependency of %s", target, m.Name)
}


func sortedKeys(m map[string]Dep) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

// quote renders a Go-style double-quoted string for the TOML subset we emit.
func quote(s string) string {
	b, _ := json.Marshal(s)
	return string(b)
}

// unquote strips one layer of double quotes from a TOML string literal.
func unquote(s string) string {
	s = strings.TrimSpace(s)
	if len(s) >= 2 && s[0] == '"' && s[len(s)-1] == '"' {
		var out string
		if err := json.Unmarshal([]byte(s), &out); err == nil {
			return out
		}
		return s[1 : len(s)-1]
	}
	return s
}

// cut splits s around the first occurrence of sep, reporting whether it was found.
func cut(s, sep string) (before, after string, found bool) {
	i := strings.Index(s, sep)
	if i < 0 {
		return s, "", false
	}
	return s[:i], s[i+len(sep):], true
}

// splitTop splits s on sep, ignoring separators that appear inside double
// quotes — enough to parse the inline tables this tool writes.
func splitTop(s string, sep byte) []string {
	var parts []string
	start := 0
	inStr := false
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"':
			inStr = !inStr
		case c == sep && !inStr:
			parts = append(parts, s[start:i])
			start = i + 1
		}
	}
	parts = append(parts, s[start:])
	return parts
}
