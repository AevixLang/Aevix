package main

import (
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"time"
)

var rootDir string
var workDir string

func repoRootDir() string {
	// Locate the compiler repo from this executable's own location so that a
	// globally-installed `aevix` works from any directory.
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	if resolved, err2 := filepath.EvalSymlinks(exe); err2 == nil {
		exe = resolved
	}
	return filepath.Dir(exe)
}

// buildRoot returns the directory holding all per-source build artifacts.
// Every artifact of a build lives under here, never in the repo root.
func buildRoot() string {
	return filepath.Join(os.TempDir(), "aevix-build")
}

// buildDirFor returns a stable per-source build directory, keyed by the
// absolute source path. Reusing the same directory lets `--stage` re-run a
// single stage while the other artifacts from the previous build remain.
func buildDirFor(absSrc string) string {
	h := sha1.Sum([]byte(absSrc))
	key := hex.EncodeToString(h[:])[:12]
	return filepath.Join(buildRoot(), key)
}

// stage is one step of the build pipeline. `name` is used for `--stage`
// selection and `-v` timing output; `fn` performs the step's work.
type stage struct {
	name string
	fn   func() error
}

// runStages executes the given stages in order, timing each one. When
// `only` is non-empty, exactly that named stage is run (inputs are expected
// to already exist on disk). Returns a gofmt-compatible error message.
func runStages(stages []stage, verbose bool, only string) error {
	selected := stages
	if only != "" {
		found := false
		for _, s := range stages {
			if s.name == only {
				selected = []stage{s}
				found = true
				break
			}
		}
		if !found {
			return fmt.Errorf("unknown stage %q (expected one of: %s)",
				only, stageNames(stages))
		}
	}
	for _, s := range selected {
		start := time.Now()
		if err := s.fn(); err != nil {
			return fmt.Errorf("%s stage failed: %w", s.name, err)
		}
		if verbose {
			fmt.Printf("%s %s\n", s.name, time.Since(start).Round(time.Millisecond))
		}
	}
	return nil
}

func stageNames(stages []stage) string {
	names := make([]string, 0, len(stages))
	for _, s := range stages {
		names = append(names, s.name)
	}
	return joinStrings(names, ", ")
}

func joinStrings(s []string, sep string) string {
	out := ""
	for i, v := range s {
		if i > 0 {
			out += sep
		}
		out += v
	}
	return out
}

// buildProject compiles srcFile into an executable and returns its path.
// All intermediate artifacts are written under buildDirFor(src); the repo
// root is never touched. `only` re-runs a single pipeline stage.
func buildProject(srcFile string, verbose bool, only string) (string, error) {
	python := venvPython()
	if _, err := os.Stat(python); err != nil {
		return "", fmt.Errorf("venv not found. Please run `python bootstrap.py` first")
	}

	backendExe := filepath.Join(rootDir, "backend", "build", "aevix-backend")
	if runtime.GOOS == "windows" {
		backendExe += ".exe"
	}

	var absSrc string
	if filepath.IsAbs(srcFile) {
		absSrc = srcFile
	} else {
		absSrc = filepath.Join(workDir, srcFile)
	}

	buildDir := buildDirFor(absSrc)
	if err := os.MkdirAll(buildDir, 0o755); err != nil {
		return "", err
	}

	astPath := filepath.Join(buildDir, "ast.json")
	irPath := filepath.Join(buildDir, "output.ll")
	objPath := filepath.Join(buildDir, "output.o")
	progOut := filepath.Join(buildDir, "program")
	if runtime.GOOS == "windows" {
		progOut += ".exe"
	}

	stages := []stage{
		{"parse", func() error {
			// Python: .aev -> ast.json (written straight into the build dir)
			return runFrontend(python, absSrc, astPath)
		}},
		{"backend", func() error {
			// Backend: ast.json -> LLVM IR on stdout, captured to a file
			return runBackend(backendExe, astPath, irPath)
		}},
		{"llc", func() error {
			// llc: IR -> object file
			return runLLVMTool("llc", "-filetype=obj", irPath, "-o", objPath)
		}},
		{"link", func() error {
			// clang: object + arena runtime -> executable
			runtimeObj := filepath.Join(rootDir, "backend", "build", "runtime.o")
			return runTool("clang", objPath, runtimeObj, "-o", progOut)
		}},
	}

	if err := runStages(stages, verbose, only); err != nil {
		return "", err
	}

	fmt.Println("✅ Build complete ->", progOut)
	return progOut, nil
}

func venvPython() string {
	if runtime.GOOS == "windows" {
		return filepath.Join(rootDir, "venv", "Scripts", "python.exe")
	}
	return filepath.Join(rootDir, "venv", "bin", "python")
}

func runFrontend(python, src, out string) error {
	cmd := exec.Command(python, "-m", "src.main", src, "-o", out)
	cmd.Dir = filepath.Join(rootDir, "frontend")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

func runBackend(backendExe, astPath, irPath string) error {
	cmd := exec.Command(backendExe, astPath)
	cmd.Dir = rootDir
	var ir []byte
	cmd.Stdout = decodeOut(&ir)
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return err
	}
	return os.WriteFile(irPath, ir, 0o644)
}

