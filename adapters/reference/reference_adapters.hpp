#ifndef TFTP_TEST_HARNESS_REFERENCE_REFERENCE_ADAPTERS_HPP
#define TFTP_TEST_HARNESS_REFERENCE_REFERENCE_ADAPTERS_HPP

// ---------------------------------------------------------------------------
// Adapters that plug the in-process reference engine into the harness's single
// plug-in interface (include/tftp_test_harness/adapter_interface.hpp).
//
// Three configurations are provided as factory helpers:
//   * correct   — a fully conformant implementation; must pass the whole matrix.
//   * buggy      — the correct engine with named defects switched on (Sorcerer's
//                  Apprentice cascade + broken final-block rule + out-of-sequence
//                  acceptance). Self-verification asserts the harness flags
//                  exactly these.
//   * options_off — a conformant base-protocol-only implementation that declines
//                  RFC 2347 option negotiation, so option tests report SKIPPED.
// ---------------------------------------------------------------------------

#include "reference/tftp_reference_engine.hpp"
#include "tftp_test_harness/adapter_interface.hpp"

#include <memory>
#include <string>

namespace tftp_test_harness::reference {

// Distinguishes the three shipped reference personalities in the report.
enum class ReferencePersonality {
    Correct,
    Buggy,
    OptionsUnsupported,
};

EngineConfig make_engine_config(ReferencePersonality personality);
std::string personality_name(ReferencePersonality personality);

// ---------------------------------------------------------------------------
// Client adapter
// ---------------------------------------------------------------------------
class ReferenceClientAdapter : public ClientAdapter {
public:
    explicit ReferenceClientAdapter(
        ReferencePersonality personality = ReferencePersonality::Correct)
        : personality_(personality),
          config_(make_engine_config(personality)) {}

    std::string implementation_name() const override;
    std::string implementation_version() const override { return "1.0.0"; }
    bool supports_capability(const std::string& capability_name) const override;

    TransferResult perform_read_request(
        const EndpointConfiguration& endpoint,
        const ReadRequestSpecification& specification) override;

    TransferResult perform_write_request(
        const EndpointConfiguration& endpoint,
        const WriteRequestSpecification& specification) override;

private:
    ReferencePersonality personality_;
    EngineConfig config_;
};

// ---------------------------------------------------------------------------
// Server adapter
// ---------------------------------------------------------------------------
class ReferenceServerAdapter : public ServerAdapter {
public:
    explicit ReferenceServerAdapter(
        ReferencePersonality personality = ReferencePersonality::Correct)
        : personality_(personality),
          config_(make_engine_config(personality)) {}

    std::string implementation_name() const override;
    std::string implementation_version() const override { return "1.0.0"; }
    bool supports_capability(const std::string& capability_name) const override;

    EndpointConfiguration start(
        const std::filesystem::path& served_directory) override;
    void stop() override;

private:
    ReferencePersonality personality_;
    EngineConfig config_;
    std::unique_ptr<ReferenceServer> server_;
};

} // namespace tftp_test_harness::reference

#endif // TFTP_TEST_HARNESS_REFERENCE_REFERENCE_ADAPTERS_HPP
