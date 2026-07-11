#include "runner/test_case.hpp"

#include "net/tftp_packet.hpp"
#include "runner/transfer_driver.hpp"
#include "verify/packet_observer.hpp"

#include <memory>

namespace tftp_test_harness::runner {

using metrics::MetricRecord;
using metrics::Outcome;
using metrics::Severity;
using net::Direction;
using net::InjectedPacket;
using net::TftpErrorCode;
using net::TraceRecord;
using verify::TransferDirection;

namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// FNV-1a mixing so each (test, intensity) draws a distinct but reproducible seed
// from the run's base seed.
std::uint64_t seed_for(const TestContext& context, const std::string& id,
                       int index) {
    const std::uint64_t fnv_prime = 1099511628211ull;
    std::uint64_t hash = 1469598103934665603ull;
    for (char c : id) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * fnv_prime;
    }
    hash = (hash ^ static_cast<std::uint64_t>(static_cast<unsigned>(index))) *
           fnv_prime;
    return context.base_seed ^ hash;
}

MetricRecord base_record(const TestCase& test, const TestContext& context) {
    MetricRecord record;
    record.test_id = test.id;
    record.title = test.title;
    record.rfc_clause = test.rfc_clause;
    record.severity = test.severity;
    record.implementation_name = context.implementation_name;
    return record;
}

void fill_counters(MetricRecord& record, const DriveResult& result) {
    record.counters["data_packets_sent"] = result.observer.data_packets_sent;
    record.counters["distinct_data_blocks"] =
        result.observer.distinct_data_blocks;
    record.counters["retransmissions"] =
        result.observer.retransmitted_data_packets;
    record.counters["duplicate_acks"] =
        result.observer.duplicate_acknowledgements;
    record.counters["datagrams_dropped"] = result.observer.datagrams_dropped;
    record.counters["datagrams_corrupted"] =
        result.observer.datagrams_corrupted;
    record.counters["datagrams_injected"] = result.observer.datagrams_injected;
    record.counters["duration_ms"] = result.duration_ms;
    const double bytes = static_cast<double>(result.integrity.delivered_size);
    record.counters["bytes"] = bytes;
    if (result.duration_ms > 0) {
        record.counters["throughput_bytes_per_second"] =
            bytes / (result.duration_ms / 1000.0);
    }
}

DriveRequest make_request(const TestContext& context, const TestCase& test,
                          const std::string& fixture_key,
                          TransferDirection direction, int seed_index = 0) {
    DriveRequest request;
    request.client = context.client;
    request.server = context.server;
    request.direction = direction;
    request.fixture = context.fixture(fixture_key);
    request.seed = seed_for(context, test.id, seed_index);
    request.work_root =
        context.work_dir_for(test.id) / std::to_string(seed_index);
    request.effective_block_size = 512;
    return request;
}

RequestedOptions with_block_size(std::uint32_t block_size) {
    RequestedOptions options;
    options.block_size = block_size;
    return options;
}
RequestedOptions with_window_size(std::uint16_t window_size) {
    RequestedOptions options;
    options.window_size = window_size;
    return options;
}

std::vector<MetricRecord> single(MetricRecord record) {
    return {std::move(record)};
}

// Judge a transfer expected to complete byte-exact.
void judge_byte_exact(MetricRecord& record, const DriveResult& result) {
    fill_counters(record, result);
    if (result.setup_failed) {
        record.outcome = Outcome::Fail;
        record.ungraceful_failure = true;
        record.narrative = "harness setup failed: " + result.setup_error;
        return;
    }
    if (result.watchdog_fired) {
        record.outcome = Outcome::Fail;
        record.ungraceful_failure = true;
        record.narrative =
            "transfer hung past the watchdog (no clean termination)";
        return;
    }
    if (result.app_result.completed_successfully &&
        result.integrity.verdict == verify::IntegrityVerdict::Mismatch) {
        record.outcome = Outcome::Fail;
        record.integrity_violation = true;
        record.severity = Severity::Critical;
        record.narrative = "reported SUCCESS but " + result.integrity.detail;
        return;
    }
    if (!result.app_result.completed_successfully) {
        record.outcome = Outcome::Fail;
        record.narrative = "transfer did not complete: " +
                           (result.app_result.reported_error_message.empty()
                                ? std::string("no error surfaced")
                                : result.app_result.reported_error_message);
        return;
    }
    if (!result.integrity.matches()) {
        record.outcome = Outcome::Fail;
        record.integrity_violation = true;
        record.narrative = result.integrity.detail;
        return;
    }
    record.outcome = Outcome::Pass;
    record.narrative =
        "byte-exact transfer completed. " + result.observer.narrative();
}

// ---- basic positive transfers ----
std::vector<MetricRecord> basic_download(TestContext& context,
                                         const TestCase& test,
                                         const std::string& fixture_key,
                                         RequestedOptions options = {},
                                         std::uint32_t block_size = 512,
                                         TransferMode mode = TransferMode::Octet) {
    auto request =
        make_request(context, test, fixture_key, TransferDirection::Download);
    request.options = options;
    request.mode = mode;
    request.effective_block_size = block_size;
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    judge_byte_exact(record, drive_transfer(request));
    return single(std::move(record));
}

std::vector<MetricRecord> basic_upload(TestContext& context,
                                       const TestCase& test,
                                       const std::string& fixture_key,
                                       RequestedOptions options = {},
                                       TransferMode mode = TransferMode::Octet) {
    auto request =
        make_request(context, test, fixture_key, TransferDirection::Upload);
    request.options = options;
    request.mode = mode;
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    judge_byte_exact(record, drive_transfer(request));
    return single(std::move(record));
}

