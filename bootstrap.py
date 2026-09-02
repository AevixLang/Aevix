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

def run(cmd, shell=False):
    try:
        subprocess.run(cmd, check=True, shell=shell)
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

    run([str(python_exe), "-m", "pip", "install", "lark"])
    print_status("Python environment ready.")

def setup_backend():
    print_status("Building C++ backend...")
    build_dir = Path("backend/build")
    build_dir.mkdir(parents=True, exist_ok=True)

    # CMake configuration
    # We use -DCMAKE_BUILD_TYPE=Release for speed
    cmake_cmd = ["cmake", "..", "-DCMAKE_BUILD_TYPE=Release"]

    # Run cmake inside the build directory
    os.chdir(build_dir)
    if not run(cmake_cmd):
        print_error("CMake configuration failed. Make sure LLVM is installed.")
        sys.exit(1)

    # Build using cmake --build
    if not run(["cmake", "--build", "."]):
        print_error("Backend build failed.")
        sys.exit(1)

    os.chdir("../../")
    print_status("C++ backend built successfully.")

def setup_tools():
    print_status("Building Go CLI tools...")
    # Build the aevix CLI
    if not run(["go", "build", "-o", "aevix", "tools/cmd/aevix/main.go"]):
        print_error("Go build failed. Make sure Go is installed.")
        sys.exit(1)
    print_status("Go CLI built successfully (executable: ./aevix).")

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

    # 2. Setup phases
    setup_python()
    setup_backend()
    setup_tools()

    print("\n✅ Everything is ready!")
    print("You can now use the CLI: ./aevix build <file.aev>")
    if platform.system() == "Windows":
        print("On Windows, use: aevix.exe build <file.aev>")

if __name__ == "__main__":
    main()
