#include "reference/reference_adapters.hpp"

#include "net/endpoint.hpp"

#include <chrono>

namespace tftp_test_harness::reference {

using namespace tftp_test_harness::net;

EngineConfig make_engine_config(ReferencePersonality personality) {
    EngineConfig config;
    // Loopback RTT is microseconds, so a fast retransmission timeout keeps the
    // adversarial sweeps bounded while remaining far above the real round trip.
    config.retransmission_timeout = std::chrono::milliseconds(400);
    config.maximum_retransmissions = 8;
    switch (personality) {
        case ReferencePersonality::Correct:
            break; // all defaults are correct behavior
        case ReferencePersonality::Buggy:
            // The named defects the self-verification proves are detected.
            config.quirks.retransmit_on_duplicate_ack = true;  // SAS cascade
            config.quirks.omit_terminating_empty_block = true; // final-block bug
            config.quirks.accept_out_of_sequence_data = true;  // silent corruption
            break;
        case ReferencePersonality::OptionsUnsupported:
            // A conformant base-protocol-only implementation (pre-RFC-2347).
            config.support_option_negotiation = false;
            config.support_block_size_option = false;
            config.support_timeout_option = false;
            config.support_transfer_size_option = false;
            config.support_window_size_option = false;
            break;
    }
    return config;
}

std::string personality_name(ReferencePersonality personality) {
    switch (personality) {
        case ReferencePersonality::Correct:
            return "reference-correct";
        case ReferencePersonality::Buggy:
            return "reference-buggy";
        case ReferencePersonality::OptionsUnsupported:
            return "reference-base-only";
    }
    return "reference-unknown";
}

namespace {

std::string mode_string(TransferMode mode) {
    return mode == TransferMode::Netascii ? "netascii" : "octet";
}

ClientRequestedOptions convert_options(const RequestedOptions& requested) {
    ClientRequestedOptions options;
    options.block_size = requested.block_size;
    options.timeout_seconds = requested.timeout_seconds;
    options.transfer_size = requested.transfer_size;
    options.window_size = requested.window_size;
    return options;
}

// Common capability query shared by both adapters: a false return causes the
// dependent tests to be recorded SKIPPED (unsupported) rather than FAILED.
bool config_supports(const EngineConfig& config,
                     const std::string& capability_name) {
    if (capability_name == capability::read_request ||
        capability_name == capability::write_request ||
        capability_name == capability::netascii_mode) {
        return capability_name != capability::netascii_mode ||
               config.support_netascii;
    }
    if (capability_name == capability::option_negotiation) {
        return config.support_option_negotiation;
    }
    if (capability_name == capability::block_size) {
        return config.support_block_size_option;
    }
    if (capability_name == capability::timeout_option) {
        return config.support_timeout_option;
    }
    if (capability_name == capability::transfer_size) {
        return config.support_transfer_size_option;
    }
    if (capability_name == capability::window_size) {
        return config.support_window_size_option;
    }
    return false;
}

TransferResult to_transfer_result(const TransferOutcome& outcome,
                                  std::chrono::milliseconds duration) {
    TransferResult result;
    result.completed_successfully = outcome.completed_successfully;
    if (outcome.error_code) {
        result.reported_error_code =
            static_cast<std::uint16_t>(*outcome.error_code);
    }
    result.reported_error_message = outcome.error_message;
    result.wall_clock_duration = duration;
    result.options_confirmed_in_use.block_size = outcome.negotiated.block_size;
    if (outcome.negotiated.window_size > 1) {
        result.options_confirmed_in_use.window_size =
            outcome.negotiated.window_size;
    }
    if (outcome.negotiated.timeout_seconds) {
        result.options_confirmed_in_use.timeout_seconds =
            *outcome.negotiated.timeout_seconds;
    }
    if (outcome.negotiated.transfer_size) {
        result.options_confirmed_in_use.transfer_size =
            *outcome.negotiated.transfer_size;
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Client adapter
// ---------------------------------------------------------------------------
std::string ReferenceClientAdapter::implementation_name() const {
    return personality_name(personality_) + "-client";
}

bool ReferenceClientAdapter::supports_capability(
    const std::string& capability_name) const {
    return config_supports(config_, capability_name);
}

TransferResult ReferenceClientAdapter::perform_read_request(
    const EndpointConfiguration& endpoint,
    const ReadRequestSpecification& specification) {
    Endpoint server;
    Endpoint::from_host_and_port(endpoint.proxy_host, endpoint.proxy_port,
                                 server);
    ReferenceClient client(config_);
    const auto start = std::chrono::steady_clock::now();
    TransferOutcome outcome = client.read_file(
        server, specification.remote_filename,
        specification.local_destination_path, mode_string(specification.mode),
        convert_options(specification.requested_options));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return to_transfer_result(outcome, elapsed);
}

TransferResult ReferenceClientAdapter::perform_write_request(
    const EndpointConfiguration& endpoint,
    const WriteRequestSpecification& specification) {
    Endpoint server;
    Endpoint::from_host_and_port(endpoint.proxy_host, endpoint.proxy_port,
                                 server);
    ReferenceClient client(config_);
    const auto start = std::chrono::steady_clock::now();
    TransferOutcome outcome = client.write_file(
        server, specification.local_source_path, specification.remote_filename,
        mode_string(specification.mode),
        convert_options(specification.requested_options));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return to_transfer_result(outcome, elapsed);
}

// ---------------------------------------------------------------------------
// Server adapter
// ---------------------------------------------------------------------------
std::string ReferenceServerAdapter::implementation_name() const {
    return personality_name(personality_) + "-server";
}

bool ReferenceServerAdapter::supports_capability(
    const std::string& capability_name) const {
    return config_supports(config_, capability_name);
}

EndpointConfiguration ReferenceServerAdapter::start(
    const std::filesystem::path& served_directory) {
    server_ = std::make_unique<ReferenceServer>(config_);
    const Endpoint bound = server_->start(served_directory, "127.0.0.1");
    EndpointConfiguration configuration;
    configuration.proxy_host = bound.host();
    configuration.proxy_port = bound.port();
    return configuration;
}

void ReferenceServerAdapter::stop() {
    if (server_) {
        server_->stop();
        server_.reset();
    }
}

} // namespace tftp_test_harness::reference