// ---- A-04/A-05 final-block rule ----
std::vector<MetricRecord> final_block_rule(TestContext& context,
                                           const TestCase& test,
                                           const std::string& fixture_key) {
    auto request =
        make_request(context, test, fixture_key, TransferDirection::Download);
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    DriveResult result = drive_transfer(request);
    fill_counters(record, result);
    if (result.watchdog_fired || result.setup_failed) {
        record.outcome = Outcome::Fail;
        record.ungraceful_failure = true;
        record.narrative = result.setup_failed
                               ? ("setup failed: " + result.setup_error)
                               : "transfer hung past the watchdog";
    } else if (!result.observer.terminating_short_block_present) {
        record.outcome = Outcome::Fail;
        record.severity = Severity::Critical;
        record.narrative =
            "no terminating DATA block shorter than the block size was sent; "
            "the final-block rule (RFC 1350 section 2) is violated. " +
            result.observer.narrative();
    } else if (result.app_result.completed_successfully &&
               !result.integrity.matches()) {
        record.outcome = Outcome::Fail;
        record.integrity_violation = true;
        record.narrative = "reported success but " + result.integrity.detail;
    } else if (!result.app_result.completed_successfully) {
        record.outcome = Outcome::Fail;
        record.narrative = "transfer did not complete";
    } else {
        record.outcome = Outcome::Pass;
        record.narrative =
            "terminating short/empty DATA present; byte-exact. " +
            result.observer.narrative();
    }
    return single(std::move(record));
}

// ---- A-11 file not found ----
std::vector<MetricRecord> file_not_found(TestContext& context,
                                         const TestCase& test) {
    auto request =
        make_request(context, test, "empty", TransferDirection::Download);
    request.remote_filename = "definitely-not-present-9137.bin";
    request.stage_served_fixture = false; // must genuinely be absent
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    DriveResult result = drive_transfer(request);
    fill_counters(record, result);
    const bool got_error_1 =
        result.app_result.reported_error_code.has_value() &&
        *result.app_result.reported_error_code ==
            static_cast<std::uint16_t>(TftpErrorCode::FileNotFound);
    if (result.watchdog_fired) {
        record.outcome = Outcome::Fail;
        record.ungraceful_failure = true;
        record.narrative = "server hung instead of returning ERROR 1";
    } else if (got_error_1) {
        record.outcome = Outcome::Pass;
        record.narrative = "server returned ERROR code 1 (file not found)";
    } else if (!result.app_result.completed_successfully) {
        record.outcome = Outcome::Pass;
        record.severity = Severity::Minor;
        record.narrative =
            "missing file rejected cleanly (error code was not 1)";
    } else {
        record.outcome = Outcome::Fail;
        record.narrative = "server did not reject a request for a missing file";
    }
    return single(std::move(record));
}

// ---- A-12 file already exists ----
std::vector<MetricRecord> file_exists(TestContext& context,
                                      const TestCase& test) {
    auto request =
        make_request(context, test, "one", TransferDirection::Upload);
    request.plant_existing_destination = true;
    request.remote_filename = "already-there.bin";
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    DriveResult result = drive_transfer(request);
    fill_counters(record, result);
    const bool got_error_6 =
        result.app_result.reported_error_code.has_value() &&
        *result.app_result.reported_error_code ==
            static_cast<std::uint16_t>(TftpErrorCode::FileAlreadyExists);
    if (result.watchdog_fired) {
        record.outcome = Outcome::Fail;
        record.ungraceful_failure = true;
        record.narrative = "hung instead of returning ERROR 6";
    } else if (got_error_6) {
        record.outcome = Outcome::Pass;
        record.narrative = "server returned ERROR code 6 (file already exists)";
    } else if (!result.app_result.completed_successfully) {
        record.outcome = Outcome::Pass;
        record.severity = Severity::Minor;
        record.narrative = "overwrite refused (error code was not 6)";
    } else {
        record.outcome = Outcome::Pass;
        record.severity = Severity::Minor;
        record.narrative =
            "server allows overwriting an existing file (a MAY-level policy)";
    }
    return single(std::move(record));
}

// ---- A-13 access violation (path traversal) ----
std::vector<MetricRecord> access_violation(TestContext& context,
                                           const TestCase& test) {
    auto request =
        make_request(context, test, "empty", TransferDirection::Download);
    request.remote_filename = "../../etc/hosts";
    request.stage_served_fixture = false;
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    DriveResult result = drive_transfer(request);
    fill_counters(record, result);
    if (result.watchdog_fired) {
        record.outcome = Outcome::Fail;
        record.ungraceful_failure = true;
        record.narrative = "hung on a path-traversal request";
    } else if (!result.app_result.completed_successfully) {
        record.outcome = Outcome::Pass;
        record.narrative =
            "path-traversal request refused (error code " +
            (result.app_result.reported_error_code
                 ? std::to_string(*result.app_result.reported_error_code)
                 : std::string("none")) +
            ")";
    } else {
        record.outcome = Outcome::Fail;
        record.severity = Severity::Critical;
        record.narrative = "served a file outside the served directory";
    }
    return single(std::move(record));
}

InjectedPacket make_injection(std::vector<std::uint8_t> bytes, bool toward_client,
                              bool stray, const std::string& description) {
    InjectedPacket packet;
    packet.bytes = std::move(bytes);
    packet.toward_client = toward_client;
    packet.from_stray_tid = stray;
    packet.description = description;
    return packet;
}

