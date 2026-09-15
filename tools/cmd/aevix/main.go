package main

import (
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
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

// stageResult records how long one pipeline stage took.
type stageResult struct {
	name string
	dur  time.Duration
}

// runStages executes the given stages in order, timing each one. When
// `only` is non-empty, exactly that named stage is run (inputs are expected
// to already exist on disk). `verbose` prints per-stage timings to stdout.
// The collected timings are always returned so callers (build -v, bench)
// can present them however they like.
func runStages(stages []stage, verbose bool, only string) ([]stageResult, error) {
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
			return nil, fmt.Errorf("unknown stage %q (expected one of: %s)",
				only, stageNames(stages))
		}
	}
	res := make([]stageResult, 0, len(selected))
	for _, s := range selected {
		start := time.Now()
		if err := s.fn(); err != nil {
			return nil, fmt.Errorf("%s stage failed: %w", s.name, err)
		}
		r := stageResult{s.name, time.Since(start).Round(time.Millisecond)}
		res = append(res, r)
		if verbose {
			fmt.Printf("%s %s\n", r.name, r.dur)
		}
	}
	return res, nil
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
	semaExe := filepath.Join(rootDir, "backend", "build", "aevix-sema")
	if runtime.GOOS == "windows" {
		backendExe += ".exe"
		semaExe += ".exe"
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

	stages := makeStages(absSrc, astPath, irPath, objPath, progOut, python, backendExe, semaExe, false)

	if _, err := runStages(stages, verbose, only); err != nil {
		return "", err
	}

	fmt.Println("✅ Build complete ->", progOut)
	return progOut, nil
}

// benchSource runs the whole pipeline for `src` silently and returns the
// per-stage timings. Used by `aevix bench`.
func benchSource(src, python, backendExe, semaExe string) ([]stageResult, error) {
	absSrc := src
	if !filepath.IsAbs(absSrc) {
		absSrc = filepath.Join(workDir, src)
	}
	buildDir := buildDirFor(absSrc)
	if err := os.MkdirAll(buildDir, 0o755); err != nil {
		return nil, err
	}
	astPath := filepath.Join(buildDir, "ast.json")
	irPath := filepath.Join(buildDir, "output.ll")
	objPath := filepath.Join(buildDir, "output.o")
	progOut := filepath.Join(buildDir, "bench-program")
	stages := makeStages(absSrc, astPath, irPath, objPath, progOut, python, backendExe, semaExe, true)
	return runStages(stages, false, "")
}

// syntheticBenchSource generates a large flat .aev program to make the
// compile-time pipeline (parse/backend/codegen) measurable.
func syntheticBenchSource() string {
	var b strings.Builder
	b.WriteString("// synthetic bench input: flat arithmetic chain of 2000 vars\n")
	b.WriteString("hot {\n")
	b.WriteString("    let v0 = 1;\n")
	for i := 1; i < 2000; i++ {
		if i%2 == 0 {
			fmt.Fprintf(&b, "    let v%d = v%d + %d;\n", i, i-1, i)
		} else {
			fmt.Fprintf(&b, "    let v%d = v%d * 2;\n", i, i-1)
		}
	}
	fmt.Fprintf(&b, "    print v1999;\n")
	b.WriteString("}\n")
	return b.String()
}

func timingLine(res []stageResult) string {
	parts := make([]string, 0, len(res))
	for _, r := range res {
		parts = append(parts, fmt.Sprintf("%s %s", r.name, r.dur))
	}
	return joinStrings(parts, " | ")
}

