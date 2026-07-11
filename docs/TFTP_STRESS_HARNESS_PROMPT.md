# Prompt for Claude Code: TFTP Conformance & Robustness Test Harness

Paste everything below into Claude Code. The three seed files referenced
(`include/tftp_test_harness/adapter_interface.hpp`, `docs/ARCHITECTURE.md`,
`docs/RFC_CONFORMANCE_MATRIX.md`) should be placed in the repo root first so the
agent starts from the preplanned design.

---

## Purpose and scope

Build a **local, loopback conformance and robustness test harness** in C++ for
evaluating TFTP client and server implementations. The operator plugs in an
implementation they control or are assessing, runs the full suite, and gets an
objective report on how well that implementation conforms to the TFTP RFCs and
how it degrades under adversarial network conditions.

This is a measurement instrument for comparing implementations. All network
manipulation is applied by a userspace UDP proxy bound to `127.0.0.1`, sitting
between a client-under-test and a server-under-test on the local host only. It
is not directed at any external network or host. The impairments (packet loss,
duplication, reordering, delay, corruption, saturation, malformed-packet
injection) exist to reproduce the real network failure modes a compliant TFTP
implementation must survive — this is standard protocol robustness and fuzz
testing, the same discipline used to harden any UDP protocol implementation.

The end goal: run implementation A through the suite, run implementation B
through the identical suite separately, and produce a comparison report that
traces every difference to specific test IDs and RFC clauses so a reader can
see, with evidence, which implementation is more correct and more resilient.

## Deliverables

1. A CMake C++17 project (no third-party dependencies — see constraints).
2. A userspace UDP **impairment proxy** that both injects faults and observes
   every packet for conformance judgments.
3. A **single plug-in adapter interface** (already sketched in
   `include/tftp_test_harness/adapter_interface.hpp`) — the only file an
   integrator edits to add a new implementation.
4. The full test suite implementing every ID in `docs/RFC_CONFORMANCE_MATRIX.md`
   (RFC 1350, 2347, 2348, 2349, 7440, plus the adversarial and malformed-input
   categories).
5. A metrics store and a **self-contained HTML report generator** (inline CSS,
   hand-generated inline SVG charts, no external assets or JavaScript
   frameworks), including a **comparison mode** that diffs two runs.
6. A self-verification suite: a deliberately buggy reference implementation and a
   correct one, proving the harness flags the former and clears the latter.
7. Example adapters, including a reusable base for CLI-binary implementations.

## Constraints

- **Dependencies**: standard library only, interpreted as *no third-party
  libraries*. C++ has no standard networking, so use native sockets — BSD
  sockets on POSIX, Winsock2 on Windows — behind one thin shim
  (`net/socket_platform.hpp`). Nothing else outside the standard library. If a
  hash is needed and an OS crypto API is undesirable, vendor a single small
  public-domain SHA-256 rather than adding a dependency, and document it. The
  goal is a tree that builds in a constrained/air-gapped environment with only a
  toolchain and CMake.
- **Standard**: C++17. Use C++20 only where it is a clear win and toolchain
  support is confirmed.
- **Build**: CMake, with separate targets for the harness library, the runner,
  the report generator, example adapters, and the self-tests. Concurrency via
  `std::thread`.
- **Style**: no abbreviations in identifiers — names should read as prose
  (`retransmission_count`, not `rtx_cnt`). Cite the relevant RFC clause in
  comments near the logic it governs. Use the precise terminology of the TFTP
  RFCs and the UDP/IP specifications. Prioritize correctness and coverage;
  reserve stylistic polish for the final passes.
- **Determinism**: the proxy uses a seeded PRNG; record the seed with every
  test so any failure reproduces exactly.

## How to work (this is a long-running, checkpointed build)

Time is not a constraint; overnight runs are expected and fine. Work in phases
and keep a living `PROGRESS.md` at the repo root so you can resume across
sessions. `PROGRESS.md` should list phases, per-phase task checklists with
status, current focus, and any open design questions. Update it as you go and
commit after each meaningful unit of work.

Suggested phases:

1. **Scaffold** — CMake tree, `PROGRESS.md`, socket shim, TFTP packet
   parse/serialize with unit tests. Confirm a trivial loopback transfer works
   through a pass-through (zero-impairment) proxy.
2. **Adapter layer** — finalize the interface, build the subprocess-client base
   adapter, and one worked example against a common system TFTP binary.
3. **Impairment proxy** — implement each impairment stage with its own unit
   tests; verify injected faults are observed as intended.
4. **Observer/oracle** — packet-level conformance checks and the content
   integrity oracle.
5. **Test suite** — implement A→H from the matrix; each test self-describes its
   RFC citation, required capabilities, and assertions. Unsupported capabilities
   record as SKIPPED, never FAILED.
6. **Resilience sweeps** — F-series intensity sweeps producing resilience curves;
   chunk and checkpoint them for overnight execution.
7. **Metrics + report** — structured metrics store; single-file HTML report;
   comparison mode.
8. **Self-verification** — buggy vs correct reference implementations; assert the
   harness's own correctness.
9. **Polish** — naming, comments, RFC cross-references, documentation, final
   cleanup pass.

Before starting, restate your understanding and lay out the concrete plan in
`PROGRESS.md`. If a design decision is genuinely ambiguous, note it there and
proceed with the most defensible choice rather than blocking.

## What the report must make objective

For each implementation, compute and display: a weighted **conformance score**
(MUST failures shown separately, never averaged away), a **resilience index**
(normalized area under each impairment's success-rate curve), an **integrity
ledger** (any transfer that reported success but delivered non-matching bytes —
the most damning metric), a **graceful-degradation rate** (clean failure vs
hang/crash/corruption), **efficiency** counters (retransmissions, packets on
wire, throughput), and a **capability matrix** (supported RFCs/options).

Comparison mode diffs these six axes between two independently produced runs and
attributes every divergence to specific test IDs and RFC clauses.

## Definition of done

- `cmake --build` succeeds clean on Linux; the socket shim compiles for Windows.
- Every matrix test ID is implemented and either passes against the correct
  reference implementation or is correctly reported as SKIPPED/unsupported.
- The buggy reference implementation is flagged on exactly the defects it
  contains (at minimum: Sorcerer's Apprentice cascade, broken final-block rule),
  and the correct one passes — demonstrating the instrument's own validity.
- Running the suite twice against different adapters and passing both metrics
  stores to comparison mode yields a single HTML report that a reader can use to
  justify, with test-ID and RFC-clause evidence, which implementation is
  superior on each axis.
- `PROGRESS.md` reflects the final state.

Begin by reading the three seed files, then write your plan into `PROGRESS.md`.