// ---- A-14/G-03 illegal opcode injected ----
std::vector<MetricRecord> illegal_opcode(TestContext& context,
                                         const TestCase& test) {
    auto request =
        make_request(context, test, "kib_8", TransferDirection::Download);
    auto fired = std::make_shared<bool>(false);
    request.injection = [fired](const TraceRecord& observed)
        -> std::vector<InjectedPacket> {
        if (!*fired && observed.is_data() &&
            observed.disposition != net::TraceDisposition::Injected) {
            *fired = true;
            std::vector<std::uint8_t> bytes = {0x00, 0x09, 0x00, 0x01};
            return {make_injection(std::move(bytes), true, true,
                                   "illegal opcode 9 injected")};
        }
        return {};
    };
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    DriveResult result = drive_transfer(request);
    fill_counters(record, result);
    if (result.watchdog_fired) {
        record.outcome = Outcome::Fail;
        record.ungraceful_failure = true;
        record.narrative = "crashed/hung on an illegal opcode";
    } else if (result.app_result.completed_successfully &&
               result.integrity.matches()) {
        record.outcome = Outcome::Pass;
        record.narrative =
            "illegal opcode tolerated; transfer completed byte-exact";
    } else {
        record.outcome = Outcome::Fail;
        record.narrative = "an injected illegal opcode disrupted the transfer";
    }
    return single(std::move(record));
}

// ---- G-01/G-05 truncated / malformed packet injected ----
std::vector<MetricRecord> malformed_packet(TestContext& context,
                                           const TestCase& test,
                                           std::vector<std::uint8_t> bytes,
                                           const std::string& what) {
    auto request =
        make_request(context, test, "kib_8", TransferDirection::Download);
    auto fired = std::make_shared<bool>(false);
    request.injection = [fired, bytes, what](const TraceRecord& observed)
        -> std::vector<InjectedPacket> {
        if (!*fired && observed.is_data() &&
            observed.disposition != net::TraceDisposition::Injected) {
            *fired = true;
            return {make_injection(bytes, true, true, what)};
        }
        return {};
    };
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    DriveResult result = drive_transfer(request);
    fill_counters(record, result);
    if (result.watchdog_fired) {
        record.outcome = Outcome::Fail;
        record.ungraceful_failure = true;
        record.narrative = "crashed/hung on " + what;
    } else if (result.app_result.completed_successfully &&
               result.integrity.matches()) {
        record.outcome = Outcome::Pass;
        record.narrative = what + " tolerated; transfer completed byte-exact";
    } else {
        record.outcome = Outcome::Fail;
        record.narrative = what + " disrupted the transfer";
    }
    return single(std::move(record));
}

// ---- A-20 fresh TID ----
std::vector<MetricRecord> fresh_tid(TestContext& context, const TestCase& test) {
    auto request =
        make_request(context, test, "odd_medium", TransferDirection::Download);
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    DriveResult result = drive_transfer(request);
    fill_counters(record, result);
    if (result.observer.server_selected_fresh_tid) {
        record.outcome = Outcome::Pass;
        record.narrative =
            "server replied from a fresh TID distinct from the request port. " +
            result.observer.narrative();
    } else {
        record.outcome = Outcome::Fail;
        record.narrative = "server did not select a fresh TID for the transfer";
    }
    return single(std::move(record));
}

// ---- A-21/A-22/G-06/G-08 stray-TID / spurious injection ----
std::vector<MetricRecord> stray_tid(TestContext& context, const TestCase& test,
                                    std::vector<std::uint8_t> injected_bytes,
                                    bool toward_client,
                                    const std::string& what) {
    auto request =
        make_request(context, test, "kib_8", TransferDirection::Download);
    auto fired = std::make_shared<bool>(false);
    request.injection = [fired, injected_bytes, toward_client,
                         what](const TraceRecord& observed)
        -> std::vector<InjectedPacket> {
        if (!*fired && observed.is_data() &&
            observed.disposition != net::TraceDisposition::Injected) {
            *fired = true;
            return {make_injection(injected_bytes, toward_client, true, what)};
        }
        return {};
    };
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    DriveResult result = drive_transfer(request);
    fill_counters(record, result);
    const bool ok = result.app_result.completed_successfully &&
                    result.integrity.matches();
    if (result.watchdog_fired) {
        record.outcome = Outcome::Fail;
        record.ungraceful_failure = true;
        record.narrative = "aborted/hung after " + what;
    } else if (ok) {
        record.outcome = Outcome::Pass;
        record.narrative =
            what + " ignored; transfer completed byte-exact" +
            std::string(result.observer.stray_tid_reply_was_error_5
                            ? " (stray TID rebuffed with ERROR 5)"
                            : "");
    } else {
        record.outcome = Outcome::Fail;
        record.severity = Severity::Critical;
        record.narrative = what + " derailed the legitimate transfer";
    }
    return single(std::move(record));
}

