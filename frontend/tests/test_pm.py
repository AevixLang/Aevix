"""
Package-manager (aevix-pm) integration tests.

aevix-pm is the write-only half of the package contract: it authors aevix.toml
and aevix.lock, which both frontends then consume read-only. These tests build
the Go binary and drive its commands end-to-end, then check the generated lock
is what the compiler resolves. Go is required; the C++-consumer checks are
skipped unless the frontend binary is built.
"""
import json
import os
import shutil
import subprocess
import tempfile

import pytest

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
CPP_FE = os.path.join(ROOT, "backend", "build", "aevix-frontend")

pytestmark = pytest.mark.skipif(
    shutil.which("go") is None, reason="go toolchain not available"
)


@pytest.fixture(scope="module")
def pm_bin():
    """Builds aevix-pm once into a temp dir and yields its path."""
    out = os.path.join(tempfile.mkdtemp(), "aevix-pm")
    tools_dir = os.path.join(ROOT, "tools")
    proc = subprocess.run(["go", "build", "-o", out, "./cmd/aevix-pm"],
                          cwd=tools_dir, capture_output=True, text=True)
    assert proc.returncode == 0, f"go build aevix-pm failed:\n{proc.stderr}"
    return out


def run_pm(pm, cwd, *args):
    return subprocess.run([pm, *args], cwd=cwd, capture_output=True, text=True)


def make_pkg(dirpath, name, version):
    """Creates a minimal dependency package with lib.aev + aevix.toml."""
    os.makedirs(dirpath, exist_ok=True)
    with open(os.path.join(dirpath, "lib.aev"), "w") as f:
        f.write("func dep_val(): int { return 42; }\n")
    with open(os.path.join(dirpath, "aevix.toml"), "w") as f:
        f.write(f'[package]\nname = "{name}"\nversion = "{version}"\n')


