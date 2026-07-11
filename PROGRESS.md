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
- [ ] **Phase 1 — Scaffold**: CMake, socket shim, UDP socket, endpoint, TFTP packet
  codec + unit tests, seeded PRNG, UDP checksum helper. Loopback transfer through a
  zero-impairment proxy.
- [ ] **Phase 2 — Reference engine + adapters**: correct engine, buggy engine,
  reference client/server adapters, subprocess base, system-tftp example.
- [ ] **Phase 3 — Impairment proxy**: each stage + per-stage unit tests.
- [ ] **Phase 4 — Observer / oracle**: packet-level conformance checks + content oracle.
- [ ] **Phase 5 — Test suite A–H + fixtures + runner**: every matrix ID; SKIP vs FAIL.
- [ ] **Phase 6 — Resilience sweeps**: F-series intensity sweeps + checkpointing.
- [ ] **Phase 7 — Metrics + report**: metrics store, HTML report, comparison mode.
- [ ] **Phase 8 — Self-verification + polish**: buggy-vs-correct proof; final cleanup.

## Current focus

Phase 1 — networking core and packet codec.

## Open design questions

- Windows path: the socket shim compiles for Winsock2 but is only exercised on POSIX in
  this environment. Kept behind `#ifdef _WIN32`; not runtime-tested on Windows.
- IP-level UDP checksum recomputation for the corruption stage: we recompute the UDP
  checksum over the *payload we deliver* so the datagram stays deliverable; since we use
  connected UDP on loopback the kernel owns the real checksum, so "checksum-passing
  corruption" is modeled at the TFTP payload layer (the bytes the peer parses), which is
  what the conformance test actually cares about. Documented in `impairments`.
