// ============================================================================
// semver.go — a tiny Aevix-flavored SemVer helper.
//
// Backs the `Dep.Version` ranges stored in aevix.toml/updates. The compiler
// never reads them; only aevix-pm does. Parsing is permissive on purpose: a
// version without a leading 'v', a single component, or a build suffix is all
// accepted, because path deps emit 0.0.0 and registry versions arrive from
// git tags / arbitrary strings.
// ============================================================================
package main

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

// Version is a parsed semantic version. Missing components default to 0.
type Version struct {
	Major, Minor, Patch int
	Pre                 string // raw pre-release (no leading '-')
	Build               string // raw build metadata
}

// RangeConstraint is a conjunction of clauses ("1.2, <2.0, >=1.5").
type RangeConstraint struct {
	Clauses []string
}

func (v Version) String() string {
	s := fmt.Sprintf("%d.%d.%d", v.Major, v.Minor, v.Patch)
	if v.Pre != "" {
		s += "-" + v.Pre
	}
	if v.Build != "" {
		s += "+" + v.Build
	}
	return s
}

func parseVersion(s string) (Version, error) {
	s = strings.TrimSpace(s)
	s = strings.TrimPrefix(s, "v")
	var v Version
	if i := strings.IndexByte(s, '+'); i >= 0 {
		v.Build = s[i+1:]
		s = s[:i]
	}
	if i := strings.IndexByte(s, '-'); i >= 0 {
		v.Pre = s[i+1:]
		s = s[:i]
	}
	parts := strings.Split(s, ".")
	for len(parts) < 3 {
		parts = append(parts, "0")
	}
	for i := 0; i < 3; i++ {
		n, err := strconv.Atoi(parts[i])
		if err != nil {
			return v, fmt.Errorf("invalid version %q", s+"-...")
		}
		switch i {
		case 0:
			v.Major = n
		case 1:
			v.Minor = n
		case 2:
			v.Patch = n
		}
	}
	return v, nil
}

// Less reports whether v < w. Pre-release makes a version smaller than the
// corresponding release.
func (v Version) Less(w Version) bool {
	if v.Major != w.Major {
		return v.Major < w.Major
	}
	if v.Minor != w.Minor {
		return v.Minor < w.Minor
	}
	if v.Patch != w.Patch {
		return v.Patch < w.Patch
	}
	if v.Pre == "" && w.Pre != "" {
		return false // release > pre-release
	}
	if v.Pre != "" && w.Pre == "" {
		return true
	}
	return v.Pre < w.Pre // lexical for prereleases
}

func (v Version) Equal(w Version) bool {
	return v.Major == w.Major && v.Minor == w.Minor && v.Patch == w.Patch && v.Pre == w.Pre
}

func (v Version) GreaterOrEqual(w Version) bool { return !v.Less(w) }

// parseRange normalizes a range string into comma-separated clauses. A bare
// value (or "*", "latest") is treated as >= that value.
func parseRange(s string) RangeConstraint {
	s = strings.TrimSpace(s)
	if s == "" || s == "*" || strings.EqualFold(s, "latest") {
		return RangeConstraint{Clauses: nil}
	}
	var clauses []string
	for _, p := range strings.Split(s, ",") {
		p = strings.TrimSpace(p)
		if p != "" {
			clauses = append(clauses, p)
		}
	}
	return RangeConstraint{Clauses: clauses}
}

// satisfies reports whether v fits rc.
func (rc RangeConstraint) Satisfies(v Version) bool {
	if len(rc.Clauses) == 0 {
		return true
	}
	for _, c := range rc.Clauses {
		c = strings.TrimSpace(c)
		switch {
		case strings.HasPrefix(c, ">="):
			w, err := parseVersion(strings.TrimSpace(c[2:]))
			if err != nil || !v.GreaterOrEqual(w) {
				return false
			}
		case strings.HasPrefix(c, "<="):
			w, err := parseVersion(strings.TrimSpace(c[2:]))
			if err != nil || !w.GreaterOrEqual(v) {
				return false
			}
		case strings.HasPrefix(c, ">"):
			w, err := parseVersion(strings.TrimSpace(c[1:]))
			if err != nil || !v.GreaterOrEqual(w) || v.Equal(w) {
				return false
			}
		case strings.HasPrefix(c, "<"):
			w, err := parseVersion(strings.TrimSpace(c[1:]))
			if err != nil || !w.GreaterOrEqual(v) || v.Equal(w) {
				return false
			}
		case strings.HasPrefix(c, "="):
			w, err := parseVersion(strings.TrimSpace(c[1:]))
			if err != nil || !v.Equal(w) {
				return false
			}
		case strings.HasPrefix(c, "^"):
			w, err := parseVersion(strings.TrimSpace(c[1:]))
			if err != nil || !satisfyCaret(v, w) {
				return false
			}
		case strings.HasPrefix(c, "~"):
			w, err := parseVersion(strings.TrimSpace(c[1:]))
			if err != nil || !satisfyTilde(v, w) {
				return false
			}
		default:
			w, err := parseVersion(c)
			if err != nil || !v.Equal(w) {
				return false
			}
		}
	}
	return true
}

func satisfyCaret(v, v0 Version) bool {
	if v.Less(v0) {
		return false
	}
	// ^1.2.3 := >=1.2.3, <2.0.0; ^0.x behaves tighter (cargo rules simplified).
	if v0.Major != 0 {
		return v.Major == v0.Major
	}
	if v0.Minor != 0 {
		return v.Major == 0 && v.Minor == v0.Minor
	}
	return v.Major == 0 && v.Minor == 0 && v.Patch == v0.Patch
}

func satisfyTilde(v, v0 Version) bool {
	if v.Less(v0) {
		return false
	}
	if v0.Major != 0 || v0.Minor != 0 {
		return v.Major == v0.Major && v.Minor == v0.Minor
	}
	return v.Patch == v0.Patch
}

// Range validation helper for `add --version`.
var rangeRe = regexp.MustCompile(`^[0-9A-Za-z,\-+.\^~<>=* ]+$`)

func validRange(s string) bool {
	return rangeRe.MatchString(s)
}
