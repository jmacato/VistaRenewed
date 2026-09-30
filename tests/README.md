# Tests

Run commands from the repository root. Restore dependencies and build the
toolchain image as described in [BUILDING](../docs/BUILDING.md).

## Public bringup checks

```sh
python3 scripts/test_public.py --list
bash scripts/dev-container.sh run python3 scripts/test_public.py
```

The explicit CPU manifest covers source restoration, launcher behavior,
packaging helpers, transport and presentation contracts, and guest frontend
fixtures. It starts no VM and executes no GPU workload. Each run writes a fresh
directory under ignored `test-artifacts/` with `results.json` and per-case logs.
Use `--output PATH` for a fresh named directory. `--only NAME` selects a subset;
its report remains marked as a subset.

## Where checks live

| Location | Purpose |
| --- | --- |
| `bootstrap/` | Pinned source restoration and overlay validation |
| `test_*.py` | Launcher, control tooling, SDK inputs, attribution and trace helpers |
| `vista/` | Guest frontend fixtures, CPU contracts and explicit Vista runners |
| `fixtures/` | Small source inputs for CPU tests |
| `linux-*.cpp`, `run-native-blit.py` | Native Linux graphics checks |
| Component test directories | Upstream and component-specific checks |

[The Vista test guide](vista/README.md) documents individual commands and their
prerequisites. Building a component does not run every test in its tree. Native
GPU suites require a freshly built backend and actual device access; the build
container intentionally has no graphics devices. Guest tests require an
explicit disposable VM and matching installed components.

## Evidence boundaries

Keep generated binaries and captures in ignored build/evidence directories,
not beside source fixtures. Preserve failures, interruption and timeout results.
CPU, build, package and runtime results establish different things; follow
[release validation](../docs/RELEASE-VALIDATION.md) before making compatibility
or presentation claims.