// saveBenchRow appends one dated row of build timings to notes/benchmarks.md.
func saveBenchRow(src string, res []stageResult) {
	path := filepath.Join(rootDir, "notes", "benchmarks.md")
	header := "# Aevix build benchmarks\n\nBaseline recorded by `aevix bench`.\n\n| date | source | parse | sema | backend | llc | link |\n|---|---|---|---|---|---|---|\n"
	var existing string
	if data, err := os.ReadFile(path); err == nil {
		existing = string(data)
		if idx := strings.Index(existing, "\n| date |"); idx >= 0 {
			existing = existing[idx+1:]
		} else {
			existing = ""
		}
	}
	var parts []string
	for _, r := range res {
		parts = append(parts, r.dur.String())
	}
	if existing == "" {
		existing = header
	}
	row := fmt.Sprintf("| %s | %s | %s |\n", time.Now().Format("2006-01-02 15:04"), src, joinStrings(parts, " | "))
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err == nil {
		_ = os.WriteFile(path, []byte(existing+row), 0o644)
	}
}

// bench runs the pipeline on a small and a large source, prints per-stage
// timings and records them in notes/benchmarks.md (Git-ignored, local).
func bench() {
	python := venvPython()
	if _, err := os.Stat(python); err != nil {
		fmt.Println("❌ bench failed: venv not found. Run `python bootstrap.py` first")
		os.Exit(1)
	}
	backendExe := filepath.Join(rootDir, "backend", "build", "aevix-backend")
	semaExe := filepath.Join(rootDir, "backend", "build", "aevix-sema")
	if runtime.GOOS == "windows" {
		backendExe += ".exe"
		semaExe += ".exe"
	}

	smallSrc := filepath.Join(rootDir, "examples", "test.aev")
	if _, err := os.Stat(smallSrc); err != nil {
		fmt.Println("❌ bench failed: examples/test.aev not found")
		os.Exit(1)
	}
	synth := filepath.Join(buildRoot(), "_bench_synthetic.aev")
	if err := os.MkdirAll(buildRoot(), 0o755); err != nil {
		fmt.Println("❌ bench failed:", err)
		os.Exit(1)
	}
	if err := os.WriteFile(synth, []byte(syntheticBenchSource()), 0o644); err != nil {
		fmt.Println("❌ bench failed:", err)
		os.Exit(1)
	}

	for _, spec := range []struct{ label, src string }{
		{"examples/test.aev", smallSrc},
		{"synthetic (2000 vars)", synth},
	} {
		res, err := benchSource(spec.src, python, backendExe, semaExe)
		if err != nil {
			fmt.Println("❌ bench failed:", err)
			os.Exit(1)
		}
		fmt.Printf("%-26s %s\n", spec.label, timingLine(res))
		saveBenchRow(spec.label, res)
	}
	fmt.Println("📈 Baseline saved to notes/benchmarks.md")
}

// makeStages returns the build pipeline stages for a source file.
// `quiet` silences chatty stage output (used by bench).
func makeStages(absSrc, astPath, irPath, objPath, progOut, python, backendExe, semaExe string, quiet bool) []stage {
	return []stage{
		{"parse", func() error {
			// Python: .aev -> ast.json (written straight into the build dir)
			return runFrontend(python, absSrc, astPath, quiet)
		}},
		{"sema", func() error {
			// Sema: ast.json -> type/scope/escape diagnostics (validates in place)
			return runTool(semaExe, astPath)
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
}

func venvPython() string {
	if runtime.GOOS == "windows" {
		return filepath.Join(rootDir, "venv", "Scripts", "python.exe")
	}
	return filepath.Join(rootDir, "venv", "bin", "python")
}

func runFrontend(python, src, out string, quiet bool) error {
	cmd := exec.Command(python, "-m", "src.main", src, "-o", out)
	cmd.Dir = filepath.Join(rootDir, "frontend")
	if quiet {
		cmd.Stdout = io.Discard
	} else {
		cmd.Stdout = os.Stdout
	}
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
	case "bench":
		bench()
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
	fmt.Fprintln(os.Stderr, "  bench                 Time parse/backend/llc/link and save a baseline to notes/benchmarks.md")
	fmt.Fprintln(os.Stderr, "  clean                 Remove generated build artifacts")
}
