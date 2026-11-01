package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

// gitOutput runs git with optional working directory and returns trimmed stdout.
func gitOutput(dir string, args ...string) (string, error) {
	full := args
	if dir != "" {
		full = append([]string{"-C", dir}, args...)
	}
	cmd := exec.Command("git", full...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("git %s: %v: %s", strings.Join(full, " "), err, strings.TrimSpace(string(out)))
	}
	return strings.TrimSpace(string(out)), nil
}

// remoteTagVersions lists semver tags advertised by a remote repository.
func remoteTagVersions(url string) []Version {
	out, err := gitOutput("", "ls-remote", "--tags", "--refs", url)
	if err != nil {
		return nil
	}
	var versions []Version
	for _, line := range strings.Split(out, "\n") {
		fields := strings.Fields(line)
		if len(fields) != 2 {
			continue
		}
		tag := strings.TrimPrefix(fields[1], "refs/tags/")
		if v, err := parseVersion(tag); err == nil {
			versions = append(versions, v)
		}
	}
	return versions
}

// remoteTagForVersion returns the actual tag name whose parsed version equals want.
func remoteTagForVersion(url string, want Version) (string, error) {
	out, err := gitOutput("", "ls-remote", "--tags", "--refs", url)
	if err != nil {
		return "", err
	}
	var tags []string
	for _, line := range strings.Split(out, "\n") {
		fields := strings.Fields(line)
		if len(fields) != 2 {
			continue
		}
		tag := strings.TrimPrefix(fields[1], "refs/tags/")
		if v, err := parseVersion(tag); err == nil && v.Equal(want) {
			tags = append(tags, tag)
		}
	}
	if len(tags) == 0 {
		return "", fmt.Errorf("no tag for version %s", want)
	}
	sort.Strings(tags)
	return tags[0], nil
}

// gitRevParse resolves a ref in a local clone to a commit SHA.
func gitRevParse(dir, rev string) string {
	if rev == "" {
		return ""
	}
	out, err := gitOutput(dir, "rev-parse", rev+"^{commit}")
	if err != nil {
		return ""
	}
	return strings.TrimSpace(out)
}

// cloneGit materializes url at target, checking out rev when given.
func resolveGitLock(root, name string, d Dep) (LockDep, error) {
	target := filepath.Join(root, "lib", name)
	rel := filepath.ToSlash(filepath.Join("lib", name))

	rangeStr := d.Version
	if rangeStr == "" {
		rangeStr = "*"
	}
	rc := parseRange(rangeStr)

	rev := d.Rev
	version := ""
	if rev == "" {
		cands := gitTagVersions(target)
		if len(cands) == 0 {
			cands = remoteTagVersions(d.Git)
		}
		best, ok := PickNewest(cands, rc)
		if !ok {
			return LockDep{}, fmt.Errorf("no tag of %s satisfies %q", name, rangeStr)
		}
		version = best.String()
		if tag, err := remoteTagForVersion(d.Git, best); err == nil {
			rev = tag
		} else {
			rev = best.String()
		}
		if commit := gitRevParse(target, rev); commit != "" {
			rev = commit
		} else if commit := remoteTagCommit(d.Git, rev); commit != "" {
			rev = commit
		}
	}

	if version == "" {
		if v, err := parseVersion(rev); err == nil {
			version = v.String()
		} else if local := detectVersion(target); local != "0.0.0" {
			version = local
		} else if cands := gitTagVersions(target); len(cands) > 0 {
			if best, ok := PickNewest(cands, rc); ok {
				version = best.String()
			}
		}
	}
	if version == "" {
		version = "0.0.0"
	}

	return LockDep{
		Version: version,
		Path:    rel,
		Source:  "git+" + d.Git,
		Rev:     rev,
	}, nil
}

func cloneGit(url, target, rev string) error {
	if err := os.RemoveAll(target); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		return err
	}
	if rev != "" {
		// Try shallow clone with branch/tag first
		if _, err := gitOutput("", "clone", "--quiet", "--depth", "1", "--branch", rev, url, target); err != nil {
			// Fall back to full clone + checkout
			if _, err := gitOutput("", "clone", "--quiet", url, target); err != nil {
				return err
			}
			if _, err := gitOutput(target, "checkout", "--quiet", rev); err != nil {
				return err
			}
		}
	} else {
		if _, err := gitOutput("", "clone", "--quiet", url, target); err != nil {
			return err
		}
	}
	if !hasAevSource(target) {
		os.RemoveAll(target)
		return fmt.Errorf("repository %s has no .aev source at its root", url)
	}
	return nil
}

// remoteTagCommit resolves a tag to its commit SHA from the remote.
func remoteTagCommit(url, tag string) string {
	out, err := gitOutput("", "ls-remote", url, "refs/tags/"+tag, "refs/tags/"+tag+"^{}")
	if err != nil {
		return ""
	}
	for _, line := range strings.Split(out, "\n") {
		fields := strings.Fields(line)
		if len(fields) != 2 {
			continue
		}
		// Prefer the peeled ref for annotated tags
		if strings.HasSuffix(fields[1], "^{}") {
			return fields[0]
		}
	}
	return ""
}

// gitTagVersions lists tags in a local clone.

