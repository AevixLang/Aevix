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

// Dep is one declared dependency: either a local working tree (Path) or a
// remote git repository (Git), plus an optional semver range and an optional
// exact revision. The two sources are mutually exclusive.
type Dep struct {
	Path    string // directory of the package, relative to the manifest
	Git     string // remote git URL (https://, ssh://, or file://)
	Rev     string // optional exact tag/commit; empty means "newest tag in range"
	Version string // semver range, or the detected version for path deps
}

// Lock mirrors aevix.lock: the resolved, reproducible graph the compiler reads.
// It is JSON to match the format both frontends already parse today.
type Lock struct {
	Name         string             `json:"name"`
	Version      string             `json:"version"`
	Dependencies map[string]LockDep `json:"dependencies"`
}

// LockDep is one pinned dependency in the lock. Path is always the materialized
// directory the compiler reads; Source/Rev are provenance for remote deps.
type LockDep struct {
	Version string `json:"version,omitempty"`
	Path    string `json:"path,omitempty"`
	Source  string `json:"source,omitempty"` // "git+<url>" for remote deps
	Rev     string `json:"rev,omitempty"`    // pinned commit SHA
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
	case "install":
		run(cmdInstall(os.Args[2:]))
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
	fmt.Fprintln(os.Stderr, "  add <name> --git <url>   Add a git dependency, newest tag in range")
	fmt.Fprintln(os.Stderr, "  remove <name>            Remove a dependency and update the lock")
	fmt.Fprintln(os.Stderr, "  update [name]            Re-resolve deps to match the manifest")
	fmt.Fprintln(os.Stderr, "  list                     List declared dependencies")
	fmt.Fprintln(os.Stderr, "  install                  Materialize git deps pinned in aevix.lock")
	fmt.Fprintln(os.Stderr, "  vendor                   Copy dependencies into lib/vendor/")
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
		var parts []string
		if d.Path != "" {
			parts = append(parts, fmt.Sprintf("path = %s", quote(d.Path)))
		}
		if d.Git != "" {
			parts = append(parts, fmt.Sprintf("git = %s", quote(d.Git)))
		}
		if d.Version != "" {
			parts = append(parts, fmt.Sprintf("version = %s", quote(d.Version)))
		}
		if d.Rev != "" {
			parts = append(parts, fmt.Sprintf("rev = %s", quote(d.Rev)))
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
		case "git":
			d.Git = unquote(v)
		case "version":
			d.Version = unquote(v)
		case "rev":
			d.Rev = unquote(v)
		}
	}
	if d.Path == "" && d.Git == "" {
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
		return fmt.Errorf("usage: aevix-pm add <name> (--path <dir> | --git <url>) [--version <range>] [--rev <rev>]")
	}
	name := args[0]
	if !validDepName(name) {
		return fmt.Errorf("invalid package name %q (use [a-z0-9_-])", name)
	}
	var path, gitURL, version, rev string
	for i := 1; i < len(args); i++ {
		switch args[i] {
		case "--path":
			if i+1 >= len(args) {
				return fmt.Errorf("--path requires a directory")
			}
			path = args[i+1]
			i++
		case "--git":
			if i+1 >= len(args) {
				return fmt.Errorf("--git requires a URL")
			}
			gitURL = args[i+1]
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
		case "--rev":
			if i+1 >= len(args) {
				return fmt.Errorf("--rev requires a tag or commit")
			}
			rev = args[i+1]
			i++
		default:
			return fmt.Errorf("unexpected argument %q", args[i])
		}
	}
	if path == "" && gitURL == "" {
		return fmt.Errorf("add requires --path <dir> or --git <url>")
	}
	if path != "" && gitURL != "" {
		return fmt.Errorf("--path and --git are mutually exclusive")
	}
	if rev != "" && gitURL == "" {
		return fmt.Errorf("--rev is only valid with --git")
	}

	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	m, err := readManifest(root)
	if err != nil {
		return fmt.Errorf("cannot read %s: %w", manifestName, err)
	}

	if gitURL != "" {
		if version == "" {
			version = "*"
		}
		target := filepath.Join(root, "lib", name)
		cloneRef := rev
		if cloneRef == "" {
			tags := remoteTagVersions(gitURL)
			if len(tags) == 0 {
				return fmt.Errorf("no semver tags found in %s (use --rev for a pinned checkout)", gitURL)
			}
			best, ok := PickNewest(tags, parseRange(version))
			if !ok {
				return fmt.Errorf("no tag of %s satisfies %q", name, version)
			}
			tag, err := remoteTagForVersion(gitURL, best)
			if err != nil {
				return err
			}
			cloneRef = tag
		}
		if err := cloneGit(gitURL, target, cloneRef); err != nil {
			return err
		}
		m.Deps[name] = Dep{Git: gitURL, Version: version, Rev: rev}
		if err := writeManifest(root, m); err != nil {
			return err
		}
		if err := recomputeLock(root, m); err != nil {
			return err
		}
		fmt.Printf("Added %s (%s) from %s -> lib/%s\n", name, version, gitURL, name)
		return nil
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
func resolveLockVersion(root string, d Dep) string {
	p := d.Path
	if !filepath.IsAbs(p) {
		p = filepath.Join(root, p)
	}
	// "update": pick the newest git tag that satisfies the declared range.
	if cands := gitTagVersions(p); len(cands) > 0 {
		rc := parseRange(d.Version)
		if best, ok := PickNewest(cands, rc); ok {
			return best.String()
		}
	}
	v := strings.TrimSpace(d.Version)
	if v == "" || v == "*" || strings.EqualFold(v, "latest") {
		v = detectVersion(p)
	}
	return v
}

func recomputeLock(root string, m *Manifest) error {
	l := &Lock{Name: m.Name, Version: m.Version, Dependencies: map[string]LockDep{}}
	for _, name := range sortedKeys(m.Deps) {
		d := m.Deps[name]
		if d.Git != "" {
			ld, err := resolveGitLock(root, name, d)
			if err != nil {
				return err
			}
			l.Dependencies[name] = ld
			continue
		}
		l.Dependencies[name] = LockDep{
			Version: resolveLockVersion(root, d),
			Path:    d.Path,
		}
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
		src := d.Path
		if d.Git != "" {
			src = d.Git
		}
		if d.Version != "" {
			fmt.Printf("%s %s -> %s\n", name, d.Version, src)
		} else {
			fmt.Printf("%s -> %s\n", name, src)
		}
	}
	return nil
}

// cmdInstall materializes every git dependency pinned in aevix.lock. It never
// re-resolves versions: the lock is the contract, so an install on another
// machine produces the same source tree.
func cmdInstall(args []string) error {
	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	l, err := readLock(root)
	if err != nil {
		return fmt.Errorf("cannot read %s: %w", lockName, err)
	}
	installed := 0
	for _, name := range sortedLockKeys(l.Dependencies) {
		if !validDepName(name) {
			return fmt.Errorf("invalid package name %q in %s", name, lockName)
		}
		dep := l.Dependencies[name]
		if dep.Source == "" {
			continue
		}
		if !strings.HasPrefix(dep.Source, "git+") {
			return fmt.Errorf("unsupported source for %s: %s", name, dep.Source)
		}
		url := strings.TrimPrefix(dep.Source, "git+")
		target := dep.Path
		if target == "" {
			target = filepath.ToSlash(filepath.Join("lib", name))
		}
		abs := target
		if !filepath.IsAbs(abs) {
			abs = filepath.Join(root, abs)
		}
		ref := dep.Rev
		if ref == "" {
			ref = dep.Version
		}
		if err := cloneGit(url, abs, ref); err != nil {
			return fmt.Errorf("install %s: %w", name, err)
		}
		fmt.Printf("installed %s (%s) -> %s\n", name, dep.Version, target)
		installed++
	}
	if installed == 0 {
		fmt.Println("no remote dependencies to install")
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
	if err := os.MkdirAll(vendorDir, 0o755); err != nil {
		return err
	}
	for _, name := range sortedLockKeys(l.Dependencies) {
		if !validDepName(name) {
			return fmt.Errorf("invalid package name %q in %s", name, lockName)
		}
		dep := l.Dependencies[name]
		if dep.Path == "" {
			continue
		}
		// A second `vendor` run must not delete or recurse into the copy it
		// produced the first time.
		if strings.HasPrefix(filepath.ToSlash(dep.Path), "lib/vendor/") {
			fmt.Printf("already vendored %s\n", name)
			continue
		}
		srcPath := dep.Path
		if !filepath.IsAbs(srcPath) {
			srcPath = filepath.Join(root, srcPath)
		}
		target := filepath.Join(vendorDir, name)
		if err := os.RemoveAll(target); err != nil {
			return err
		}
		if err := copyTree(srcPath, target); err != nil {
			return fmt.Errorf("vendor %s: %w", name, err)
		}
		fmt.Printf("vendored %s -> %s\n", name, target)
		// Point the lock at the vendored copy so the project stays buildable
		// after the source tree is removed: offline, self-contained builds.
		dep.Path = filepath.ToSlash(filepath.Join("lib", "vendor", name))
		l.Dependencies[name] = dep
	}
	return writeLock(root, l)
}

func copyTree(src, dst string) error {
	info, err := os.Stat(src)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return fmt.Errorf("not a directory: %s", src)
	}
	absSrc, err := filepath.Abs(src)
	if err != nil {
		return err
	}
	absDst, err := filepath.Abs(dst)
	if err != nil {
		return err
	}
	// Vendoring a project into its own lib/vendor tree would recurse forever.
	if absDst == absSrc || strings.HasPrefix(absDst, absSrc+string(os.PathSeparator)) {
		return fmt.Errorf("refusing to copy %s into itself", src)
	}
	return filepath.WalkDir(src, func(p string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		// WalkDir does not follow symlinks, so this catches both a symlinked
		// file and a symlinked directory before any bytes are copied.
		if d.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("refusing to copy symlink: %s", p)
		}
		// A submodule stores .git as a file, a normal repository as a
		// directory; neither is package source.
		if d.Name() == ".git" {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		rel, err := filepath.Rel(src, p)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		// Device nodes, sockets, and FIFOs cannot be reproduced reproducibly.
		if !d.Type().IsRegular() {
			return fmt.Errorf("refusing to copy non-regular file: %s", p)
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
	for i := 0; i < len(args); i++ {
		if args[i] == "--name" && i+1 < len(args) {
			only = args[i+1]
			i++
		} else if !strings.HasPrefix(args[i], "--") {
			only = args[i]
		} else {
			return fmt.Errorf("unknown option %s", args[i])
		}
	}
	if only != "" {
		if _, ok := m.Deps[only]; !ok {
			return fmt.Errorf("dependency %q is not declared in %s", only, manifestName)
		}
	}
	// Re-materialize git dependencies at the newest tag in their declared
	// range, then rebuild the lock. Path deps are re-resolved from their local
	// git tags by recomputeLock.
	for name, d := range m.Deps {
		if only != "" && name != only {
			continue
		}
		if d.Git == "" {
			continue
		}
		ref := d.Rev
		if ref == "" {
			tags := remoteTagVersions(d.Git)
			if len(tags) == 0 {
				return fmt.Errorf("no semver tags found in %s", d.Git)
			}
			best, ok := PickNewest(tags, parseRange(d.Version))
			if !ok {
				return fmt.Errorf("no tag of %s satisfies %q", name, d.Version)
			}
			tag, err := remoteTagForVersion(d.Git, best)
			if err != nil {
				return err
			}
			ref = tag
		}
		if err := cloneGit(d.Git, filepath.Join(root, "lib", name), ref); err != nil {
			return fmt.Errorf("update %s: %w", name, err)
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
	visited := map[string]bool{}
	for _, name := range sortedLockKeys(l.Dependencies) {
		printDepTree(root, name, l.Dependencies[name], 0, visited)
	}
	return nil
}

func printDepTree(baseDir, name string, dep LockDep, depth int, visited map[string]bool) {
	pad := strings.Repeat("    ", depth)
	if dep.Path != "" {
		fmt.Printf("%s└── %s (%s) -> %s\n", pad, name, dep.Version, dep.Path)
	} else {
		fmt.Printf("%s└── %s (%s)\n", pad, name, dep.Version)
	}
	depDir := dep.Path
	if depDir == "" {
		return
	}
	if !filepath.IsAbs(depDir) {
		depDir = filepath.Join(baseDir, depDir)
	}
	if visited[depDir] {
		return
	}
	visited[depDir] = true
	m, err := readManifest(depDir)
	if err != nil {
		// A dependency without its own manifest is a leaf.
		return
	}
	for _, childName := range sortedKeys(m.Deps) {
		child := m.Deps[childName]
		childDir := childPath(depDir, child.Path)
		childLockPath := child.Path
		if child.Path == "" && child.Git != "" {
			childDir = filepath.Join(depDir, "lib", childName)
			childLockPath = filepath.ToSlash(filepath.Join("lib", childName))
		}
		v := child.Version
		if v == "" {
			v = detectVersion(childDir)
		}
		childDep := LockDep{Path: childLockPath, Version: v}
		printDepTree(depDir, childName, childDep, depth+1, visited)
	}
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
	rootDep := map[string]Dep{}
	for k, v := range m.Deps {
		rootDep[k] = v
	}
	path, ok := findDepChain(rootDep, root, target, nil, map[string]bool{})
	if !ok {
		return fmt.Errorf("%s is not a dependency of %s", target, m.Name)
	}
	fmt.Printf("%s is required by %s: %s\n", target, m.Name, strings.Join(path, " -> "))
	return nil
}

// findDepChain returns the chain of package names from the root's direct
// dependency to target, or ok==false if target is not reachable. It guards
// against cycles.
func findDepChain(deps map[string]Dep, baseDir, target string, chain []string, visited map[string]bool) ([]string, bool) {
	// DFS with root-first chain building.
	var helper func(deps map[string]Dep, baseDir, target string, chain []string, visited map[string]bool) ([]string, bool)
	helper = func(deps map[string]Dep, baseDir, target string, chain []string, visited map[string]bool) ([]string, bool) {
		for _, name := range sortedKeys(deps) {
			d := deps[name]
			if name == target {
				return append([]string{}, append(chain, name)...), true
			}
			depDir := d.Path
			if depDir == "" && d.Git != "" {
				depDir = filepath.Join("lib", name)
			}
			if depDir == "" {
				continue
			}
			if !filepath.IsAbs(depDir) {
				depDir = filepath.Join(baseDir, depDir)
			}
			if visited[depDir] {
				continue
			}
			nextVisited := make(map[string]bool)
			for k := range visited {
				nextVisited[k] = true
			}
			nextVisited[depDir] = true
			childManifest, err := readManifest(depDir)
			if err != nil {
				continue
			}
			childChain := append([]string{}, append(chain, name)...)
			if found, ok := helper(childManifest.Deps, depDir, target, childChain, nextVisited); ok {
				return found, ok
			}
		}
		return nil, false
	}
	return helper(deps, baseDir, target, chain, visited)
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

func childPath(base, p string) string {
	if filepath.IsAbs(p) {
		return p
	}
	return filepath.Join(base, p)
}
