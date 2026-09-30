# Build checks

Build checks for [Vista Renewed](../README.md) cover Triton code, the build
tools, IE setup and Sidebar providers.
They run without starting a VM or GPU workload.

```sh
bash scripts/dev-container.sh run python3 scripts/test_public.py
```

Use `--list` to see the checks, or `--only NAME` to run one. Logs go in
`test-artifacts/`; they are build outputs and are not shipped in the source.

The IE build also checks Vista PE imports:

```sh
python3 scripts/build_mshtml.py --both
```

CPU checks do not test the installed drivers or QEMU display performance.
See [current limits](../docs/STATUS.md).
