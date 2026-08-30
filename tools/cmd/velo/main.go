package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

const (
	llvmBin   = "/opt/homebrew/opt/llvm/bin"
	frontend  = "frontend"
	backendExe = "backend/build/velo-backend"
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
	// 1. Python: .velo -> ast.json
	python := filepath.Join(rootDir, "venv", "bin", "python")
	if _, err := os.Stat(python); err != nil {
		fmt.Println("❌ venv not found. Run `make setup` first.")
		return err
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
		return err
	}

	// 2. Backend: ast.json -> LLVM IR (stdout)
	irOut := filepath.Join(rootDir, "output.ll")
	backend := filepath.Join(rootDir, backendExe)
	cmd = exec.Command(backend, astPath)
	var ir []byte
	cmd.Dir = rootDir
	cmd.Stdout = decodeOut(&ir)
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return err
	}
	if err := os.WriteFile(irOut, ir, 0o644); err != nil {
		return err
	}

	// 3. llc: IR -> object file
	objOut := filepath.Join(rootDir, "output.o")
	if err := runLLVMTool("llc", "-filetype=obj", irOut, "-o", objOut); err != nil {
		return err
	}

	// 4. clang: object -> executable
	progOut := filepath.Join(rootDir, "program")
	if err := runTool("clang", objOut, "-o", progOut); err != nil {
		return err
	}

	fmt.Println("✅ Build complete ->", progOut)
	return nil
}

func runLLVMTool(name string, args ...string) error {
	return runTool(filepath.Join(llvmBin, name), args...)
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
	buildFlag := flag.Bool("build", false, "Compile a .velo file into an executable")
	runFlag := flag.Bool("run", false, "Build and run the program")
	cleanFlag := flag.Bool("clean", false, "Remove generated artifacts")
	rootFlag := flag.String("root", "", "Project root directory (defaults to current dir)")
	flag.Usage = func() {
		fmt.Fprintln(os.Stderr, "Usage: velo [flags] <file.velo>")
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
		runTool(filepath.Join(rootDir, "program"))
		return
	}

	flag.Usage()
	os.Exit(1)
}
