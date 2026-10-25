package main

import (
	"os/exec"
	"strings"
)

// gitTagVersions returns the semver-tag versions available in a local git
// repository. Non-semver tags and non-repositories are silently ignored.
func gitTagVersions(dir string) []Version {
	out, err := exec.Command("git", "-C", dir, "tag", "--list").Output()
	if err != nil {
		return nil
	}
	var versions []Version
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if v, err := parseVersion(line); err == nil {
			versions = append(versions, v)
		}
	}
	return versions
}