def test_init_writes_manifest_and_lock(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        p = run_pm(pm_bin, proj, "init", "app")
        assert p.returncode == 0, p.stderr
        assert os.path.exists(os.path.join(proj, "aevix.toml"))
        with open(os.path.join(proj, "aevix.lock")) as f:
            lock = json.load(f)
        assert lock["name"] == "app"
        assert lock["dependencies"] == {}


def test_add_detects_version_and_pins_lock(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        make_pkg(os.path.join(proj, "calc"), "calc", "1.2.3")
        p = run_pm(pm_bin, proj, "add", "calc", "--path", "./calc")
        assert p.returncode == 0, p.stderr
        with open(os.path.join(proj, "aevix.lock")) as f:
            lock = json.load(f)
        dep = lock["dependencies"]["calc"]
        assert dep["version"] == "1.2.3"
        assert dep["path"] == "calc"


def test_add_explicit_version_overrides(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        make_pkg(os.path.join(proj, "calc"), "calc", "1.2.3")
        run_pm(pm_bin, proj, "add", "calc", "--path", "./calc", "--version", "9.9.9")
        with open(os.path.join(proj, "aevix.lock")) as f:
            lock = json.load(f)
        assert lock["dependencies"]["calc"]["version"] == "9.9.9"


def test_add_missing_path_fails(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        p = run_pm(pm_bin, proj, "add", "calc")
        assert p.returncode != 0


def test_add_dir_without_source_fails(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        os.makedirs(os.path.join(proj, "empty"))
        p = run_pm(pm_bin, proj, "add", "calc", "--path", "./empty")
        assert p.returncode != 0


def test_remove_clears_dependency(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        make_pkg(os.path.join(proj, "calc"), "calc", "1.0.0")
        run_pm(pm_bin, proj, "add", "calc", "--path", "./calc")
        p = run_pm(pm_bin, proj, "remove", "calc")
        assert p.returncode == 0, p.stderr
        with open(os.path.join(proj, "aevix.lock")) as f:
            assert json.load(f)["dependencies"] == {}


def test_lock_output_is_deterministic(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        make_pkg(os.path.join(proj, "b"), "b", "1.0.0")
        make_pkg(os.path.join(proj, "a"), "a", "2.0.0")
        run_pm(pm_bin, proj, "add", "b", "--path", "./b")
        run_pm(pm_bin, proj, "add", "a", "--path", "./a")
        with open(os.path.join(proj, "aevix.lock"), "rb") as f:
            first = f.read()
        # Re-add in a fresh project in the opposite order; sorted output must match.
        with tempfile.TemporaryDirectory() as proj2:
            run_pm(pm_bin, proj2, "init", "app")
            make_pkg(os.path.join(proj2, "a"), "a", "2.0.0")
            make_pkg(os.path.join(proj2, "b"), "b", "1.0.0")
            run_pm(pm_bin, proj2, "add", "a", "--path", "./a")
            run_pm(pm_bin, proj2, "add", "b", "--path", "./b")
            with open(os.path.join(proj2, "aevix.lock"), "rb") as f:
                second = f.read()
        assert first == second


@pytest.mark.skipif(not os.path.exists(CPP_FE), reason="c++ frontend not built")
def test_pm_output_resolves_in_compiler(pm_bin):
    """End to end: aevix-pm authors the lock, the C++ frontend consumes it."""
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        make_pkg(os.path.join(proj, "calc"), "calc", "1.0.0")
        with open(os.path.join(proj, "calc", "lib.aev"), "w") as f:
            f.write("func add(a: int, b: int): int { return a + b; }\n")
        assert run_pm(pm_bin, proj, "add", "calc", "--path", "./calc").returncode == 0
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write('import "calc:";\nprint add(2, 3);\n')
        out = os.path.join(proj, "ast.json")
        proc = subprocess.run([CPP_FE, os.path.join(proj, "main.aev"), "-o", out],
                              cwd=proj, capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
        with open(out) as f:
            assert "add" in f.read()


# --- git dependencies (0.3.9) ---

def make_git_pkg(root, name, versions):
    """Create a git repo with one commit+tag per version."""
    if shutil.which("git") is None:
        pytest.skip("git toolchain not available")
    path = os.path.join(root, name)
    os.makedirs(path)
    subprocess.run(["git", "init", "-q"], cwd=path, check=True)
    subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=path, check=True)
    subprocess.run(["git", "config", "user.name", "Aevix Test"], cwd=path, check=True)
    for v in versions:
        with open(os.path.join(path, "lib.aev"), "w") as f:
            f.write("func dep_val(): int { return 42; }\n")
        with open(os.path.join(path, "aevix.toml"), "w") as f:
            f.write(f'[package]\nname = "{name}"\nversion = "{v}"\n')
        subprocess.run(["git", "add", "."], cwd=path, check=True)
        subprocess.run(["git", "commit", "-q", "-m", f"release {v}"], cwd=path, check=True)
        subprocess.run(["git", "tag", f"v{v}"], cwd=path, check=True)
    return path


def test_add_git_picks_newest_tag(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        repo = make_git_pkg(proj, "remotedep", ["1.0.0", "1.1.0"])
        p = run_pm(pm_bin, proj, "add", "remotedep", "--git", repo, "--version", "^1.0")
        assert p.returncode == 0, p.stderr
        with open(os.path.join(proj, "aevix.lock")) as f:
            dep = json.load(f)["dependencies"]["remotedep"]
        assert dep["version"] == "1.1.0"
        assert dep["path"] == "lib/remotedep"
        assert dep["source"] == "git+" + repo
        assert len(dep["rev"]) == 40
        assert os.path.exists(os.path.join(proj, "lib", "remotedep", "lib.aev"))


def test_install_restores_git_dep(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        repo = make_git_pkg(proj, "remotedep", ["1.0.0"])
        assert run_pm(pm_bin, proj, "add", "remotedep", "--git", repo).returncode == 0
        shutil.rmtree(os.path.join(proj, "lib", "remotedep"))
        p = run_pm(pm_bin, proj, "install")
        assert p.returncode == 0, p.stderr
        assert os.path.exists(os.path.join(proj, "lib", "remotedep", "lib.aev"))


@pytest.mark.skipif(not os.path.exists(CPP_FE), reason="c++ frontend not built")
def test_git_dep_resolves_in_compiler(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        repo = make_git_pkg(proj, "remotedep", ["1.0.0"])
        assert run_pm(pm_bin, proj, "add", "remotedep", "--git", repo).returncode == 0
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write('import "remotedep:";\nprint dep_val();\n')
        out = os.path.join(proj, "ast.json")
        proc = subprocess.run([CPP_FE, os.path.join(proj, "main.aev"), "-o", out],
                              cwd=proj, capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
        with open(out) as f:
            assert "dep_val" in f.read()


# --- vendor hardening (0.3.9) ---

def test_vendor_skips_git_and_rejects_symlinks(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        dep = os.path.join(proj, "calc")
        make_pkg(dep, "calc", "1.0.0")
        os.makedirs(os.path.join(dep, ".git"))
        with open(os.path.join(dep, ".git", "config"), "w") as f:
            f.write("secret\n")
        assert run_pm(pm_bin, proj, "add", "calc", "--path", "./calc").returncode == 0

        link = os.path.join(dep, "alias.aev")
        try:
            os.symlink("lib.aev", link)
        except OSError:
            pytest.skip("symlinks unavailable")
        p = run_pm(pm_bin, proj, "vendor")
        assert p.returncode != 0, "vendor must refuse a symlink"
        os.unlink(link)

        p = run_pm(pm_bin, proj, "vendor")
        assert p.returncode == 0, p.stderr
        vendored = os.path.join(proj, "lib", "vendor", "calc")
        assert os.path.exists(os.path.join(vendored, "lib.aev"))
        assert not os.path.exists(os.path.join(vendored, ".git"))

        # A second run must not recurse into or delete its own output.
        p = run_pm(pm_bin, proj, "vendor")
        assert p.returncode == 0, p.stderr
        assert os.path.exists(os.path.join(vendored, "lib.aev"))


def add_git_version(repo, name, version):
    with open(os.path.join(repo, "aevix.toml"), "w") as f:
        f.write(f'[package]\nname = "{name}"\nversion = "{version}"\n')
    subprocess.run(["git", "add", "."], cwd=repo, check=True)
    subprocess.run(["git", "commit", "-q", "-m", f"release {version}"], cwd=repo, check=True)
    subprocess.run(["git", "tag", f"v{version}"], cwd=repo, check=True)


def test_update_git_picks_newer_tag(pm_bin):
    with tempfile.TemporaryDirectory() as proj:
        run_pm(pm_bin, proj, "init", "app")
        repo = make_git_pkg(proj, "remotedep", ["1.0.0"])
        assert run_pm(pm_bin, proj, "add", "remotedep", "--git", repo, "--version", "^1.0").returncode == 0
        add_git_version(repo, "remotedep", "1.1.0")
        p = run_pm(pm_bin, proj, "update", "remotedep")
        assert p.returncode == 0, p.stderr
        with open(os.path.join(proj, "aevix.lock")) as f:
            dep = json.load(f)["dependencies"]["remotedep"]
        assert dep["version"] == "1.1.0"
