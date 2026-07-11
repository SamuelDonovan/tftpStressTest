# TFTP Conformance and Robustness Test Matrix

This document is the research backbone for the harness. It enumerates the
behaviors to test, the RFC clause each is derived from, the expected outcome,
and a severity used to weight the objective comparison score. Test identifiers
are stable and appear verbatim in the generated report.

Severity legend, aligned with RFC 2119 keyword strength:
- **CRITICAL** — violates a MUST, or silently corrupts/loses data. A completed
  transfer whose bytes do not match the source is always CRITICAL.
- **MAJOR** — violates a SHOULD, or hangs/crashes instead of failing cleanly.
- **MINOR** — cosmetic or MAY-level divergence, or inefficiency.
- **INFO** — capability presence/absence; not itself a pass/fail.

Every test records: outcome (PASS / FAIL / SKIPPED-unsupported), the RFC
citation, measured metrics, and, on failure, a plain-language explanation of
what the implementation did versus what the RFC requires.

---

## A. RFC 1350 — Base protocol (TFTP Revision 2)

| ID | Behavior under test | Expected | RFC clause | Severity |
|----|--------------------|----------|-----------|----------|
| A-01 | RRQ octet transfer, small file | Byte-exact download | RFC 1350 §1, §5 | CRITICAL |
| A-02 | WRQ octet transfer, small file | Byte-exact upload | RFC 1350 §1, §5 | CRITICAL |
| A-03 | Zero-byte file | Single final DATA of length 0; success | RFC 1350 §2 | MAJOR |
| A-04 | File length exactly 512 bytes | One full DATA then a 0-length final DATA | RFC 1350 §2 (final block = <blocksize) | CRITICAL |
| A-05 | File length exact multiple of block size | Terminating 0-length DATA present | RFC 1350 §2 | CRITICAL |
| A-06 | File length 511 / 513 bytes | Correct final-block detection | RFC 1350 §2 | MAJOR |
| A-07 | Large file (multi-megabyte) | Byte-exact; sustained lock-step | RFC 1350 §2 | CRITICAL |
| A-08 | netascii CR LF line endings | Correct end-of-line translation both directions | RFC 1350 §1, App. | MAJOR |
| A-09 | netascii bare CR must be CR NUL | CR encoded as CR NUL; decoded correctly | RFC 1350 App. (netascii) | MAJOR |
| A-10 | Lock-step ACK discipline | Sender waits for ACK(n) before DATA(n+1) | RFC 1350 §2 | MAJOR |
| A-11 | Error 1: file not found on RRQ | ERROR packet code 1, transfer aborts | RFC 1350 §5 (error codes) | MAJOR |
| A-12 | Error 6: file already exists on WRQ | ERROR packet code 6 | RFC 1350 §5 | MAJOR |
| A-13 | Error 2: access violation | ERROR packet code 2 | RFC 1350 §5 | MINOR |
| A-14 | Illegal opcode received | ERROR code 4 or clean discard | RFC 1350 §5 | MAJOR |
| A-15 | Malformed RRQ/WRQ (missing NUL terminator) | Rejected, not crashed | RFC 1350 §5 | MAJOR |

### TID (Transfer Identifier) semantics — RFC 1350 §4
| ID | Behavior under test | Expected | Severity |
|----|--------------------|----------|----------|
| A-20 | Server selects a fresh TID (ephemeral port) for the transfer | New source port on DATA/OACK | MAJOR |
| A-21 | Packet arrives from an unexpected TID | Reply ERROR code 5 to the stray source, do **not** abort the legitimate transfer | CRITICAL |
| A-22 | Injected spoofed ERROR from wrong TID | Ignored; transfer continues | CRITICAL |

