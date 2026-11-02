package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
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

// checksumDir produces sha256:<hex> over a directory's file contents. Used for
// lock pinning verification.
func checksumDir(dir string) (string, error) {
	h := sha256.New()
	err := filepath.Walk(dir, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		rel, _ := filepath.Rel(dir, p)
		h.Write([]byte(rel + ":"))
		data, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		h.Write(data)
		h.Write([]byte("\n"))
		return nil
	})
	if err != nil {
		return "", err
	}
	return "sha256:" + hex.EncodeToString(h.Sum(nil)), nil
}

// packTarball creates a gzip-compressed tarball of src at dst.
func packTarball(src, dst string) error {
	f, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer f.Close()
	gw := gzip.NewWriter(f)
	defer gw.Close()
	tw := tar.NewWriter(gw)
	defer tw.Close()
	return filepath.Walk(src, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, p)
		if rel == "." {
			return nil
		}
		if info.IsDir() && info.Name() == ".git" {
			return filepath.SkipDir
		}
		hdr, err := tar.FileInfoHeader(info, "")
		if err != nil {
			return err
		}
		hdr.Name = rel + "/"
		if !info.IsDir() {
			hdr.Name = rel
			data, rerr := os.ReadFile(p)
			if rerr != nil {
				return rerr
			}
			hdr.Size = int64(len(data))
			hdr.Mode = 0644
			hdr.Typeflag = tar.TypeReg
			if err := tw.WriteHeader(hdr); err != nil {
				return err
			}
			if _, err := tw.Write(data); err != nil {
				return err
			}
		} else {
			hdr.Typeflag = tar.TypeDir
			hdr.Mode = 0755
			if err := tw.WriteHeader(hdr); err != nil {
				return err
			}
		}
		return nil
	})
}

// unpackTarball extracts a gzip tarball at src into dst and validates path
// traversal.
func unpackTarball(src, dst string) error {
	f, err := os.Open(src)
	if err != nil {
		return err
	}
	defer f.Close()
	gr, err := gzip.NewReader(f)
	if err != nil {
		return err
	}
	defer gr.Close()
	tr := tar.NewReader(gr)
	if err := os.MkdirAll(dst, 0o755); err != nil {
		return err
	}
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		name := filepath.Clean(hdr.Name)
		if name == "" {
			continue
		}
		target := filepath.Join(dst, name)
		if !strings.HasPrefix(target, filepath.Clean(dst)+string(os.PathSeparator)) && target != filepath.Clean(dst) {
			return fmt.Errorf("path traversal in tar entry: %s", hdr.Name)
		}
		if hdr.Typeflag == tar.TypeDir {
			if err := os.MkdirAll(target, 0o755); err != nil {
				return err
			}
			continue
		}
		if hdr.Typeflag != tar.TypeReg {
			return fmt.Errorf("unsupported tar type %d in %s", hdr.Typeflag, hdr.Name)
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		out, err := os.Create(target)
		if err != nil {
			return err
		}
		if _, err := io.Copy(out, tr); err != nil {
			out.Close()
			return err
		}
		out.Close()
	}
	return nil
}

// cmdServe runs a minimal HTTP directory registry at the given address.
func cmdServe(args []string) error {
	root := defaultRegistryDir()
	addr := ":8080"
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--root":
			if i+1 >= len(args) {
				return fmt.Errorf("--root requires a directory")
			}
			root = args[i+1]
			i++
		case "--addr":
			if i+1 >= len(args) {
				return fmt.Errorf("--addr requires a bind address")
			}
			addr = args[i+1]
			i++
		}
	}
	absRoot, err := filepath.Abs(root)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(absRoot, 0o755); err != nil {
		return err
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/packages/", func(w http.ResponseWriter, r *http.Request) {
		// routing:
		// GET    /packages/{name}             -> index.json
		// GET    /packages/{name}/versions/{v} -> tarball
		// POST   /packages/{name}/versions/{v}  -> upload tarball + index update
		r.URL.Path = cleanpath(r.URL.Path)
		parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/packages/"), "/")
		if len(parts) == 1 {
			name := parts[0]
			if r.Method == "GET" {
				http.ServeFile(w, r, filepath.Join(absRoot, name, "index.json"))
				return
			}
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		if len(parts) == 3 && parts[1] == "versions" {
			name, ver := parts[0], parts[2]
			verPath := filepath.Join(absRoot, name, "versions", ver)
			tarPath := verPath + ".tar.gz"
			switch r.Method {
			case "GET":
				if _, err := os.Stat(tarPath); err == nil {
					http.ServeFile(w, r, tarPath)
				} else if info, err := os.Stat(verPath); err == nil && info.IsDir() {
					// fall back to a freshly packed tarball
					if err := packTarball(verPath, tarPath); err != nil {
						http.Error(w, err.Error(), http.StatusInternalServerError)
						return
					}
					http.ServeFile(w, r, tarPath)
				} else {
					http.NotFound(w, r)
				}
			case "POST", "PUT":
				tmp, err := os.CreateTemp("", "publish-*.tar.gz")
				if err != nil {
					http.Error(w, err.Error(), http.StatusInternalServerError)
					return
				}
				defer tmp.Close()
				if _, err := io.Copy(tmp, r.Body); err != nil {
					http.Error(w, err.Error(), http.StatusBadRequest)
					return
				}
				if err := os.MkdirAll(verPath, 0o755); err != nil {
					http.Error(w, err.Error(), http.StatusInternalServerError)
					return
				}
				// extract
				if err := unpackTarball(tmp.Name(), verPath); err != nil {
					http.Error(w, err.Error(), http.StatusBadRequest)
					return
				}
				// store tarball for future
				_ = os.Rename(tmp.Name(), tarPath)
				// append index
				_ = registryAddVersion(filepath.Join(absRoot, name), ver)
				fmt.Fprintf(w, "published %s %s\n", name, ver)
			default:
				http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			}
			return
		}
		http.NotFound(w, r)
	})
	fmt.Printf("registry serving root=%s at %s\n", absRoot, addr)
	return http.ListenAndServe(addr, mux)
}

