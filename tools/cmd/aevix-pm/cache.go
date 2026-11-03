// ============================================================================
// cache.go — global package cache (~/.cache/aevix/pkg) for 0.4.x
//
// A shared, user-level cache so multiple projects don't re-fetch the same
// package. Cache entries are keyed by (name, version, rev) and stored under
// a single directory. Network access still only happens through `add`,
// `update`, or `install` — builds never touch it implicitly.
// ============================================================================
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// cacheRoot resolves the cache root. AEVIX_CACHE_DIR overrides; $XDG_CACHE_HOME
// or ~/.cache/aevix/pkg are the fallbacks.
func cacheRoot() (string, error) {
	if v := os.Getenv("AEVIX_CACHE_DIR"); v != "" {
		abs, err := filepath.Abs(v)
		if err != nil {
			return "", err
		}
		return filepath.ToSlash(filepath.Clean(abs)), nil
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	base := os.Getenv("XDG_CACHE_HOME")
	if base == "" {
		base = filepath.Join(home, ".cache")
	}
	return filepath.ToSlash(filepath.Join(base, "aevix", "pkg")), nil
}

// cacheEntryName returns a filesystem-safe key for (name, version, rev). rev is
// part of the key only if non-empty; short revs keep directory names sane.
func cacheEntryName(name, version, rev string) string {
	key := name + "@" + version
	if rev != "" {
		trim := rev
		if len(trim) > 12 {
			trim = trim[:12]
		}
		key = key + "-" + trim
	}
	return strings.Map(func(r rune) rune {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
			return r
		case r == '-', r == '_', r == '.':
			return r
		default:
			return '-'
		}
	}, key)
}

// cacheEntryPath returns the absolute path for an (name, version, rev) entry.
func cacheEntryPath(name, version, rev string) (string, error) {
	root, err := cacheRoot()
	if err != nil {
		return "", err
	}
	return filepath.Join(root, cacheEntryName(name, version, rev)), nil
}

// cacheContains reports whether a cache entry for (name, version, rev) exists.
func cacheContains(name, version, rev string) bool {
	p, err := cacheEntryPath(name, version, rev)
	if err != nil {
		return false
	}
	fi, err := os.Stat(p)
	return err == nil && fi.IsDir()
}

// materializeToCache copies src into the cache entry keyed by (name,version,rev).
// Returns the cached path, or "" on any error. Best-effort only.
func materializeToCache(name, version, rev, src string) string {
	p, err := cacheEntryPath(name, version, rev)
	if err != nil {
		return ""
	}
	fi, err := os.Stat(p)
	if err == nil && fi.IsDir() {
		return p
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return ""
	}
	tmp := p + ".tmp"
	os.RemoveAll(tmp)
	if err := os.MkdirAll(tmp, 0o755); err != nil {
		return ""
	}
	if err := copyTree(src, tmp); err != nil {
		os.RemoveAll(tmp)
		return ""
	}
	if err := os.Rename(tmp, p); err != nil {
		os.RemoveAll(tmp)
		return ""
	}
	return p
}

// materializeFromCache copies a cache entry into dst. Returns error if not cached.
func materializeFromCache(name, version, rev, dst string) error {
	p, err := cacheEntryPath(name, version, rev)
	if err != nil {
		return err
	}
	fi, err := os.Stat(p)
	if err != nil || !fi.IsDir() {
		return fmt.Errorf("not cached: %s@%s", name, version)
	}
	if err := os.RemoveAll(dst); err != nil {
		return err
	}
	return copyTree(p, dst)
}

// cmdCache dispatched `aevix-pm cache <info|clean<|message|>list<|channel|>path>`.
func cmdCache(args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: aevix-pm cache <info|clean|list|path>")
	}
	switch args[0] {
	case "info":
		return cmdCacheInfo()
	case "clean":
		return cmdCacheClean()
	case "list":
		return cmdCacheList()
	case "path":
		root, err := cacheRoot()
		if err != nil {
			return err
		}
		fmt.Println(root)
		return nil
	default:
		return fmt.Errorf("unknown cache subcommand %q", args[0])
	}
}

func cmdCacheInfo() error {
	root, err := cacheRoot()
	if err != nil {
		return err
	}
	fmt.Printf("cache root: %s\n", root)
	fi, err := os.Stat(root)
	if err != nil || !fi.IsDir() {
		fmt.Println("status: empty (directory does not exist)")
		return nil
	}
	entries, err := os.ReadDir(root)
	if err != nil {
		return err
	}
	fmt.Printf("entries: %d\n", len(entries))
	for _, e := range entries {
		fmt.Printf("  - %s\n", e.Name())
	}
	return nil
}

func cmdCacheClean() error {
	root, err := cacheRoot()
	if err != nil {
		return err
	}
	if _, err := os.Stat(root); err != nil {
		fmt.Println("cache already empty")
		return nil
	}
	if err := os.RemoveAll(root); err != nil {
		return err
	}
	fmt.Printf("removed cache: %s\n", root)
	return nil
}

func cmdCacheList() error {
	return cmdCacheInfo()
}
