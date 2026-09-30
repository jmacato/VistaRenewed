#!/usr/bin/env python3
"""Exercise primary layout validation and attach/detach ordering."""
from pathlib import Path
from harness import extract_section, run_harness
root = Path(__file__).resolve().parents[2]
host = root / 'triton-virglrenderer/src/neptune'
guest = root / 'triton-umd/src/virtio/neptune'
# These are a cast-direct wire ABI; host and guest must agree byte for byte.
for start, end in [('struct npt_blob_export_info {', '\n/* Synchronous.'),
                   ('struct npt_cmd_shared_query_layout {', '\n/* ====================================================================== */')]:
    assert extract_section(host/'npt_transport_defs.h', start, end) == extract_section(guest/'npt_transport_defs.h', start, end)
layout = extract_section(root/'triton-virglrenderer/src/virgl_resource.h',
                         'struct virgl_attachment_layout {', '\nstruct iovec;')
blob = extract_section(host/'npt_transport_defs.h', 'struct npt_blob_export_info {', '\n/* Synchronous.')
reply = extract_section(host/'npt_transport_defs.h', 'struct npt_cmd_shared_query_layout_reply {', '\n/* ====================================================================== */')
functions = extract_section(host/'npt_shared.c', 'static bool\nnpt_shared_linear_desc_valid(', '\n/* Duplicate the attached resource fd')
run_harness(Path(__file__).with_suffix('.c'), layout + "\nRESOURCE_RECORD\n" + blob + reply + functions)
