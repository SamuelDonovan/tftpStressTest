# PROGRESS — TFTP Conformance & Robustness Test Harness

Living checkpoint file. Update after each meaningful unit of work so the build is
resumable across sessions.

## Understanding (restated)

Build a **local, loopback** C++17 conformance and robustness test harness for TFTP
client/server implementations. An operator plugs an implementation in behind the
`ClientAdapter` / `ServerAdapter` interface. Every test drives that implementation
through a userspace **UDP impairment proxy** bound to `127.0.0.1`. The proxy both
injects faults (loss / duplication / reordering / delay / corruption / saturation /
malformed injection) and **observes** every packet to make packet-level conformance
judgments the adapters cannot make themselves. Results feed a structured metrics
store and a single-file HTML report with an A-vs-B comparison mode.

Everything is on the local host only; the impairments reproduce the real network
failure modes a compliant TFTP implementation must survive. Standard library only
(native BSD/Winsock sockets behind one shim); CMake; deterministic seeded PRNG so any
failure reproduces exactly.

## Key design decisions

- **In-process reference implementations drive self-verification.** Rather than depend
  on a system `tftp` binary being present, the harness ships its own full TFTP engine
  (client + server, all options) and a deliberately *buggy* variant. These are wrapped
  by in-process adapters. A subprocess client-adapter base + a system-`tftp` example
  are also provided for real-world use.
- **Proxy TID model.** The proxy listens on `proxy_port` (masquerades as the server's
  well-known port). On the first client datagram it allocates one per-session socket
  whose ephemeral port masquerades as the server TID to the client and as the client
  TID to the server — a NAT-like relay. Server TID switch is observed on the
  server-facing side. Injection uses an extra "stray TID" socket.
- **Content oracle** uses a vendored public-domain SHA-256 (`verify/sha256`), no OS
  crypto, no third-party dependency.
- **Metrics** are newline-delimited JSON records written by a self-contained writer.
- **Determinism.** One 64-bit seed per test; the proxy PRNG is seeded from it and the
  seed is recorded in every metrics record.

## Source layout

```
include/tftp_test_harness/adapter_interface.hpp   # the single plug-in point (provided)
src/net/        socket_platform, udp_socket, endpoint, tftp_packet, prng, checksum,
                impairments, impairment_proxy
src/verify/     sha256, content_oracle, packet_trace, packet_observer
src/metrics/    json_writer, metrics_store
src/fixtures/   fixture_generator
src/runner/     test_case, test_registry, runner
src/report/     html_report, comparison_report
adapters/       subprocess_client_adapter, system_tftp example,
                reference/ (engine, buggy engine, client/server adapters)
apps/           runner_main, report_main, compare_main
tests/          harness self-tests
```

## Phases & status

- [x] **Phase 0 — repo scaffold**: dirs, seed files relocated to `docs/` + `include/`, git init.
- [x] **Phase 1 — Scaffold**: CMake, socket shim, UDP socket, endpoint, TFTP packet
  codec + unit tests, seeded SplitMix64 PRNG. Loopback transfer through a
  zero-impairment proxy confirmed.
- [x] **Phase 2 — Reference engine + adapters**: full correct engine (RFC
  1350/2347/2348/2349/7440), buggy engine via named `EngineQuirks`, reference
  client/server adapters, subprocess client base, system-tftp example.
- [x] **Phase 3 — Impairment proxy**: pipeline (loss/GE-loss/dup/reorder/delay/
  corrupt/throttle/blackout) with per-stage unit tests; select() event-loop proxy with
  TID emulation, scheduled sends, injection, full trace.
- [x] **Phase 4 — Observer / oracle**: packet-level conformance observer + vendored
  SHA-256 content oracle. Proven to flag the buggy engine and clear the correct one.
- [x] **Phase 5 — Test suite A–H + fixtures + runner**: deterministic fixtures,
  self-describing registry for every matrix ID, transfer driver with watchdog +
  isolation, runner with capability resolution (SKIP vs FAIL).
- [x] **Phase 6 — Resilience sweeps**: F-series intensity sweeps; metrics streamed
  incrementally (crash-resilient checkpointing).
- [x] **Phase 7 — Metrics + report**: self-contained JSON metrics store, analysis
  model (six axes), single-file HTML report with inline SVG, comparison mode.
- [x] **Phase 8 — Self-verification + polish**: `test_self_verification` asserts the
  buggy engine is flagged on its exact defects and the correct one passes.

## Results (self-verification)

- **Correct** reference: 73/73 records pass, 0 integrity violations, 0 hangs.
- **Buggy** reference: flagged on the final-block rule (A-04/A-05), Sorcerer's
  Apprentice (A-32/A-33), and out-of-sequence acceptance (reordering) — while clean
  transfers unaffected by its defects pass.

## Notes / design decisions

- Windows path: the socket shim compiles for Winsock2 but is only exercised on POSIX in
  this environment. Kept behind `#ifdef _WIN32`; not runtime-tested on Windows. The
  subprocess launcher is POSIX-only (returns launch_failed on Windows); the in-process
  reference adapters work everywhere.
- Corruption stage models mutation at the TFTP payload layer (the bytes the peer
  parses): on loopback the kernel owns the real UDP checksum, so this is exactly the
  "checksum-passing corruption" F-06 is about. F-06 targets the opcode field (which an
  implementation parses and can reject); corrupting the opaque DATA payload is inherently
  undetectable by base TFTP and would unfairly fail every implementation, so it is not a
  pass/fail gate.
- Impairment/injection fixtures are deliberately non-exact-multiples of 512 so the
  buggy final-block defect stays isolated to A-04/A-05 and does not confound the other
  tests' attribution.
- Reference retransmission timeout is 400 ms (loopback RTT is microseconds); keeps the
  adversarial sweeps bounded while remaining far above the real round trip.
