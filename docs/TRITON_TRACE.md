# Graphics tracing

`tools/triton_trace.py` collects guest and host events through the control
service and QMP. It needs matching driver and host builds with trace support,
plus the guest recorder executable.

```sh
python3 tools/triton_trace.py capture --help
python3 tools/triton_trace.py analyze test-artifacts/my-trace
```

The `capture` command requires a fresh output directory, a workload command
and a workload kind. Use `--socket`, `--qmp` and `--container` for a nondefault
VM. `--package-manifest` can check the installed files against a package.

Output includes the source and binary identities, raw traces, a report and
`timeline.json.gz`, which can be opened in Perfetto. The `overhead` command
compares runs with and without recording.

Trace timings do not prove smooth output in the QEMU window. See
[current limits](STATUS.md) and the [test index](../tests/README.md).
