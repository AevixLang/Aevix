package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// defaultRegistryDir is where publish refuses to bypass explicit flags.
func defaultRegistryDir() string {
	if v := os.Getenv("AEVIX_REGISTRY"); v != "" {
		return v
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return ""
	}
	return filepath.Join(home, ".aevix", "registry")
}

var knownRegistrySourcePrefixes = []string{"registry+file+", "registry+https+", "registry+http+"}

func registrySourceKind(source string) (string, string, bool) {
	for _, pre := range knownRegistrySourcePrefixes {
		if strings.HasPrefix(source, pre) {
			return strings.TrimPrefix(source, "registry+"), strings.TrimPrefix(strings.TrimPrefix(source, pre), "+"), true
		}
	}
	return "", "", false
}

func registryReadIndex(dir string) (map[string][]string, error) {
	idxPath := filepath.Join(dir, "index.json")
	b, err := os.ReadFile(idxPath)
	if err != nil {
		return nil, err
	}
	var out struct {
		Name     string   `json:"name"`
		Versions []string `json:"versions"`
	}
	if err := json.Unmarshal(b, &out); err != nil {
		return nil, err
	}
	return map[string][]string{"versions": out.Versions}, nil
}

func registryAddVersion(dir, version string) error {
	idxPath := filepath.Join(dir, "index.json")
	var out struct {
		Name     string   `json:"name"`
		Versions []string `json:"versions"`
	}
	if b, err := os.ReadFile(idxPath); err == nil {
		_ = json.Unmarshal(b, &out)
	}
	if out.Name == "" {
		out.Name = filepath.Base(dir)
	}
	// dedupe
	found := false
	for _, v := range out.Versions {
		if v == version {
			found = true
		}
	}
	if !found {
		out.Versions = append(out.Versions, version)
	}
	sort.Strings(out.Versions)
	b, err := json.MarshalIndent(out, "", "  ")
	if err != nil {
		return err
	}
	// write atomically
	tmp := idxPath + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, idxPath)
}

// packDir packs src (package root) into dst (versioned package dir) by copyTree.
func packDir(src, dst string) error {
	if err := os.RemoveAll(dst); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	return copyTree(src, dst)
}

func cmdPublish(args []string) error {
	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	m, err := readManifest(root)
	if err != nil {
		return err
	}
	if m.Version == "" {
		return fmt.Errorf("manifest has no version; set version in aevix.toml")
	}
	registry := ""
	for i := 0; i < len(args); i++ {
		if args[i] == "--registry" && i+1 < len(args) {
			registry = args[i+1]
			i++
		}
	}
	if registry == "" {
		registry = defaultRegistryDir()
	}
	if registry == "" {
		return fmt.Errorf("no registry specified (--registry) and home dir unavailable")
	}
	absReg, err := filepath.Abs(registry)
	if err != nil {
		return err
	}
	pkgDir := filepath.Join(absReg, m.Name, "versions", m.Version)
	// src is the project root (whole package)
	if err := packDir(root, pkgDir); err != nil {
		return fmt.Errorf("publish %s@%s: %w", m.Name, m.Version, err)
	}
	if err := registryAddVersion(filepath.Join(absReg, m.Name), m.Version); err != nil {
		return fmt.Errorf("publish index: %w", err)
	}
	fmt.Printf("published %s@%s -> %s\n", m.Name, m.Version, pkgDir)
	return nil
}
