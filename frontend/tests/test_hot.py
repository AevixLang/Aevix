"""
Hot region tests: `hot { }` must drive the L1 optimizations it promises.

Three layers are asserted:
  - runtime: hot loops produce the same values as cold ones (all runtime tests
    run through the real opt -O2 stage);
  - IR: the optimizations themselves are present inside hot and absent
    outside it (loop vectorize metadata, the hot function attribute,
    64-byte aligned locals, allocation prefetch);
  - optimizer: hot IR actually vectorizes after opt — software prefetch is
    deliberately absent from loop bodies because any call in a loop body
    makes LLVM's vectorizer reject the loop.
"""
import os
import re
import subprocess
import tempfile

from test_arrays import (
    VENV_PYTHON, FRONTEND_DIR, BACKEND_BIN, OPT, run_and_capture,
)


def emit_ir(code):
    """Run frontend + backend and return the generated LLVM IR."""
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".aev", delete=False, dir=FRONTEND_DIR
    ) as f:
        f.write(code)
        f.flush()
        src = f.name
    workdir = tempfile.mkdtemp()
    ast = os.path.join(workdir, "ast.json")
    try:
        fe = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", src, "-o", ast],
            cwd=FRONTEND_DIR, capture_output=True, text=True,
        )
        assert fe.returncode == 0, fe.stderr
        be = subprocess.run(
            [BACKEND_BIN, ast], cwd=workdir, capture_output=True, text=True,
        )
        assert be.returncode == 0, be.stderr
        return be.stdout
    finally:
        os.unlink(src)


