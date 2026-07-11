#include "metrics/metrics_store.hpp"

#include <fstream>

namespace tftp_test_harness::metrics {

const char* outcome_name(Outcome outcome) {
    switch (outcome) {
        case Outcome::Pass: return "PASS";
        case Outcome::Fail: return "FAIL";
        case Outcome::Skipped: return "SKIPPED";
    }
    return "UNKNOWN";
}

const char* severity_name(Severity severity) {
    switch (severity) {
        case Severity::Critical: return "CRITICAL";
        case Severity::Major: return "MAJOR";
        case Severity::Minor: return "MINOR";
        case Severity::Info: return "INFO";
    }
    return "UNKNOWN";
}

Outcome outcome_from_name(const std::string& name) {
    if (name == "PASS") return Outcome::Pass;
    if (name == "FAIL") return Outcome::Fail;
    return Outcome::Skipped;
}

Severity severity_from_name(const std::string& name) {
    if (name == "CRITICAL") return Severity::Critical;
    if (name == "MAJOR") return Severity::Major;
    if (name == "MINOR") return Severity::Minor;
    return Severity::Info;
}

JsonValue MetricRecord::to_json() const {
    JsonValue record = JsonValue::object();
    record.set("type", JsonValue::string("metric"));
    record.set("test_id", JsonValue::string(test_id));
    record.set("title", JsonValue::string(title));
    record.set("rfc_clause", JsonValue::string(rfc_clause));
    record.set("severity", JsonValue::string(severity_name(severity)));
    record.set("outcome", JsonValue::string(outcome_name(outcome)));
    if (intensity.has_value()) {
        record.set("intensity", JsonValue::number(*intensity));
    }
    record.set("intensity_label", JsonValue::string(intensity_label));
    record.set("seed", JsonValue::integer(static_cast<long long>(seed)));
    record.set("narrative", JsonValue::string(narrative));
    record.set("integrity_violation", JsonValue::boolean(integrity_violation));
    record.set("ungraceful_failure", JsonValue::boolean(ungraceful_failure));
    record.set("implementation_name", JsonValue::string(implementation_name));
    JsonValue counters_json = JsonValue::object();
    for (const auto& counter : counters) {
        counters_json.set(counter.first, JsonValue::number(counter.second));
    }
    record.set("counters", std::move(counters_json));
    return record;
}

MetricRecord MetricRecord::from_json(const JsonValue& value) {
    MetricRecord record;
    if (value.has("test_id")) record.test_id = value.at("test_id").as_string();
    if (value.has("title")) record.title = value.at("title").as_string();
    if (value.has("rfc_clause"))
        record.rfc_clause = value.at("rfc_clause").as_string();
    if (value.has("severity"))
        record.severity = severity_from_name(value.at("severity").as_string());
    if (value.has("outcome"))
        record.outcome = outcome_from_name(value.at("outcome").as_string());
    if (value.has("intensity") &&
        value.at("intensity").type() == JsonValue::Type::Number) {
        record.intensity = value.at("intensity").as_number();
    }
    if (value.has("intensity_label"))
        record.intensity_label = value.at("intensity_label").as_string();
    if (value.has("seed"))
        record.seed = static_cast<std::uint64_t>(value.at("seed").as_integer());
    if (value.has("narrative"))
        record.narrative = value.at("narrative").as_string();
    if (value.has("integrity_violation"))
        record.integrity_violation = value.at("integrity_violation").as_bool();
    if (value.has("ungraceful_failure"))
        record.ungraceful_failure = value.at("ungraceful_failure").as_bool();
    if (value.has("implementation_name"))
        record.implementation_name =
            value.at("implementation_name").as_string();
    if (value.has("counters")) {
        for (const auto& member : value.at("counters").members()) {
            record.counters[member.first] = member.second.as_number();
        }
    }
    return record;
}

bool MetricsStore::open(const std::string& path,
                        const std::string& implementation_name) {
    path_ = path;
    implementation_name_ = implementation_name;
    records_.clear();
    std::ofstream stream(path_, std::ios::trunc);
    if (!stream) {
        return false;
    }
    // Header line: a self-describing record so the store is stand-alone.
    JsonValue header = JsonValue::object();
    header.set("type", JsonValue::string("header"));
    header.set("format", JsonValue::string("tftp-test-harness-metrics/1"));
    header.set("implementation_name", JsonValue::string(implementation_name));
    stream << header.serialize(false) << "\n";
    open_ = true;
    return true;
}

void MetricsStore::append(const MetricRecord& record) {
    records_.push_back(record);
    if (!open_) {
        return;
    }
    // Append + flush per record so a crash mid-run keeps completed results.
    std::ofstream stream(path_, std::ios::app);
    if (stream) {
        stream << record.to_json().serialize(false) << "\n";
        stream.flush();
    }
}

void MetricsStore::close() { open_ = false; }

MetricsStore::~MetricsStore() { close(); }

bool MetricsStore::load(const std::string& path, MetricsStore& out) {
    std::ifstream stream(path);
    if (!stream) {
        return false;
    }
    out.path_ = path;
    out.records_.clear();
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        JsonValue value;
        if (!parse_json(line, value)) {
            continue; // tolerate a partially written final line
        }
        if (!value.has("type")) continue;
        const std::string type = value.at("type").as_string();
        if (type == "header") {
            if (value.has("implementation_name")) {
                out.implementation_name_ =
                    value.at("implementation_name").as_string();
            }
        } else if (type == "metric") {
            out.records_.push_back(MetricRecord::from_json(value));
        }
    }
    return true;
}

} // namespace tftp_test_harness::metrics
