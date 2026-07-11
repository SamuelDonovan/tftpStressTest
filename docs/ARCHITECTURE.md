# TFTP Test Harness — Architecture

## Purpose and scope (read first)

This is a **local, loopback conformance and robustness test harness** for TFTP
client and server implementations that the operator controls or is evaluating.
All network impairment is applied by a userspace UDP proxy bound to the local
host; the harness sits between a client-under-test and a server-under-test and
manipulates only that loopback traffic. It is a measurement instrument for
comparing implementations against the TFTP RFCs, not a tool directed at any
third-party network or host.

## High-level dataflow

```
  ClientAdapter  --UDP-->  Impairment Proxy  --UDP-->  ServerAdapter
   (under test)            (loss/dup/reorder/           (under test)
                            delay/corrupt/inject/
                            saturate + observer)
                                   |
                                   v
                         Packet Observer / Oracle
                                   |
                                   v
                    Metrics store  -->  HTML report generator
```

The proxy is the heart of the design. Because it sees every packet in both
directions, it serves two roles simultaneously: **fault injector** (applies the
F-series and G-series impairments) and **observer** (records the exact packet
sequence for conformance judgments the adapters cannot make on their own, e.g.
Sorcerer's Apprentice amplification, TID handling, final-block correctness).

## Components

### 1. Network impairment proxy (`net/impairment_proxy`)
- Binds a UDP socket on `127.0.0.1:<proxy_port>`; forwards to the real server
  endpoint; tracks the client's ephemeral TID and the server's chosen TID.
- Pipeline of composable impairment stages, each configured per test:
  loss (uniform + Gilbert–Elliott bursty), duplication, reordering (bounded
  displacement queue), delay + jitter, corruption (bit flips with UDP checksum
  recomputation so the datagram stays deliverable), bandwidth throttle/token
  bucket, blackout-after-N, and packet injection (spoofed ERROR/OACK/ACK/DATA
  from a wrong TID).
- Deterministic: seeded PRNG so any failing run is exactly reproducible. The
  seed is recorded in the report for every test.
- Records a timestamped packet trace (opcode, block number, length, direction,
  TID) for the observer.

### 2. Packet observer / oracle (`verify/`)
- Parses the trace to assert packet-level conformance the adapter cannot see:
  final-block rule (A-04/A-05), lock-step discipline (A-10), no duplicate-ACK
  amplification (A-32/A-33), TID discipline (A-20/A-21), block-number monotonicity
  and wraparound behavior (A-40/A-41), windowsize rollback correctness (E-03).
- Content oracle: SHA-256 (or a self-contained hash if avoiding OS crypto) of
  source vs delivered bytes. A "successful" transfer with a hash mismatch is the
  CRITICAL integrity failure that dominates the scoreboard.

### 3. Test registry and runner (`runner/`)
- Each test is a self-describing object: stable ID, RFC citation, required
  capabilities, fixture spec, proxy configuration, and assertions.
- Runner resolves `supports_capability` first: unsupported → SKIPPED (not FAIL).
- Sweeps F-series intensities to build resilience curves. Long sweeps are
  chunked and checkpointed so an overnight run is resumable (see PROGRESS model).
- Per-test isolation: fresh temp directories, fresh ports, timeouts to bound
  hangs so one stuck implementation cannot stall the suite.

### 4. Fixtures (`fixtures/`)
- Generated deterministically: sizes at every boundary (0, 1, 511, 512, 513,
  exact multiples, MB-scale, and a file crossing the 65535-block line).
- netascii fixtures with mixed CR/LF/CR-NUL to exercise A-08/A-09.

### 5. Metrics store (`metrics/`)
- Append-only structured records (self-contained JSON writer; no third-party
  library). One record per (test ID, intensity) with outcome, severity, RFC
  clause, measured counters, PRNG seed, and a human-readable failure narrative.

### 6. Report generator (`report/`)
- Reads the metrics store and emits a **single self-contained HTML file** (inline
  CSS, hand-generated inline SVG charts — no external assets, no JS frameworks,
  no network fetch). Sections: executive scorecard, capability matrix,
  per-RFC conformance tables, resilience curves, integrity ledger, and an
  appendix of failure narratives each linking test ID → RFC clause.
- **Comparison mode**: given two metrics stores (implementation A and B run
  separately through the identical suite), emit a diff report where every
  divergence is traced to specific test IDs and RFC clauses, with a summary
  verdict per scoring axis.

## Constraints and the "standard library only" question

C++ has no standard networking, so "standard library only" is interpreted as
**no third-party dependencies**: use the platform's native sockets — BSD sockets
on POSIX, Winsock2 on Windows — behind one thin platform shim
(`net/socket_platform.hpp`), and otherwise restrict to the C++ standard library.
This keeps the tree buildable in constrained/air-gapped environments with only a
toolchain and CMake. If a self-contained SHA-256 is preferred over an OS crypto
API, vendor a single small public-domain implementation rather than adding a
dependency; document the choice.

- **Language**: C++17 (portable, VxWorks/older-toolchain friendly). Use C++20
  only if a clear win and toolchain support is confirmed.
- **Build**: CMake with targets for the harness library, the runner executable,
  the report generator, and example adapters. Threads via `std::thread`.
- **Style**: no abbreviations in identifiers; names read as prose; comments cite
  RFC clauses. Correctness and coverage first; stylistic cleanup on final passes.

## Suggested source layout

```
tftp-test-harness/
  CMakeLists.txt
  PROGRESS.md                     # living plan / checkpoint (see prompt)
  include/tftp_test_harness/
    adapter_interface.hpp         # the single plug-in point (provided)
  src/
    net/impairment_proxy.*        # fault injection + forwarding
    net/socket_platform.*         # POSIX/Winsock shim
    net/tftp_packet.*             # opcode/field parse + serialize
    verify/packet_observer.*      # packet-level conformance oracle
    verify/content_oracle.*       # byte/hash integrity
    runner/test_registry.*        # test objects A..H
    runner/runner.*               # scheduling, sweeps, checkpointing
    metrics/metrics_store.*       # structured results writer
    report/html_report.*          # single-file HTML + inline SVG
    report/comparison_report.*    # A vs B diff
  adapters/
    subprocess_client_adapter.*   # base for CLI-binary implementations
    example_*                     # worked examples
  fixtures/                       # deterministic fixture generator
  tests/                          # self-tests of the harness itself
```

## Self-verification (the harness must earn trust)

Ship a deliberately buggy reference TFTP implementation (introducing, e.g., the
Sorcerer's Apprentice cascade and a broken final-block rule) plus a correct one,
and assert the harness flags the buggy one and clears the correct one. This is
what makes the report "irrefutable": the instrument is validated against known
ground truth before it judges anyone's code.
