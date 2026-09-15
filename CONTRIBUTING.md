# Contributing to Aevix

Thanks for your interest in contributing! Here are the rules that keep
the project healthy.

## The test contract

`aevix test` is the single gate before any merge into `dev`.

- **A feature without a test does not exist.** If you add a language
  feature, built-in, or behavioural change, there must be at least one
  positive test (the feature works) and, where applicable, at least one
  negative test (the compiler rejects the wrong usage).
- **Negative tests are permanent.** Once a negative test is committed
  (e.g. "returning from an epoch is rejected"), it may never be silently
  removed. If the language changes to allow the previously-rejected
  pattern, the test must be updated to reflect the new behaviour — not
  deleted.
- **`aevix test` must be green before every merge.** No exceptions,
  including hotfixes.

## How we work

### Branches

```
main   = releases only (merge-commits with `--no-ff` + version tags)
dev    = the development mainline (all history lives here)
feat/* = feature branches (fork from dev, merge back with `--no-ff`)
```

- Almost all work happens on `dev` or on `feat/*` branches off it.
- `main` is touched **only** when cutting a release.
- `dev` is never rewritten or deleted.

### Commit messages

One line, English, lowercase first letter. Conventional Commits prefix
with an optional scope:

```
feat(backend): add sema stage
fix(cli): inherit stdin for input()
test: cover arena-backed slices
docs: document compound assignment
chore: ignore out.txt artifact
style: unify code comments
```

Typical scopes: `(frontend)`, `(backend)`, `(cli)`, `(tools)`.

No multi-line commit messages in regular commits. No Russian in commit
messages.

### Releasing

```bash
git switch dev
aevix test                                  # everything must be green

git switch main
git merge --no-ff dev -m "release 0.x.x: short summary"
git tag -a v0.x.x -m "release 0.x.x"
git push origin main --tags
```

`--no-ff` is mandatory on release merges so the release fan-in is
visible in `git log --graph`.

## Code style

- **C++:** `// === Section Header ===` for major sections; `/** ... */`
  block comments for function-level documentation. English, no trailing
  period on single-line comments.
- **Go:** short English comments above the line they explain; prefix
  explains *why*, not *what*.
- **Python:** standard docstrings on modules; comments explain *why*.

## Golden corpus

Examples in `examples/` serve as the language's living contract. Each
example that compiles and runs deterministically without arguments has
a matching `examples/<name>.golden` file containing the exact expected
stdout. `aevix test` verifies these automatically. If your change alters
output, update the `.golden` file in the same commit.