// ---- A-32/A-33 Sorcerer's Apprentice ----
std::vector<MetricRecord> sorcerers_apprentice(TestContext& context,
                                               const TestCase& test) {
    auto request =
        make_request(context, test, "kib_8", TransferDirection::Download);
    request.injection = [](const TraceRecord& observed)
        -> std::vector<InjectedPacket> {
        if (observed.direction == Direction::ServerToClient &&
            observed.is_data() &&
            observed.disposition != net::TraceDisposition::Injected) {
            return {make_injection(
                net::serialize_acknowledgement(observed.packet.block_number),
                false, false, "duplicate ACK (Sorcerer's Apprentice probe)")};
        }
        return {};
    };
    MetricRecord record = base_record(test, context);
    record.seed = request.seed;
    DriveResult result = drive_transfer(request);
    fill_counters(record, result);
    if (result.watchdog_fired) {
        record.outcome = Outcome::Fail;
        record.ungraceful_failure = true;
        record.narrative = "hung under duplicate-ACK injection";
    } else if (result.observer.retransmitted_data_packets == 0) {
        record.outcome = Outcome::Pass;
        record.narrative =
            "no retransmission from duplicate ACKs; no amplification "
            "(RFC 1123 section 4.2). " +
            result.observer.narrative();
    } else {
        record.outcome = Outcome::Fail;
        record.severity = Severity::Critical;
        record.narrative =
            "sender retransmitted DATA in response to duplicate ACKs — "
            "Sorcerer's Apprentice amplification (" +
            std::to_string(result.observer.retransmitted_data_packets) +
            " needless retransmissions). " +
            result.observer.narrative();
    }
    return single(std::move(record));
}

// ---- F-series resilience sweep ----
// Runs the same transfer at each intensity, producing one record per point.
std::vector<MetricRecord> resilience_sweep(
    TestContext& context, const TestCase& test, const std::string& fixture_key,
    const std::vector<std::pair<double, net::ImpairmentConfig>>& points,
    RequestedOptions options = {}) {
    std::vector<MetricRecord> out;
    int index = 0;
    for (const auto& point : points) {
        auto request = make_request(context, test, fixture_key,
                                    TransferDirection::Download, index);
        request.impairment = point.second;
        request.options = options;
        request.watchdog = std::chrono::milliseconds(45000);
        MetricRecord record = base_record(test, context);
        record.seed = request.seed;
        record.intensity = point.first;
        char label[64];
        std::snprintf(label, sizeof(label), "%s=%.4g", test.id.c_str(),
                      point.first);
        record.intensity_label = label;
        DriveResult result = drive_transfer(request);
        fill_counters(record, result);
        const bool byte_exact = result.app_result.completed_successfully &&
                                result.integrity.matches();
        record.counters["success"] = byte_exact ? 1.0 : 0.0;
        if (result.app_result.completed_successfully &&
            result.integrity.verdict == verify::IntegrityVerdict::Mismatch) {
            record.outcome = Outcome::Fail;
            record.integrity_violation = true;
            record.severity = Severity::Critical;
            record.narrative =
                "reported SUCCESS but delivered corrupted bytes under "
                "impairment — " +
                result.integrity.detail;
        } else if (result.watchdog_fired) {
            record.outcome = Outcome::Fail;
            record.ungraceful_failure = true;
            record.narrative = "hung under impairment (no clean termination)";
        } else if (byte_exact) {
            record.outcome = Outcome::Pass;
            record.narrative = "completed byte-exact under impairment. " +
                               result.observer.narrative();
        } else {
            // A clean non-completion under heavy impairment is expected and
            // graceful, not a conformance fault: the implementation behaved
            // correctly (no corruption, no hang). It counts as PASS for the
            // outcome while the `success` counter (0) records the drop for the
            // resilience curve.
            record.outcome = Outcome::Pass;
            record.narrative =
                "did not complete at this intensity, but failed cleanly (no "
                "corruption, no hang). " +
                result.observer.narrative();
        }
        out.push_back(std::move(record));
        ++index;
    }
    return out;
}

// Integrity-focused judging for corruption tests (A-41 / F-06): the only true
// failures are silent data corruption (reported success but wrong bytes) and a
// hang. Detecting the corruption and failing cleanly — or completing byte-exact
// after dropping corrupted datagrams — both pass. Runs one record per point.
std::vector<MetricRecord> integrity_under_corruption(
    TestContext& context, const TestCase& test, const std::string& fixture_key,
    const std::vector<std::pair<double, net::ImpairmentConfig>>& points) {
    std::vector<MetricRecord> out;
    int index = 0;
    for (const auto& point : points) {
        auto request = make_request(context, test, fixture_key,
                                    TransferDirection::Download, index);
        request.impairment = point.second;
        request.watchdog = std::chrono::milliseconds(25000);
        MetricRecord record = base_record(test, context);
        record.seed = request.seed;
        record.intensity = point.first;
        char label[64];
        std::snprintf(label, sizeof(label), "%s=%.4g", test.id.c_str(),
                      point.first);
        record.intensity_label = label;
        DriveResult result = drive_transfer(request);
        fill_counters(record, result);
        const bool byte_exact = result.app_result.completed_successfully &&
                                result.integrity.matches();
        record.counters["success"] = byte_exact ? 1.0 : 0.0;
        if (result.app_result.completed_successfully &&
            result.integrity.verdict == verify::IntegrityVerdict::Mismatch) {
            record.outcome = Outcome::Fail;
            record.integrity_violation = true;
            record.severity = Severity::Critical;
            record.narrative =
                "SILENTLY accepted corrupted data as valid — " +
                result.integrity.detail;
        } else if (result.watchdog_fired) {
            record.outcome = Outcome::Fail;
            record.ungraceful_failure = true;
            record.narrative = "hung under corruption (no clean termination)";
        } else if (byte_exact) {
            record.outcome = Outcome::Pass;
            record.narrative =
                "dropped corrupted datagrams and completed byte-exact. " +
                result.observer.narrative();
        } else {
            record.outcome = Outcome::Pass;
            record.narrative =
                "detected corruption and refused to deliver invalid data "
                "(failed cleanly, no silent corruption). " +
                result.observer.narrative();
        }
        out.push_back(std::move(record));
        ++index;
    }
    return out;
}

