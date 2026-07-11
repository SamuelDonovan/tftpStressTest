# TFTP Conformance & Robustness Test Harness

A local, loopback **measurement instrument** for TFTP client and server
implementations. You plug an implementation in behind a single adapter
interface, run the full suite, and get an objective, evidence-backed report on
how well it conforms to the TFTP RFCs and how it degrades under adversarial
network conditions — with every result traced to a stable test ID and RFC
clause.

Everything runs on `127.0.0.1`. A userspace UDP **impairment proxy** sits
between a client-under-test and a server-under-test, injects the network faults
a compliant implementation must survive (loss, duplication, reordering, delay,
corruption, saturation, malformed-packet injection), and simultaneously
**observes** every packet to make the packet-level conformance judgments the
implementations cannot make about themselves.

## What it measures

For each implementation the report computes six objective axes:

1. **Conformance score** — weighted pass rate over the RFC sections (A–E, G),
   weighting CRITICAL ≫ MAJOR ≫ MINOR. MUST-level failures are listed separately
   and never averaged away.
2. **Resilience index** — normalized area under each impairment's success-rate
   curve (the F-series sweeps).
3. **Integrity ledger** — any transfer that *reported success* but delivered
   non-matching bytes. The single most damning metric.
4. **Graceful-degradation rate** — clean failures vs. hang/crash/corruption.
5. **Efficiency** — retransmissions, packets on the wire, throughput.
6. **Capability matrix** — supported RFCs/options (unsupported ⇒ tests SKIPPED,
   never FAILED).

Comparison mode diffs these axes between two independently produced runs and
attributes every divergence to specific test IDs and RFC clauses.

## Building

Requirements: a C++17 toolchain and CMake ≥ 3.16. No third-party dependencies —
native sockets (BSD/Winsock2) behind one shim, a vendored public-domain SHA-256,
and a self-contained JSON writer. The tree builds in a constrained/air-gapped
environment.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build          # run the harness self-tests
```

## Running the suite

The runner drives a plugged-in implementation and writes a metrics store:

```sh
# Self-verification personalities (in-process reference engine):
build/runner_main --impl correct   --out correct.json
build/runner_main --impl buggy     --out buggy.json
build/runner_main --impl base-only --out base.json    # declines options -> SKIPs

# Options: --seed N   --filter A-   --huge   (large + >65535-block fixtures)
```

Turn a metrics store into a single self-contained HTML report, or diff two:

```sh
build/report_main  correct.json  correct.html
build/compare_main correct.json  buggy.json  comparison.html
```

The HTML is one file — inline CSS, hand-drawn inline SVG charts, no external
assets, no JavaScript, theme-aware (light/dark). Safe to open offline.

## Plugging in your own implementation

Implement `ClientAdapter` and/or `ServerAdapter` from
[`include/tftp_test_harness/adapter_interface.hpp`](include/tftp_test_harness/adapter_interface.hpp)
— the only file an integrator must touch. Direct the implementation's traffic at
the `EndpointConfiguration` the harness hands you (the proxy), and declare the
capabilities it genuinely supports so unsupported tests are SKIPPED rather than
FAILED.

- For an **in-process** implementation, wrap it directly (see
  [`adapters/reference/`](adapters/reference/)).
- For a **CLI binary**, subclass `SubprocessClientAdapter` and supply just the
  argument vectors (worked example:
  [`adapters/example_system_tftp_client.hpp`](adapters/example_system_tftp_client.hpp)
  for the tftp-hpa `tftp` program).

Then run `runner_main` (or your own `main`) with your adapters in place of the
reference ones.

## Determinism

The impairment proxy uses a seeded, portable PRNG (SplitMix64), and every
metrics record stores the seed that produced it — so any failing run reproduces
exactly, on any toolchain.

## Self-verification (why the report is trustworthy)

The harness ships a deliberately **buggy** reference implementation (Sorcerer's
Apprentice cascade, broken final-block rule, out-of-sequence acceptance) and a
**correct** one, and asserts the instrument flags the former on exactly its
defects while clearing the latter (`tests/test_self_verification.cpp`). The
instrument is validated against known ground truth before it judges anyone's
code.

## Layout

```
include/tftp_test_harness/adapter_interface.hpp   the single plug-in point
src/net/         socket shim, UDP socket, TFTP codec, netascii, PRNG,
                 impairment pipeline, impairment proxy, packet trace
src/verify/      vendored SHA-256, content oracle, packet observer
src/metrics/     self-contained JSON, metrics store
src/fixtures/    deterministic fixture generator
src/runner/      transfer driver, test registry (A–H), runner
src/report/      analysis model, single-file HTML report, comparison
adapters/        subprocess base, system-tftp example, reference engine
apps/            runner_main, report_main, compare_main
tests/           harness self-tests
docs/            architecture, RFC conformance matrix, original prompt
```

See [`docs/RFC_CONFORMANCE_MATRIX.md`](docs/RFC_CONFORMANCE_MATRIX.md) for the
enumerated behaviors and their RFC citations, and
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the design.
