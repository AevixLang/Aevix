import os
import subprocess
import sys
import platform
import shutil
from pathlib import Path

def print_status(msg):
    print(f"🚀 {msg}")

def print_error(msg):
    print(f"❌ {msg}")

def run(cmd, shell=False, cwd=None):
    try:
        subprocess.run(cmd, check=True, shell=shell, cwd=cwd)
        return True
    except subprocess.CalledProcessError:
        return False

def setup_python():
    print_status("Setting up Python environment...")
    venv_dir = Path("venv")
    if not venv_dir.exists():
        run([sys.executable, "-m", "venv", "venv"])

    # Determine python executable in venv
    if platform.system() == "Windows":
        python_exe = venv_dir / "Scripts" / "python.exe"
    else:
        python_exe = venv_dir / "bin" / "python"

    run([str(python_exe), "-m", "pip", "install", "-r", "frontend/requirements.txt"])
    print_status("Python environment ready.")

def setup_backend():
    print_status("Building C++ backend...")
    build_dir = Path("backend/build")
    cache_file = build_dir / "CMakeCache.txt"

    # If cache exists, check if it was generated from a different source directory
    if cache_file.exists():
        try:
            with open(cache_file) as f:
                for line in f:
                    if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
                        cached_dir = line.split("=", 1)[1].strip()
                        current_dir = str(Path("backend").resolve())
                        if cached_dir != current_dir:
                            print_status("Stale CMake cache detected, cleaning build directory...")
                            shutil.rmtree(build_dir)
                        break
        except Exception:
            pass

    build_dir.mkdir(parents=True, exist_ok=True)

    cmake_cmd = ["cmake", "..", "-DCMAKE_BUILD_TYPE=Release"]

    # Run cmake inside the build directory
    if not run(cmake_cmd, cwd=str(build_dir)):
        print_error("CMake configuration failed. Make sure LLVM is installed.")
        sys.exit(1)

    # Build using cmake --build
    if not run(["cmake", "--build", "."], cwd=str(build_dir)):
        print_error("Backend build failed.")
        sys.exit(1)

    print_status("C++ backend built successfully.")

def setup_tools():
    print_status("Building Go CLI tools...")
    # Build the aevix CLI
    if not run(["go", "build", "-o", "aevix", "tools/cmd/aevix/main.go"]):
        print_error("Go build failed. Make sure Go is installed.")
        sys.exit(1)
    print_status("Go CLI built successfully (executable: ./aevix).")
    install_cli_symlink()


def path_list():
    return [p for p in os.environ.get("PATH", "").split(os.pathsep) if p]


def install_cli_symlink():
    """Symlink the built CLI into ~/.local/bin so `aevix` works from anywhere.
    The CLI locates the compiler repo from its own path, so the symlink is safe."""
    if platform.system() == "Windows":
        print_status("Skipping PATH install on Windows (add %AEVIX_HOME% manually).")
        return
    home = Path.home()
    bin_dir = home / ".local" / "bin"
    try:
        bin_dir.mkdir(parents=True, exist_ok=True)
        link = bin_dir / "aevix"
        target = Path("aevix").resolve()
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(target)
    except OSError as e:
        print_error(f"Could not symlink aevix into {bin_dir}: {e}")
        return
    if str(bin_dir) in path_list():
        print_status(f"Global CLI installed: {link} -> {target}")
    else:
        print_status(f"Global CLI installed at {link}")
        print(f"ℹ️  Add {bin_dir} to your PATH (export PATH=\"$HOME/.local/bin:$PATH\").")


def main():
    print("=== Aevix Project Bootstrap ===")

    # 1. Check prerequisites
    if not shutil.which("cmake"):
        print_error("cmake not found in PATH.")
        sys.exit(1)
    if not shutil.which("go"):
        print_error("go not found in PATH.")
        sys.exit(1)
    if not shutil.which("clang") and not shutil.which("gcc"):
        print_error("No C++ compiler (clang/gcc) found.")
        sys.exit(1)
    if not shutil.which("llc"):
        # Check common Homebrew/system LLVM paths
        llc_found = False
        for prefix in ["/opt/homebrew/opt/llvm/bin", "/usr/local/opt/llvm/bin", "/usr/lib/llvm/bin"]:
            if (Path(prefix) / "llc").exists():
                llc_found = True
                break
        if not llc_found:
            print_error("llc not found in PATH. Install LLVM and ensure llc is accessible.")
            sys.exit(1)

    # 2. Setup phases
    setup_python()
    setup_backend()
    setup_tools()

    print("\n✅ Everything is ready!")
    print("You can now use the CLI: ./aevix build <file.aev>")
    print("It is also on your PATH, so `aevix` works from any directory.")
    if platform.system() == "Windows":
        print("On Windows, use: aevix.exe build <file.aev>")

if __name__ == "__main__":
    main()