### Retransmission and the Sorcerer's Apprentice Syndrome
| ID | Behavior under test | Expected | RFC clause | Severity |
|----|--------------------|----------|-----------|----------|
| A-30 | DATA lost, sender times out | Sender retransmits current DATA | RFC 1350 §2 | MAJOR |
| A-31 | ACK lost, sender times out | Sender retransmits; receiver discards duplicate DATA and re-ACKs | RFC 1350 §2 | MAJOR |
| A-32 | **Duplicate ACK injected** (Sorcerer's Apprentice) | Sender does **not** retransmit DATA in response to a duplicate ACK; no packet-doubling cascade | RFC 1123 §4.2 (documents SAS and the fix) | CRITICAL |
| A-33 | Retransmit only on timeout, never on duplicate | No amplification observed at proxy | RFC 1123 §4.2 | CRITICAL |
| A-34 | Retry cap / give-up behavior | Bounded retries; terminates rather than looping forever | RFC 1350 §2 (unspecified — document behavior) | MAJOR |

### 16-bit block-number wraparound — unspecified in RFC 1350
| ID | Behavior under test | Expected | Severity |
|----|--------------------|----------|----------|
| A-40 | Transfer exceeding 65535 blocks (> 32 MB at 512-byte blocks) | Document behavior at rollover: wrap to 0, wrap to 1, or abort. Divergence here is a prime objective-comparison data point | MAJOR |
| A-41 | Block-number desync injection (proxy sends wrong block number) | Receiver detects mismatch; does not silently accept out-of-sequence data as in-sequence | CRITICAL |

---

## B. RFC 2347 — Option extension (OACK)

| ID | Behavior under test | Expected | RFC clause | Severity |
|----|--------------------|----------|-----------|----------|
| B-01 | Client requests an option; server supports it | Server replies OACK (opcode 6); client ACKs block 0 | RFC 2347 §2 | MAJOR |
| B-02 | Client requests an option; server does not support options | Server ignores options and proceeds as plain RFC 1350; MUST NOT send ERROR for unknown options | RFC 2347 §2 | CRITICAL |
| B-03 | Server OACKs an option the client never requested | Client rejects with ERROR code 8 | RFC 2347 §2 | MAJOR |
| B-04 | Option names are case-insensitive | "BLKSIZE" == "blksize" | RFC 2347 §2 | MINOR |
| B-05 | Malformed / truncated OACK | Rejected cleanly, no crash | RFC 2347 §2 | MAJOR |
| B-06 | ERROR code 8 (option negotiation failed) surfaced when appropriate | Correct code used | RFC 2347 §2 | MINOR |

---

## C. RFC 2348 — Block size option (blksize)

| ID | Value(s) | Expected | RFC clause | Severity |
|----|----------|----------|-----------|----------|
| C-01 | blksize = 512 (default) | Behaves identically to no option | RFC 2348 | MINOR |
| C-02 | blksize = 8 (minimum) | Honored or clamped, consistently | RFC 2348 (range 8..65464) | MAJOR |
| C-03 | blksize = 1428 (Ethernet-friendly) | Honored; final block detection correct | RFC 2348 | MAJOR |
| C-04 | blksize = 65464 (maximum) | Honored or negotiated down | RFC 2348 | MAJOR |
| C-05 | blksize below 8 / above 65464 | Rejected or clamped; not accepted verbatim | RFC 2348 | MAJOR |
| C-06 | Server clamps requested blksize down in OACK | Client honors the smaller returned value | RFC 2348 §Negotiation | MAJOR |
| C-07 | blksize exceeds path MTU | Transfer still correct despite IP fragmentation; document throughput impact | RFC 2348; UDP/IP fragmentation | MAJOR |

---

## D. RFC 2349 — Timeout interval and transfer size

| ID | Behavior under test | Expected | RFC clause | Severity |
|----|--------------------|----------|-----------|----------|
| D-01 | timeout option 1..255 seconds negotiated | Value echoed in OACK and observably applied | RFC 2349 §2 | MAJOR |
| D-02 | timeout out of range | Rejected | RFC 2349 §2 | MINOR |
| D-03 | tsize = 0 on RRQ | Server returns actual file size in OACK | RFC 2349 §3 | MAJOR |
| D-04 | tsize on WRQ = actual size | Server may reject if too large (disk full → ERROR 3) | RFC 2349 §3 | MAJOR |
| D-05 | Returned tsize accuracy | Matches real byte count | RFC 2349 §3 | MINOR |
| D-06 | Dishonest tsize (proxy rewrites) | Transfer integrity still enforced by receiver | RFC 2349 §3 | MAJOR |

---

## E. RFC 7440 — Windowsize option

| ID | Behavior under test | Expected | RFC clause | Severity |
|----|--------------------|----------|-----------|----------|
| E-01 | windowsize = 1 | Equivalent to classic lock-step | RFC 7440 §2 | MAJOR |
| E-02 | windowsize = 4, 16, 64 | Sender emits N blocks before awaiting ACK; correct data | RFC 7440 §2 | MAJOR |
| E-03 | Single block lost within a window | Receiver ACKs last in-sequence block; sender rolls back and resumes correctly | RFC 7440 §4 (rollback) | CRITICAL |
| E-04 | Reordering within a window | No silent corruption; correct reassembly | RFC 7440 §4 | CRITICAL |
| E-05 | Duplicate ACK within windowed transfer | No desync, no cascade | RFC 7440 §4; RFC 1123 §4.2 | CRITICAL |
| E-06 | windowsize negotiated down by server | Client honors returned value | RFC 7440 §2 | MAJOR |
| E-07 | Sustained loss forcing repeated rollback | Eventually completes byte-exact, or fails cleanly | RFC 7440 §4 | CRITICAL |

---

## F. Adversarial network conditions (stress to failure)

Applied by the impairment proxy against the transfers above. Each is swept
across intensity levels to build a **resilience curve** (success rate vs
intensity); the area under that curve is the primary objective comparator.

| ID | Impairment | Sweep | What it exposes | Severity of misbehavior |
|----|-----------|-------|-----------------|-------------------------|
| F-01 | Uniform packet loss | 1, 5, 10, 25, 50, 75, 90 % | Retransmission correctness and give-up policy | CRITICAL if data corrupts, MAJOR if hangs |
| F-02 | Bursty (Gilbert–Elliott) loss | tunable burst length | Behavior under correlated loss | MAJOR |
| F-03 | Packet duplication | 5, 25, 50 % | Sorcerer's Apprentice resistance | CRITICAL |
| F-04 | Reordering | small/large displacement | Sequence handling / desync | CRITICAL |
| F-05 | Fixed delay + jitter | up to and beyond RTO | Timeout tuning, spurious retransmit | MAJOR |
| F-06 | Corruption passing UDP checksum | opcode/block/data fields | Application-level validation | CRITICAL |
| F-07 | Bandwidth saturation / throttle | down to near-zero throughput | Progress under starvation; clean timeout vs hang | MAJOR |
| F-08 | Combined loss + reorder + delay | matrix of combinations | Compound-failure resilience | CRITICAL |
| F-09 | Total blackout after N blocks | drop everything mid-transfer | Bounded termination, no infinite loop | MAJOR |

---

## G. Malformed-input and interposition robustness

Still strictly conformance/robustness testing: verify the implementation
tolerates hostile-looking-but-possible network events without corrupting data
or crashing. All packets are injected by the local proxy.

| ID | Event | Expected | Severity |
|----|-------|----------|----------|
| G-01 | Truncated packet (< 4 bytes) | Discarded | MAJOR |
| G-02 | Oversized packet (> negotiated blksize) | Discarded or errored | MAJOR |
| G-03 | Invalid opcode (0, 9, 65535) | ERROR 4 or discard | MAJOR |
| G-04 | Invalid/undefined error code in ERROR packet | Handled without crash | MINOR |
| G-05 | Non-terminated mode/filename string | Rejected | MAJOR |
| G-06 | Second server responding to same request (TID confusion) | Only the legitimate TID accepted | CRITICAL |
| G-07 | Injected OACK mid-transfer | Ignored (options settle at start only) | MAJOR |
| G-08 | ACK for a future/never-sent block | Ignored, no desync | CRITICAL |

---

## H. Concurrency and resource edges

| ID | Behavior under test | Expected | Severity |
|----|--------------------|----------|----------|
| H-01 | Many concurrent transfers (server) | All complete byte-exact; distinct TIDs | MAJOR |
| H-02 | File at the 65535-block boundary | See A-40 | MAJOR |
| H-03 | Rapid start/stop churn | No leaked ports/handles | MINOR |

---

## Scoring model (for objective comparison)

For each implementation the report computes:

1. **Conformance score** — weighted pass rate over A–E and G, weighting
   CRITICAL ≫ MAJOR ≫ MINOR. MUST-level failures are shown separately and are
   never averaged away.
2. **Resilience index** — normalized area under each F resilience curve,
   averaged across impairments. Reported per-impairment and aggregate.
3. **Integrity record** — count of transfers that *reported success* but
   produced non-matching bytes. Any nonzero value is flagged prominently; this
   is the single most damning metric.
4. **Graceful-degradation rate** — fraction of failures that ended with a clean
   ERROR/termination rather than hang, crash, or corruption.
5. **Efficiency** — retransmissions, packets on wire, and throughput under
   nominal and each impaired condition.
6. **Capability matrix** — supported RFCs/options (INFO; SKIPPED tests are
   reported as unsupported, never counted as failures).

Two implementations are compared by running each independently through the
identical suite and diffing these six axes, with every difference traced back
to specific test IDs and RFC clauses.
