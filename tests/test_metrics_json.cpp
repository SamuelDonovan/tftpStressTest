// Tests for the self-contained JSON writer/parser and the metrics store record
// round trip. The report and comparison modes re-read the store, so a faithful
// round trip is required.

#include "metrics/json_writer.hpp"
#include "metrics/metrics_store.hpp"
#include "test_support.hpp"

#include <string>

using namespace tftp_test_harness::metrics;

TFTP_TEST_CASE(json_escaping, "JSON string escaping handles control and quotes") {
    TFTP_CHECK_EQUAL(escape_json_string("a\"b\\c\n"),
                     std::string("\"a\\\"b\\\\c\\n\""));
}

TFTP_TEST_CASE(json_round_trip, "JSON value serialize -> parse round trip") {
    JsonValue root = JsonValue::object();
    root.set("name", JsonValue::string("F-01"));
    root.set("passed", JsonValue::boolean(true));
    root.set("count", JsonValue::integer(42));
    root.set("rate", JsonValue::number(0.25));
    JsonValue list = JsonValue::array();
    list.append(JsonValue::integer(1));
    list.append(JsonValue::string("two"));
    root.set("items", std::move(list));

    const std::string text = root.serialize(false);
    JsonValue parsed;
    TFTP_CHECK_TRUE(parse_json(text, parsed));
    TFTP_CHECK_EQUAL(parsed.at("name").as_string(), std::string("F-01"));
    TFTP_CHECK_TRUE(parsed.at("passed").as_bool());
    TFTP_CHECK_EQUAL(parsed.at("count").as_integer(), 42LL);
    TFTP_CHECK_TRUE(parsed.at("rate").as_number() > 0.24 &&
                    parsed.at("rate").as_number() < 0.26);
    TFTP_CHECK_EQUAL(parsed.at("items").as_array().size(), std::size_t(2));
}

TFTP_TEST_CASE(json_rejects_garbage, "Parser rejects malformed input") {
    JsonValue value;
    TFTP_CHECK_FALSE(parse_json("{ not json", value));
    TFTP_CHECK_FALSE(parse_json("[1,2,", value));
    TFTP_CHECK_FALSE(parse_json("{}garbage", value));
}

TFTP_TEST_CASE(metric_record_round_trip, "MetricRecord survives JSON round trip") {
    MetricRecord record;
    record.test_id = "A-04";
    record.title = "File length exactly 512 bytes";
    record.rfc_clause = "RFC 1350 section 2";
    record.severity = Severity::Critical;
    record.outcome = Outcome::Fail;
    record.intensity = 0.5;
    record.intensity_label = "loss=50%";
    record.seed = 1234567890123ULL;
    record.narrative = "no terminating zero-length DATA block observed";
    record.integrity_violation = true;
    record.counters["retransmissions"] = 7;
    record.counters["throughput_bytes_per_second"] = 1024.5;
    record.implementation_name = "reference-buggy";

    const std::string line = record.to_json().serialize(false);
    JsonValue parsed;
    TFTP_CHECK_TRUE(parse_json(line, parsed));
    MetricRecord restored = MetricRecord::from_json(parsed);

    TFTP_CHECK_EQUAL(restored.test_id, std::string("A-04"));
    TFTP_CHECK_TRUE(restored.severity == Severity::Critical);
    TFTP_CHECK_TRUE(restored.outcome == Outcome::Fail);
    TFTP_CHECK_TRUE(restored.integrity_violation);
    TFTP_CHECK_EQUAL(restored.seed, std::uint64_t(1234567890123ULL));
    TFTP_CHECK_TRUE(restored.intensity.has_value());
    TFTP_CHECK_EQUAL(restored.counters.at("retransmissions"), 7.0);
}

TFTP_TEST_MAIN()
