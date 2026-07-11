#ifndef TFTP_TEST_HARNESS_METRICS_JSON_WRITER_HPP
#define TFTP_TEST_HARNESS_METRICS_JSON_WRITER_HPP

// ---------------------------------------------------------------------------
// A tiny self-contained JSON writer. The metrics store and report generator
// need to emit and (minimally) re-read JSON, and the "no third-party
// dependency" constraint rules out a JSON library. This writer builds a small
// in-memory value tree and serializes it with correct string escaping; a
// matching minimal parser lives in json_reader for comparison mode.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tftp_test_harness::metrics {

// A JSON value: null, bool, number, string, array, or object. Object key order
// is preserved (insertion order) so diffs and reports read naturally.
class JsonValue {
public:
    enum class Type { Null, Boolean, Number, String, Array, Object };

    JsonValue() : type_(Type::Null) {}
    static JsonValue boolean(bool value);
    static JsonValue number(double value);
    static JsonValue integer(long long value);
    static JsonValue string(std::string value);
    static JsonValue array();
    static JsonValue object();

    Type type() const { return type_; }

    // Object / array mutation.
    JsonValue& set(const std::string& key, JsonValue value); // object
    JsonValue& append(JsonValue value);                      // array

    // Serialize. pretty=true indents; false is compact (one line, used for the
    // newline-delimited metrics store).
    std::string serialize(bool pretty = false) const;

    // Accessors (used by the comparison reader-side helpers).
    bool as_bool() const { return boolean_; }
    double as_number() const { return number_; }
    long long as_integer() const { return static_cast<long long>(number_); }
    const std::string& as_string() const { return string_; }
    const std::vector<JsonValue>& as_array() const { return array_; }
    bool has(const std::string& key) const;
    const JsonValue& at(const std::string& key) const;
    const std::vector<std::pair<std::string, JsonValue>>& members() const {
        return object_;
    }

private:
    void serialize_into(std::string& out, bool pretty, int depth) const;

    Type type_;
    bool boolean_ = false;
    double number_ = 0.0;
    bool number_is_integer_ = false;
    std::string string_;
    std::vector<JsonValue> array_;
    std::vector<std::pair<std::string, JsonValue>> object_;
};

// Escape a raw string as a JSON string literal (including surrounding quotes).
std::string escape_json_string(const std::string& raw);

// Parse a JSON document. Returns false on malformed input. Supports the subset
// the metrics store emits (objects, arrays, strings, numbers, booleans, null) —
// enough for the report and comparison modes to re-read a store.
bool parse_json(const std::string& text, JsonValue& out);

} // namespace tftp_test_harness::metrics

#endif // TFTP_TEST_HARNESS_METRICS_JSON_WRITER_HPP