def optimize(ir_text):
    """Run the CLI's opt -O2 stage over backend IR, return optimized IR."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".ll", delete=False) as f:
        f.write(ir_text)
        ir = f.name
    out = ir + ".opt.ll"
    try:
        oc = subprocess.run(
            [OPT, "-passes=default<O2>", "-S", ir, "-o", out],
            capture_output=True, text=True,
        )
        assert oc.returncode == 0, oc.stderr
        with open(out) as fh:
            return fh.read()
    finally:
        os.unlink(ir)
        if os.path.exists(out):
            os.unlink(out)


PREFETCH_CALL = "call void @llvm.prefetch("


# --- runtime semantics: hot must not change results --------------------------

def test_hot_for_sum_over_fixed_array():
    ec, out, err = run_and_capture(
        "let a = [1, 2, 3, 4, 5];\n"
        "hot {\n"
        "    let total = 0;\n"
        "    for (let i = 0; i < len(a); i += 1) {\n"
        "        total = total + a[i];\n"
        "    }\n"
        "    print total;\n"
        "}\n"
    )
    assert ec == 0, err
    assert out == ["15"], out


def test_hot_for_writes_slice():
    ec, out, err = run_and_capture(
        "let a = [10, 20, 30];\n"
        "let s = new int[3];\n"
        "hot {\n"
        "    for (let i = 0; i < 3; i += 1) {\n"
        "        s[i] = a[i] * 2;\n"
        "    }\n"
        "    print s[0];\n"
        "    print s[2];\n"
        "}\n"
    )
    assert ec == 0, err
    assert out == ["20", "60"], out


def test_hot_for_with_step_two_is_correct():
    ec, out, err = run_and_capture(
        "let a = [1, 2, 3, 4, 5, 6];\n"
        "hot {\n"
        "    let total = 0;\n"
        "    for (let i = 0; i < 6; i += 2) {\n"
        "        total = total + a[i];\n"
        "    }\n"
        "    print total;\n"
        "}\n"
    )
    assert ec == 0, err
    assert out == ["9"], out


def test_hot_for_in_fixed_array_and_slice():
    ec, out, err = run_and_capture(
        "let a = [7, 8, 9];\n"
        "let s = new int[2];\n"
        "hot {\n"
        "    s[0] = 100;\n"
        "    s[1] = 200;\n"
        "    for x in a { print x; }\n"
        "    for y in s { print y; }\n"
        "}\n"
    )
    assert ec == 0, err
    assert out == ["7", "8", "9", "100", "200"], out


def test_hot_nested_loops_sum_matrix():
    ec, out, err = run_and_capture(
        "let rows = new int[4];\n"
        "hot {\n"
        "    let total = 0;\n"
        "    for (let i = 0; i < 4; i += 1) {\n"
        "        for (let j = 0; j < 4; j += 1) {\n"
        "            total = total + i * 4 + j;\n"
        "        }\n"
        "    }\n"
        "    print total;\n"
        "}\n"
    )
    assert ec == 0, err
    assert out == ["120"], out


def test_hot_function_sums_and_returns():
    ec, out, err = run_and_capture(
        "func sum(a: int[]) : int {\n"
        "    let t = 0;\n"
        "    hot {\n"
        "        for (let i = 0; i < len(a); i += 1) {\n"
        "            t = t + a[i];\n"
        "        }\n"
        "    }\n"
        "    return t;\n"
        "}\n"
        "let a = [4, 5, 6];\n"
        "print sum(a);\n"
    )
    assert ec == 0, err
    assert out == ["15"], out


def test_hot_long_loop_with_local_does_not_grow_stack():
    # A `let` inside a loop body used to get a fresh stack slot per iteration,
    # so long-running loops exhausted the stack. The slot now lives in the
    # function entry block.
    ec, out, err = run_and_capture(
        "hot {\n"
        "    let i = 0;\n"
        "    let total = 0;\n"
        "    while (i < 3000000) {\n"
        "        let x = i + 1;\n"
        "        total = total + x - i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    print total;\n"
        "}\n"
    )
    assert ec == 0, err
    assert out == ["3000000"], out


# --- IR: the optimizations hot promises are actually emitted -----------------

HOT_LOOP = (
    "let a = [1, 2, 3];\n"
    "hot {\n"
    "    let total = 0;\n"
    "    for (let i = 0; i < len(a); i += 1) {\n"
    "        total = total + a[i];\n"
    "    }\n"
    "    print total;\n"
    "}\n"
)

COLD_LOOP = (
    "let a = [1, 2, 3];\n"
    "let total = 0;\n"
    "for (let i = 0; i < len(a); i += 1) {\n"
    "    total = total + a[i];\n"
    "}\n"
    "print total;\n"
)


def test_hot_loop_emits_no_prefetch():
    # A prefetch call in the loop body makes LLVM reject the whole loop for
    # vectorization, which costs far more than the hint could ever win.
    assert PREFETCH_CALL not in emit_ir(HOT_LOOP)


def test_cold_loop_emits_no_prefetch():
    assert PREFETCH_CALL not in emit_ir(COLD_LOOP)


def test_hot_loop_emits_vectorize_metadata():
    ir = emit_ir(HOT_LOOP)
    assert "llvm.loop.vectorize.enable" in ir
    assert re.search(r"br label %[\w.]+, !llvm.loop !\d+", ir)


def test_cold_loop_emits_no_vectorize_metadata():
    assert "llvm.loop.vectorize" not in emit_ir(COLD_LOOP)


def test_hot_adds_function_attribute():
    ir = emit_ir(HOT_LOOP)
    assert re.search(r"attributes #\d+ = \{[^}]*\bhot\b[^}]*\}", ir), ir


def test_cold_function_has_no_hot_attribute():
    ir = emit_ir(COLD_LOOP)
    assert not re.search(r"attributes #\d+ = \{[^}]*\bhot\b[^}]*\}", ir)


def test_hot_locals_are_cache_line_aligned():
    assert "align 64" in emit_ir(HOT_LOOP)


def test_cold_locals_are_not_cache_line_aligned():
    assert "align 64" not in emit_ir(COLD_LOOP)


def test_hot_write_through_index_emits_no_prefetch():
    ir = emit_ir(
        "let a = [1, 2, 3];\n"
        "hot {\n"
        "    for (let i = 0; i < 3; i += 1) {\n"
        "        a[i] = i;\n"
        "    }\n"
        "}\n"
    )
    assert PREFETCH_CALL not in ir


def test_hot_for_in_emits_no_prefetch():
    ir = emit_ir(
        "let a = [1, 2, 3];\n"
        "hot {\n"
        "    for x in a { print x; }\n"
        "}\n"
    )
    assert PREFETCH_CALL not in ir


def test_cold_for_in_emits_no_prefetch():
    ir = emit_ir(
        "let a = [1, 2, 3];\n"
        "for x in a { print x; }\n"
    )
    assert PREFETCH_CALL not in ir


def test_hot_allocation_emits_prefetch():
    # Arena allocations inside hot are prefetched too (concat, literals, new).
    ir = emit_ir('hot {\n    let s = "ab" + "cd";\n    print s;\n}\n')
    assert PREFETCH_CALL in ir


def test_cold_allocation_emits_no_prefetch():
    ir = emit_ir('let s = "ab" + "cd";\nprint s;\n')
    assert PREFETCH_CALL not in ir


def test_hot_counter_step_two_skips_prefetch():
    # No loop gets software prefetch at all, whatever the step; the +2 counter
    # is kept as a regression case for the counted-loop shape.
    ir = emit_ir(
        "let a = [1, 2, 3, 4];\n"
        "hot {\n"
        "    let total = 0;\n"
        "    for (let i = 0; i < 4; i += 2) {\n"
        "        total = total + a[i];\n"
        "    }\n"
        "    print total;\n"
        "}\n"
    )
    assert PREFETCH_CALL not in ir


# --- optimizer: hot IR actually vectorizes after the opt stage ---------------

# Arena memory cannot be constant-folded, so both loops stay real.
HOT_RUNTIME_LOOP = (
    "let n = 64;\n"
    "let a = new int[n];\n"
    "hot {\n"
    "    let i = 0;\n"
    "    while (i < n) {\n"
    "        a[i] = i;\n"
    "        i = i + 1;\n"
    "    }\n"
    "    let total = 0;\n"
    "    for (let j = 0; j < n; j += 1) {\n"
    "        total = total + a[j];\n"
    "    }\n"
    "    print total;\n"
    "}\n"
)


def test_module_has_target_triple():
    # opt's cost model refuses to vectorize a module without a target triple,
    # so the backend must stamp one onto every module it emits.
    assert re.search(r'^target triple = "', emit_ir(HOT_LOOP), re.M)


def test_hot_loop_vectorizes_after_opt():
    # -O2 may rename or merge the vector body block, but vectorized IR always
    # carries vector types (or a vector reduction intrinsic).
    ir = optimize(emit_ir(HOT_RUNTIME_LOOP))
    assert "<4 x" in ir or "llvm.vector.reduce" in ir, ir


def test_hot_runtime_loop_result_still_correct():
    # run_and_capture goes through the real opt stage: -O2 must not change
    # what the hot loops compute (sum 0..63).
    ec, out, err = run_and_capture(HOT_RUNTIME_LOOP)
    assert ec == 0, err
    assert out == ["2016"], out
