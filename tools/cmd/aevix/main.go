package main

import (
	"flag"
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

	// 4. clang: object -> executable
	progOut := filepath.Join(rootDir, "program")
	if runtime.GOOS == "windows" {
		progOut += ".exe"
	}
	if err := runTool("clang", objOut, "-o", progOut); err != nil {
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
	buildFlag := flag.Bool("build", false, "Compile a .aev file into an executable")
	runFlag := flag.Bool("run", false, "Build and run the program")
	cleanFlag := flag.Bool("clean", false, "Remove generated artifacts")
	rootFlag := flag.String("root", "", "Project root directory (defaults to current dir)")
	flag.Usage = func() {
		fmt.Fprintln(os.Stderr, "Usage: aevix [flags] <file.aev>")
		flag.PrintDefaults()
	}
	flag.Parse()

	rootDir = *rootFlag
	if rootDir == "" {
		wd, err := os.Getwd()
		if err != nil {
			panic(err)
		}
		rootDir = wd
	}

	if *cleanFlag {
		os.RemoveAll(filepath.Join(rootDir, "output.ll"))
		os.RemoveAll(filepath.Join(rootDir, "output.o"))
		os.RemoveAll(filepath.Join(rootDir, "program"))
		if runtime.GOOS == "windows" {
			os.RemoveAll(filepath.Join(rootDir, "program.exe"))
		}
		fmt.Println("🧹 Cleaned artifacts")
		return
	}

	src := flag.Arg(0)

	if *buildFlag {
		if src == "" {
			flag.Usage()
			os.Exit(1)
		}
		if err := buildProject(src); err != nil {
			fmt.Println("❌ Build failed:", err)
			os.Exit(1)
		}
		return
	}

	if *runFlag {
		if src == "" {
			flag.Usage()
			os.Exit(1)
		}
		if err := buildProject(src); err != nil {
			fmt.Println("❌ Build failed:", err)
			os.Exit(1)
		}

		prog := filepath.Join(rootDir, "program")
		if runtime.GOOS == "windows" {
			prog += ".exe"
		}
		runTool(prog)
		return
	}

	flag.Usage()
	os.Exit(1)
}
