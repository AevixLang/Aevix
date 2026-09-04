package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

const (
	frontend  = "frontend"
)

var rootDir string

func runCmd(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Dir = rootDir
	return cmd.Run()
}

func buildProject(srcFile string) error {
	// 1. Python: .aev -> ast.json
	var python string
	if runtime.GOOS == "windows" {
		python = filepath.Join(rootDir, "venv", "Scripts", "python.exe")
	} else {
		python = filepath.Join(rootDir, "venv", "bin", "python")
	}

	if _, err := os.Stat(python); err != nil {
		return fmt.Errorf("venv not found. Please run `python bootstrap.py` first")
	}

	astPath := filepath.Join(rootDir, "frontend", "ast.json")
	var absSrc string
	if filepath.IsAbs(srcFile) {
		absSrc = srcFile
	} else {
		absSrc = filepath.Join(rootDir, srcFile)
	}

	cmd := exec.Command(python, "-m", "src.main", absSrc)
	cmd.Dir = filepath.Join(rootDir, frontend)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("frontend parsing failed: %w", err)
	}

	// 2. Backend: ast.json -> LLVM IR (stdout)
	backendExe := filepath.Join(rootDir, "backend", "build", "aevix-backend")
	if runtime.GOOS == "windows" {
		backendExe += ".exe"
	}

	irOut := filepath.Join(rootDir, "output.ll")
	cmd = exec.Command(backendExe, astPath)
	var ir []byte
	cmd.Dir = rootDir
	cmd.Stdout = decodeOut(&ir)
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("backend generation failed: %w", err)
	}
	if err := os.WriteFile(irOut, ir, 0o644); err != nil {
		return fmt.Errorf("failed to write IR: %w", err)
	}

	// 3. llc: IR -> object file
	objOut := filepath.Join(rootDir, "output.o")
	if err := runLLVMTool("llc", "-filetype=obj", irOut, "-o", objOut); err != nil {
		return fmt.Errorf("llc failed: %w", err)
	}

	// 4. clang: objects -> executable (llc object + arena runtime)
	progOut := filepath.Join(rootDir, "program")
	if runtime.GOOS == "windows" {
		progOut += ".exe"
	}
	runtimeObj := filepath.Join(rootDir, "backend", "build", "runtime.o")
	if err := runTool("clang", objOut, runtimeObj, "-o", progOut); err != nil {
		return fmt.Errorf("clang failed: %w", err)
	}

	fmt.Println("✅ Build complete ->", progOut)
	return nil
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
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Dir = rootDir
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
		wd, err := os.Getwd()
		if err != nil {
			panic(err)
		}
		rootDir = wd
	}

	cmd := args[0]
	cmdArgs := args[1:]

	switch cmd {
	case "build":
		if len(cmdArgs) < 1 {
			fmt.Fprintln(os.Stderr, "Usage: aevix build <file.aev>")
			os.Exit(1)
		}
		if err := buildProject(cmdArgs[0]); err != nil {
			fmt.Println("❌ Build failed:", err)
			os.Exit(1)
		}
	case "run":
		if len(cmdArgs) < 1 {
			fmt.Fprintln(os.Stderr, "Usage: aevix run <file.aev> [args...]")
			os.Exit(1)
		}
		if err := buildProject(cmdArgs[0]); err != nil {
			fmt.Println("❌ Build failed:", err)
			os.Exit(1)
		}
		prog := filepath.Join(rootDir, "program")
		if runtime.GOOS == "windows" {
			prog += ".exe"
		}
		if err := runTool(prog, cmdArgs[1:]...); err != nil {
			if ee, ok := err.(*exec.ExitError); ok {
				os.Exit(ee.ExitCode())
			}
			fmt.Println("❌ Run failed:", err)
			os.Exit(1)
		}
	case "test":
		python := filepath.Join(rootDir, "venv", "bin", "python")
		if runtime.GOOS == "windows" {
			python = filepath.Join(rootDir, "venv", "Scripts", "python.exe")
		}
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
		os.RemoveAll(filepath.Join(rootDir, "output.ll"))
		os.RemoveAll(filepath.Join(rootDir, "output.o"))
		os.RemoveAll(filepath.Join(rootDir, "program"))
		if runtime.GOOS == "windows" {
			os.RemoveAll(filepath.Join(rootDir, "program.exe"))
		}
		fmt.Println("🧹 Cleaned artifacts")
	default:
		fmt.Fprintf(os.Stderr, "Unknown command: %s\n\n", cmd)
		printUsage()
		os.Exit(1)
	}
}

func printUsage() {
	fmt.Fprintln(os.Stderr, "Usage: aevix <command> [arguments]")
	fmt.Fprintln(os.Stderr)
	fmt.Fprintln(os.Stderr, "Commands:")
	fmt.Fprintln(os.Stderr, "  build <file.aev>    Compile a .aev file into an executable")
	fmt.Fprintln(os.Stderr, "  run <file.aev> [args...]  Build, then run with the given program arguments")
	fmt.Fprintln(os.Stderr, "  test [pytest args]  Run the full backend integration test suite")
	fmt.Fprintln(os.Stderr, "  clean               Remove generated artifacts")
}