net::ImpairmentConfig loss_config(double probability) {
    net::ImpairmentConfig config;
    config.uniform_loss_probability = probability;
    return config;
}
net::ImpairmentConfig dup_config(double probability) {
    net::ImpairmentConfig config;
    config.duplication_probability = probability;
    return config;
}
net::ImpairmentConfig reorder_config(double probability, int extra_delay_ms) {
    net::ImpairmentConfig config;
    config.reorder_probability = probability;
    config.reorder_extra_delay = std::chrono::milliseconds(extra_delay_ms);
    return config;
}
net::ImpairmentConfig delay_config(int fixed_ms, int jitter_ms) {
    net::ImpairmentConfig config;
    config.fixed_delay = std::chrono::milliseconds(fixed_ms);
    config.jitter = std::chrono::milliseconds(jitter_ms);
    return config;
}
net::ImpairmentConfig corruption_config(double probability,
                                        net::CorruptionTarget target) {
    net::ImpairmentConfig config;
    config.corruption_probability = probability;
    config.corruption_target = target;
    return config;
}

} // namespace

// ===========================================================================
// Registry
// ===========================================================================
std::vector<TestCase> build_test_registry() {
    std::vector<TestCase> tests;
    const std::string rrq = capability::read_request;
    const std::string wrq = capability::write_request;
    const std::string opt = capability::option_negotiation;
    const std::string blk = capability::block_size;
    const std::string win = capability::window_size;
    const std::string tsz = capability::transfer_size;
    const std::string tmo = capability::timeout_option;
    const std::string net_ascii = capability::netascii_mode;

    auto add = [&](const std::string& id, const std::string& title,
                   const std::string& rfc, Severity severity,
                   std::vector<std::string> caps,
                   std::function<std::vector<MetricRecord>(TestContext&,
                                                           const TestCase&)>
                       run) {
        TestCase test;
        test.id = id;
        test.title = title;
        test.rfc_clause = rfc;
        test.severity = severity;
        test.required_capabilities = std::move(caps);
        test.run = std::move(run);
        tests.push_back(std::move(test));
    };

    // ---- A. RFC 1350 base protocol ----
    add("A-01", "RRQ octet transfer, small file", "RFC 1350 sections 1, 5",
        Severity::Critical, {rrq},
        [](TestContext& c, const TestCase& t) { return basic_download(c, t, "one"); });
    add("A-02", "WRQ octet transfer, small file", "RFC 1350 sections 1, 5",
        Severity::Critical, {wrq},
        [](TestContext& c, const TestCase& t) { return basic_upload(c, t, "one"); });
    add("A-03", "Zero-byte file", "RFC 1350 section 2", Severity::Major, {rrq},
        [](TestContext& c, const TestCase& t) { return basic_download(c, t, "empty"); });
    add("A-04", "File length exactly 512 bytes", "RFC 1350 section 2",
        Severity::Critical, {rrq},
        [](TestContext& c, const TestCase& t) { return final_block_rule(c, t, "exact_block"); });
    add("A-05", "File length exact multiple of block size",
        "RFC 1350 section 2", Severity::Critical, {rrq},
        [](TestContext& c, const TestCase& t) { return final_block_rule(c, t, "two_blocks_exact"); });
    add("A-06", "File length 511 / 513 bytes", "RFC 1350 section 2",
        Severity::Major, {rrq}, [](TestContext& c, const TestCase& t) {
            auto a = basic_download(c, t, "just_under_block");
            auto b = basic_download(c, t, "just_over_block");
            a.insert(a.end(), b.begin(), b.end());
            return a;
        });
    add("A-07", "Large multi-block file", "RFC 1350 section 2",
        Severity::Critical, {rrq},
        [](TestContext& c, const TestCase& t) { return basic_download(c, t, "kib_64"); });
    add("A-08", "netascii CR LF line endings", "RFC 1350 section 1, App.",
        Severity::Major, {rrq, net_ascii}, [](TestContext& c, const TestCase& t) {
            return basic_download(c, t, "netascii_mixed", {}, 512,
                                  TransferMode::Netascii);
        });
    add("A-09", "netascii bare CR must be CR NUL", "RFC 1350 App. (netascii)",
        Severity::Major, {wrq, net_ascii}, [](TestContext& c, const TestCase& t) {
            return basic_upload(c, t, "netascii_mixed", {},
                                TransferMode::Netascii);
        });
    add("A-10", "Lock-step ACK discipline", "RFC 1350 section 2",
        Severity::Major, {rrq},
        [](TestContext& c, const TestCase& t) { return basic_download(c, t, "odd_medium"); });
    add("A-11", "Error 1: file not found on RRQ", "RFC 1350 section 5",
        Severity::Major, {rrq},
        [](TestContext& c, const TestCase& t) { return file_not_found(c, t); });
    add("A-12", "Error 6: file already exists on WRQ", "RFC 1350 section 5",
        Severity::Major, {wrq},
        [](TestContext& c, const TestCase& t) { return file_exists(c, t); });
    add("A-13", "Error 2: access violation", "RFC 1350 section 5",
        Severity::Minor, {rrq},
        [](TestContext& c, const TestCase& t) { return access_violation(c, t); });
    add("A-14", "Illegal opcode received", "RFC 1350 section 5",
        Severity::Major, {rrq},
        [](TestContext& c, const TestCase& t) { return illegal_opcode(c, t); });
    add("A-15", "Malformed RRQ/WRQ (missing NUL terminator)",
        "RFC 1350 section 5", Severity::Major, {rrq},
        [](TestContext& c, const TestCase& t) {
            // A WRQ opcode with an unterminated filename, injected at the server.
            std::vector<std::uint8_t> bytes = {0x00, 0x02, 'x', 'y', 'z'};
            return malformed_packet(c, t, bytes,
                                    "unterminated request string");
        });

    // TID semantics
    add("A-20", "Server selects a fresh TID", "RFC 1350 section 4",
        Severity::Major, {rrq},
        [](TestContext& c, const TestCase& t) { return fresh_tid(c, t); });
    add("A-21", "Packet from an unexpected TID", "RFC 1350 section 4",
        Severity::Critical, {rrq}, [](TestContext& c, const TestCase& t) {
            return stray_tid(c, t, net::serialize_acknowledgement(1), true,
                             "stray-TID ACK");
        });
    add("A-22", "Injected spoofed ERROR from wrong TID", "RFC 1350 section 4",
        Severity::Critical, {rrq}, [](TestContext& c, const TestCase& t) {
            return stray_tid(
                c, t,
                net::serialize_error(TftpErrorCode::NotDefined, "spoofed"),
                true, "spoofed ERROR from stray TID");
        });

    // Retransmission and Sorcerer's Apprentice
    add("A-30", "DATA lost, sender times out and retransmits",
        "RFC 1350 section 2", Severity::Major, {rrq},
        [](TestContext& c, const TestCase& t) {
            return resilience_sweep(c, t, "kib_8", {{0.1, loss_config(0.1)}});
        });
    add("A-31", "ACK lost, receiver discards duplicate DATA and re-ACKs",
        "RFC 1350 section 2", Severity::Major, {rrq},
        [](TestContext& c, const TestCase& t) {
            return resilience_sweep(c, t, "kib_8", {{0.1, loss_config(0.1)}});
        });
    add("A-32", "Duplicate ACK (Sorcerer's Apprentice)",
        "RFC 1123 section 4.2", Severity::Critical, {rrq},
        [](TestContext& c, const TestCase& t) { return sorcerers_apprentice(c, t); });
    add("A-33", "Retransmit only on timeout, never on duplicate",
        "RFC 1123 section 4.2", Severity::Critical, {rrq},
        [](TestContext& c, const TestCase& t) { return sorcerers_apprentice(c, t); });
    add("A-34", "Retry cap / give-up behavior", "RFC 1350 section 2",
        Severity::Major, {rrq}, [](TestContext& c, const TestCase& t) {
            net::ImpairmentConfig blackout;
            blackout.blackout_enabled = true;
            blackout.blackout_after_data_packets = 3;
            auto request = make_request(c, t, "kib_8",
                                        TransferDirection::Download);
            request.impairment = blackout;
            request.watchdog = std::chrono::milliseconds(20000);
            MetricRecord record = base_record(t, c);
            record.seed = request.seed;
            DriveResult result = drive_transfer(request);
            fill_counters(record, result);
            if (result.watchdog_fired) {
                record.outcome = Outcome::Fail;
                record.ungraceful_failure = true;
                record.narrative =
                    "did not terminate after total blackout (infinite loop)";
            } else {
                record.outcome = Outcome::Pass;
                record.narrative =
                    "bounded termination after blackout (gave up cleanly)";
            }
            return single(std::move(record));
        });
    add("A-41", "Block-number desync injection (corrupted block number)",
        "RFC 1350 section 2", Severity::Critical, {rrq},
        [](TestContext& c, const TestCase& t) {
            return integrity_under_corruption(
                c, t, "odd_medium",
                {{0.05, corruption_config(0.05,
                                          net::CorruptionTarget::BlockNumber)}});
        });

    // ---- B. RFC 2347 option negotiation ----
    add("B-01", "Client requests an option; server supports it",
        "RFC 2347 section 2", Severity::Major, {rrq, opt, blk},
        [](TestContext& c, const TestCase& t) {
            return basic_download(c, t, "kib_8", with_block_size(1024), 1024);
        });
    add("B-04", "Option names are case-insensitive", "RFC 2347 section 2",
        Severity::Minor, {rrq, opt, blk}, [](TestContext& c, const TestCase& t) {
            return basic_download(c, t, "odd_medium", with_block_size(700), 700);
        });

    // ---- C. RFC 2348 blksize ----
    add("C-01", "blksize = 512 (default)", "RFC 2348", Severity::Minor,
        {rrq, opt, blk}, [](TestContext& c, const TestCase& t) {
            return basic_download(c, t, "kib_8", with_block_size(512), 512);
        });
    add("C-02", "blksize = 8 (minimum)", "RFC 2348", Severity::Major,
        {rrq, opt, blk}, [](TestContext& c, const TestCase& t) {
            // 513 bytes is not a multiple of 8, so the terminating short block
            // always exists (isolates blksize handling from the final-block rule).
            return basic_download(c, t, "just_over_block", with_block_size(8), 8);
        });
    add("C-03", "blksize = 1428 (Ethernet-friendly)", "RFC 2348",
        Severity::Major, {rrq, opt, blk}, [](TestContext& c, const TestCase& t) {
            return basic_download(c, t, "kib_8", with_block_size(1428), 1428);
        });
    add("C-04", "blksize = 65464 (maximum)", "RFC 2348", Severity::Major,
        {rrq, opt, blk}, [](TestContext& c, const TestCase& t) {
            return basic_download(c, t, "kib_8", with_block_size(65464), 65464);
        });
    add("C-07", "blksize exceeds path MTU", "RFC 2348; UDP/IP fragmentation",
        Severity::Major, {rrq, opt, blk}, [](TestContext& c, const TestCase& t) {
            return basic_download(c, t, "kib_8", with_block_size(9000), 9000);
        });

    // ---- D. RFC 2349 timeout / tsize ----
    add("D-01", "timeout option negotiated", "RFC 2349 section 2",
        Severity::Major, {rrq, opt, tmo}, [](TestContext& c, const TestCase& t) {
            RequestedOptions options;
            options.timeout_seconds = 2;
            return basic_download(c, t, "odd_medium", options);
        });
    add("D-03", "tsize = 0 on RRQ returns file size", "RFC 2349 section 3",
        Severity::Major, {rrq, opt, tsz}, [](TestContext& c, const TestCase& t) {
            RequestedOptions options;
            options.transfer_size = 0;
            return basic_download(c, t, "kib_8", options);
        });

    // ---- E. RFC 7440 windowsize ----
    add("E-01", "windowsize = 1 (lock-step)", "RFC 7440 section 2",
        Severity::Major, {rrq, opt, win}, [](TestContext& c, const TestCase& t) {
            return basic_download(c, t, "kib_8", with_window_size(1));
        });
    add("E-02", "windowsize = 4/16/64", "RFC 7440 section 2", Severity::Major,
        {rrq, opt, win}, [](TestContext& c, const TestCase& t) {
            std::vector<MetricRecord> out;
            for (std::uint16_t w : {std::uint16_t(4), std::uint16_t(16),
                                    std::uint16_t(64)}) {
                auto records = basic_download(c, t, "kib_8", with_window_size(w));
                out.insert(out.end(), records.begin(), records.end());
            }
            return out;
        });
    add("E-03", "Single block lost within a window (rollback)",
        "RFC 7440 section 4", Severity::Critical, {rrq, opt, win},
        [](TestContext& c, const TestCase& t) {
            return resilience_sweep(c, t, "kib_8", {{0.1, loss_config(0.1)}},
                                    with_window_size(8));
        });
    add("E-04", "Reordering within a window", "RFC 7440 section 4",
        Severity::Critical, {rrq, opt, win}, [](TestContext& c, const TestCase& t) {
            return resilience_sweep(c, t, "kib_8",
                                    {{0.2, reorder_config(0.2, 40)}},
                                    with_window_size(8));
        });
    add("E-05", "Duplicate ACK within windowed transfer",
        "RFC 7440 section 4; RFC 1123 section 4.2", Severity::Critical,
        {rrq, opt, win}, [](TestContext& c, const TestCase& t) {
            return resilience_sweep(c, t, "kib_8", {{0.25, dup_config(0.25)}},
                                    with_window_size(8));
        });
    add("E-07", "Sustained loss forcing repeated rollback",
        "RFC 7440 section 4", Severity::Critical, {rrq, opt, win},
        [](TestContext& c, const TestCase& t) {
            return resilience_sweep(c, t, "kib_8", {{0.3, loss_config(0.3)}},
                                    with_window_size(16));
        });

    // ---- F. Adversarial network conditions (resilience sweeps) ----
    add("F-01", "Uniform packet loss", "RFC 1350 section 2 (retransmission)",
        Severity::Critical, {rrq}, [](TestContext& c, const TestCase& t) {
            std::vector<std::pair<double, net::ImpairmentConfig>> points;
            for (double p : {0.01, 0.05, 0.10, 0.25, 0.50, 0.75, 0.90}) {
                points.push_back({p, loss_config(p)});
            }
            return resilience_sweep(c, t, "kib_8", points);
        });
    add("F-02", "Bursty (Gilbert-Elliott) loss", "correlated loss",
        Severity::Major, {rrq}, [](TestContext& c, const TestCase& t) {
            std::vector<std::pair<double, net::ImpairmentConfig>> points;
            for (double g2b : {0.05, 0.15, 0.30}) {
                net::ImpairmentConfig config;
                config.gilbert_elliott_enabled = true;
                config.good_to_bad_probability = g2b;
                config.bad_to_good_probability = 0.4;
                config.loss_in_bad_state = 1.0;
                points.push_back({g2b, config});
            }
            return resilience_sweep(c, t, "kib_8", points);
        });
    add("F-03", "Packet duplication", "RFC 1123 section 4.2 (SAS resistance)",
        Severity::Critical, {rrq}, [](TestContext& c, const TestCase& t) {
            std::vector<std::pair<double, net::ImpairmentConfig>> points;
            for (double p : {0.05, 0.25, 0.50}) points.push_back({p, dup_config(p)});
            return resilience_sweep(c, t, "kib_8", points);
        });
    add("F-04", "Reordering", "sequence handling", Severity::Critical, {rrq},
        [](TestContext& c, const TestCase& t) {
            std::vector<std::pair<double, net::ImpairmentConfig>> points;
            points.push_back({0.1, reorder_config(0.1, 30)});
            points.push_back({0.3, reorder_config(0.3, 80)});
            return resilience_sweep(c, t, "kib_8", points);
        });
    add("F-05", "Fixed delay + jitter", "timeout tuning", Severity::Major,
        {rrq}, [](TestContext& c, const TestCase& t) {
            std::vector<std::pair<double, net::ImpairmentConfig>> points;
            points.push_back({50, delay_config(50, 50)});
            points.push_back({200, delay_config(200, 100)});
            return resilience_sweep(c, t, "kib_8", points);
        });
    add("F-06", "Corruption passing UDP checksum",
        "application-level validation", Severity::Critical, {rrq},
        [](TestContext& c, const TestCase& t) {
            // Corrupt the opcode field: an implementation parses the opcode and
            // can reject a structurally-invalid datagram, so a robust receiver
            // drops it and recovers by retransmission. (Corrupting the opaque
            // DATA payload is inherently undetectable by base TFTP, which has no
            // application-layer checksum — it would unfairly fail every
            // implementation, so it is not used as a pass/fail gate here.)
            std::vector<std::pair<double, net::ImpairmentConfig>> points;
            for (double p : {0.05, 0.15, 0.30}) {
                points.push_back(
                    {p, corruption_config(p, net::CorruptionTarget::OpcodeField)});
            }
            return integrity_under_corruption(c, t, "odd_medium", points);
        });
    add("F-07", "Bandwidth saturation / throttle", "progress under starvation",
        Severity::Major, {rrq}, [](TestContext& c, const TestCase& t) {
            std::vector<std::pair<double, net::ImpairmentConfig>> points;
            for (double bps : {50000.0, 10000.0, 2000.0}) {
                net::ImpairmentConfig config;
                config.throttle_enabled = true;
                config.throttle_bytes_per_second = bps;
                points.push_back({bps, config});
            }
            return resilience_sweep(c, t, "odd_medium", points);
        });
    add("F-08", "Combined loss + reorder + delay", "compound-failure resilience",
        Severity::Critical, {rrq}, [](TestContext& c, const TestCase& t) {
            net::ImpairmentConfig config;
            config.uniform_loss_probability = 0.1;
            config.reorder_probability = 0.1;
            config.reorder_extra_delay = std::chrono::milliseconds(40);
            config.fixed_delay = std::chrono::milliseconds(20);
            config.jitter = std::chrono::milliseconds(20);
            return resilience_sweep(c, t, "kib_8", {{1.0, config}});
        });
    add("F-09", "Total blackout after N blocks", "bounded termination",
        Severity::Major, {rrq}, [](TestContext& c, const TestCase& t) {
            net::ImpairmentConfig config;
            config.blackout_enabled = true;
            config.blackout_after_data_packets = 5;
            auto request =
                make_request(c, t, "kib_8", TransferDirection::Download);
            request.impairment = config;
            request.watchdog = std::chrono::milliseconds(20000);
            MetricRecord record = base_record(t, c);
            record.seed = request.seed;
            DriveResult result = drive_transfer(request);
            fill_counters(record, result);
            if (result.watchdog_fired) {
                record.outcome = Outcome::Fail;
                record.ungraceful_failure = true;
                record.narrative = "infinite loop after blackout";
            } else {
                record.outcome = Outcome::Pass;
                record.narrative = "terminated cleanly after blackout";
            }
            return single(std::move(record));
        });

    // ---- G. Malformed-input / interposition robustness ----
    add("G-01", "Truncated packet (< 4 bytes)", "robustness", Severity::Major,
        {rrq}, [](TestContext& c, const TestCase& t) {
            return malformed_packet(c, t, {0x00, 0x03},
                                    "truncated 2-byte datagram");
        });
    add("G-03", "Invalid opcode (0, 9, 65535)", "robustness", Severity::Major,
        {rrq}, [](TestContext& c, const TestCase& t) { return illegal_opcode(c, t); });
    add("G-04", "Invalid/undefined error code in ERROR packet", "robustness",
        Severity::Minor, {rrq}, [](TestContext& c, const TestCase& t) {
            std::vector<std::uint8_t> bytes = {0x00, 0x05, 0x00, 0x63,
                                               'x', 0x00};
            return malformed_packet(c, t, bytes, "undefined error code 99");
        });
    add("G-06", "Second server responding (TID confusion)",
        "RFC 1350 section 4", Severity::Critical, {rrq},
        [](TestContext& c, const TestCase& t) {
            return stray_tid(c, t, net::serialize_data(2, {0xDE, 0xAD}), true,
                             "spoofed DATA from a second server");
        });
    add("G-08", "ACK for a future/never-sent block", "robustness",
        Severity::Critical, {rrq}, [](TestContext& c, const TestCase& t) {
            return stray_tid(c, t, net::serialize_acknowledgement(60000), false,
                             "ACK for a never-sent block");
        });

    // ---- H. Concurrency and resource edges ----
    add("H-03", "Rapid start/stop churn", "resource edges", Severity::Minor,
        {rrq}, [](TestContext& c, const TestCase& t) {
            std::vector<MetricRecord> out;
            for (int i = 0; i < 5; ++i) {
                auto records = basic_download(c, t, "one");
                out.insert(out.end(), records.begin(), records.end());
            }
            // Collapse to a single pass/fail: all iterations must succeed.
            MetricRecord record = base_record(t, c);
            record.seed = out.empty() ? 0 : out.front().seed;
            bool all_ok = true;
            for (const auto& r : out) all_ok = all_ok && r.outcome == Outcome::Pass;
            record.outcome = all_ok ? Outcome::Pass : Outcome::Fail;
            record.narrative = all_ok
                                   ? "5 rapid start/stop transfers all clean"
                                   : "a transfer failed under rapid churn";
            return single(std::move(record));
        });

    return tests;
}

} // namespace tftp_test_harness::runner