func runLLVMTool(name string, args ...string) error {
	// Try to find the tool in PATH first (most portable)
	if _, err := exec.LookPath(name); err == nil {
		return runTool(name, args...)
	}

	// Fallback to common LLVM paths for macOS Homebrew
	if runtime.GOOS == "darwin" {
		homebrewPath := filepath.Join("/opt/homebrew/opt/llvm/bin", name)
		if _, err := os.Stat(homebrewPath); err == nil {
			return runTool(homebrewPath, args...)
		}
	}

	return fmt.Errorf("llvm tool %s not found in PATH", name)
}

func runTool(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Dir = rootDir
	return cmd.Run()
}

func runToolIn(dir, name string, args ...string) error {
	if dir == "" {
		dir = rootDir
	}
	cmd := exec.Command(name, args...)
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Dir = dir
	return cmd.Run()
}

type writeBuffer struct{ dst *[]byte }

func decodeOut(b *[]byte) *writeBuffer { return &writeBuffer{dst: b} }

func (w *writeBuffer) Write(p []byte) (int, error) {
	*w.dst = append(*w.dst, p...)
	return len(p), nil
}

func main() {
	if len(os.Args) < 2 {
		printUsage()
		os.Exit(1)
	}

	// Handle -root flag before subcommand
	args := os.Args[1:]
	for i, arg := range args {
		if arg == "-root" && i+1 < len(args) {
			rootDir = args[i+1]
			args = append(args[:i], args[i+2:]...)
			break
		}
	}

	if rootDir == "" {
		rootDir = repoRootDir()
	}
	if rootDir == "" {
		wd, err := os.Getwd()
		if err != nil {
			panic(err)
		}
		rootDir = wd
	}
	if wd, err := os.Getwd(); err == nil {
		workDir = wd
	}

	cmd := args[0]
	cmdArgs := args[1:]

	switch cmd {
	case "build":
		if len(cmdArgs) < 1 {
			fmt.Fprintln(os.Stderr, "Usage: aevix build <file.aev> [-v] [--stage <name>]")
			os.Exit(1)
		}
		src, verbose, only := parseBuildFlags(cmdArgs)
		if _, err := buildProject(src, verbose, only); err != nil {
			fmt.Println("❌ Build failed:", err)
			os.Exit(1)
		}
	case "run":
		if len(cmdArgs) < 1 {
			fmt.Fprintln(os.Stderr, "Usage: aevix run <file.aev> [-v] [args...]")
			os.Exit(1)
		}
		src, verbose, runArgs := parseRunFlags(cmdArgs)
		prog, err := buildProject(src, verbose, "")
		if err != nil {
			fmt.Println("❌ Build failed:", err)
			os.Exit(1)
		}
		if err := runToolIn(workDir, prog, runArgs...); err != nil {
			if ee, ok := err.(*exec.ExitError); ok {
				os.Exit(ee.ExitCode())
			}
			fmt.Println("❌ Run failed:", err)
			os.Exit(1)
		}
	case "test":
		python := venvPython()
		if _, err := os.Stat(python); err != nil {
			fmt.Fprintln(os.Stderr, "venv not found. Please run `python bootstrap.py` first")
			os.Exit(1)
		}
		args := []string{"-m", "pytest", filepath.Join("frontend", "tests"), "-q"}
		args = append(args, cmdArgs...)
		if err := runTool(python, args...); err != nil {
			fmt.Println("❌ Tests failed:", err)
			os.Exit(1)
		}
	case "clean":
		os.RemoveAll(buildRoot())
		fmt.Println("🧹 Cleaned build artifacts")
	default:
		fmt.Fprintf(os.Stderr, "Unknown command: %s\n\n", cmd)
		printUsage()
		os.Exit(1)
	}
}

// parseBuildFlags extracts the source file, verbose flag and optional
// --stage name from `aevix build` arguments.
func parseBuildFlags(args []string) (src string, verbose bool, only string) {
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-v":
			verbose = true
		case "--stage":
			if i+1 < len(args) {
				only = args[i+1]
				i++
			}
		default:
			if src == "" {
				src = args[i]
			}
		}
	}
	return src, verbose, only
}

// parseRunFlags extracts the source file and verbose flag from `aevix run`,
// returning everything else as program arguments.
func parseRunFlags(args []string) (src string, verbose bool, runArgs []string) {
	for i := 0; i < len(args); i++ {
		if args[i] == "-v" {
			verbose = true
			continue
		}
		if src == "" {
			src = args[i]
		} else {
			runArgs = append(runArgs, args[i])
		}
	}
	return src, verbose, runArgs
}

func printUsage() {
	fmt.Fprintln(os.Stderr, "Usage: aevix <command> [arguments]")
	fmt.Fprintln(os.Stderr)
	fmt.Fprintln(os.Stderr, "Commands:")
	fmt.Fprintln(os.Stderr, "  build <file.aev> [-v] [--stage <name>]  Compile a .aev file into an executable")
	fmt.Fprintln(os.Stderr, "  run <file.aev> [-v] [args...]    Build, then run with the given program arguments")
	fmt.Fprintln(os.Stderr, "  test [pytest args]    Run the full backend integration test suite")
	fmt.Fprintln(os.Stderr, "  clean                 Remove generated build artifacts")
}