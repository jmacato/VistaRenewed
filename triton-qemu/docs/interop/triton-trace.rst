.. SPDX-License-Identifier: GPL-2.0-or-later

Triton diagnostic capture (downstream extension)
==============================================

``triton-trace-start`` and ``triton-trace-stop`` provide an opt-in, bounded
capture for correlating guest driver work with host processing. They are
Triton extensions, not upstream QEMU commands. The buffer holds 131072 events;
further events increment a loss counter, saturated at INT32_MAX. Recording
does no file I/O. A capture with lost events cannot establish complete timing
or frame coverage.

Start requires a nonzero run identity and rejects an already active capture.
Stop freezes the capture before exporting it to a newly created file. Existing
files are never overwritten. On export failure the frozen buffer remains
available for retry with another filename; retries preserve the stop time.
Failed writes remove the incomplete file. QEMU fdset paths are rejected because
they refer to existing descriptors. A new start replaces the old buffer.

The recorder supplements ordinary QEMU diagnostic logging: its fixed format
allows the same offline decoder to correlate guest and host clock domains,
detect loss, and validate transfer integrity. No synchronous log formatting or
per-event file writes occur on display or virtqueue paths.

Wire format
-----------

All integers are little-endian, independent of host byte order. A file consists
of a 128-byte header followed by ``count`` 80-byte records, without padding or
trailing data. Offsets below are in bytes. Reserved fields are zero.

Header:

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Value
   * - 0
     - uint64
     - Magic: 0x3152435454495254
   * - 8, 12
     - uint32 each
     - Version (1), record size (80)
   * - 16, 24
     - uint64 each
     - Run identity, clock frequency (1000000000 Hz)
   * - 32
     - uint32
     - Capacity (131072)
   * - 36, 40, 44, 48
     - int32 each
     - Stored count, dropped count, enabled (0), active writers (0)
   * - 52
     - uint32
     - Complete (1)
   * - 56, 64
     - uint64 each
     - Capture start and stop timestamps
   * - 72
     - int64
     - Reserved guest frame counter (0)
   * - 80
     - uint64
     - IEEE CRC-32 of the whole file with these eight bytes zeroed
   * - 88 through 127
     - uint64 each
     - Reserved (0)

Each record has eight uint64 fields at offsets 0 through 56: sequence number,
timestamp, run identity, guest frame (0 on host), command cookie, and three
event arguments. Four uint32 fields follow at offsets 64 through 76: event
kind, process ID, thread ID (0 for the host main loop), and context ID.
Host event kinds 64 through 73 are defined in ``include/ui/triton-trace.h``.

Timestamps use QEMU_CLOCK_REALTIME (the host monotonic clock), including while
the guest is paused. A display event records a frontend observation, not proof
of physical monitor delivery. Guest/host clock offsets require independent
causal calibration; timestamps alone do not establish frame identity.

Scanout and display event arguments are the resource ID, console index, and
zero. Display attribution is retained separately for each console and is
invalidated when its surface is replaced, its scanout is disabled, or the
console is finalized. Starting a capture clears all previous attribution. A
draw without a publication in the current capture emits no display event.