func cleanpath(s string) string {
	// shorten query/mailto issues, keep stable
	return strings.TrimSpace(s)
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
	if isHTTPUrl(registry) {
		// pack to temp then POST
		tmp, err := os.CreateTemp("", "publish-*.tar.gz")
		if err != nil {
			return err
		}
		tmpn := tmp.Name()
		tmp.Close()
		if err := packTarball(root, tmpn); err != nil {
			os.Remove(tmpn)
			return fmt.Errorf("pack: %w", err)
		}
		data, err := os.ReadFile(tmpn)
		os.Remove(tmpn)
		if err != nil {
			return err
		}
		u := strings.TrimRight(registry, "/") + "/packages/" + m.Name + "/versions/" + m.Version
		req, err := http.NewRequest("POST", u, bytes.NewReader(data))
		if err != nil {
			return err
		}
		req.Header.Set("Content-Type", "application/gzip")
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return err
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			return fmt.Errorf("http %d: %s", resp.StatusCode, string(b))
		}
		fmt.Printf("%s", string(b))
		return nil
	}
	if strings.HasPrefix(registry, "http://") || strings.HasPrefix(registry, "https://") {
		// Pack to temp tarball and POST it to the registry server.
		tmp, err := os.CreateTemp("", "publish-*.tar.gz")
		if err != nil {
			return err
		}
		tmpn := tmp.Name()
		tmp.Close()
		if err := packTarball(root, tmpn); err != nil {
			os.Remove(tmpn)
			return fmt.Errorf("publish %s@%s pack: %w", m.Name, m.Version, err)
		}
		data, err := os.ReadFile(tmpn)
		os.Remove(tmpn)
		if err != nil {
			return err
		}
		u := strings.TrimRight(registry, "/") + "/packages/" + m.Name + "/versions/" + m.Version
		req, err := http.NewRequest("POST", u, bytes.NewReader(data))
		if err != nil {
			return err
		}
		req.Header.Set("Content-Type", "application/gzip")
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return err
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			return fmt.Errorf("publish %s@%s: http %d: %s", m.Name, m.Version, resp.StatusCode, string(b))
		}
		fmt.Printf("%s", string(b))
		return nil
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

// ----- HTTP registry helpers (0.4.x) -----

func isHTTPUrl(s string) bool {
	return strings.HasPrefix(s, "http://") || strings.HasPrefix(s, "https://")
}

func httpRegistryIndex(reg, name string) (map[string]any, error) {
	url := strings.TrimRight(reg, "/") + "/packages/" + name
	resp, err := http.Get(url)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("http %d: %s", resp.StatusCode, resp.Status)
	}
	var out map[string]any
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, err
	}
	return out, nil
}

func httpFetchTarball(reg, name, version, target string) error {
	url := strings.TrimRight(reg, "/") + "/packages/" + name + "/versions/" + version
	resp, err := http.Get(url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("http %d: %s", resp.StatusCode, resp.Status)
	}
	os.MkdirAll(filepath.Dir(target+".tmp"), 0o755)
	tmp := target + ".tar.gz.tmp"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	if _, err := io.Copy(f, resp.Body); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	f.Close()
	if err := unpackTarball(tmp, target); err != nil {
		os.Remove(tmp)
		return err
	}
	os.Remove(tmp)
	return nil
}

func registryScheme(reg string) string {
	if strings.HasPrefix(reg, "https://") {
		return "https"
	}
	return "http"
}
