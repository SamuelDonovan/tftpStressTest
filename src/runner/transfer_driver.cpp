#include "runner/transfer_driver.hpp"

#include <atomic>
#include <fstream>
#include <thread>

namespace tftp_test_harness::runner {

using namespace tftp_test_harness::net;

namespace {

bool write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(stream);
}

} // namespace

DriveResult drive_transfer(const DriveRequest& request) {
    DriveResult result;
    if (request.client == nullptr || request.server == nullptr ||
        request.fixture == nullptr) {
        result.setup_failed = true;
        result.setup_error = "null adapter or fixture";
        return result;
    }

    const std::filesystem::path served = request.work_root / "served";
    const std::filesystem::path local = request.work_root / "local";
    std::error_code ec;
    std::filesystem::create_directories(served, ec);
    std::filesystem::create_directories(local, ec);

    const std::string remote_name = request.remote_filename.empty()
                                        ? request.fixture->name
                                        : request.remote_filename;
    const bool netascii = request.mode == TransferMode::Netascii;

    std::filesystem::path download_destination = local / "download.out";
    std::filesystem::path upload_source = local / "upload_source.bin";

    if (request.direction == verify::TransferDirection::Download) {
        if (request.stage_served_fixture &&
            !write_bytes(served / remote_name, request.fixture->bytes)) {
            result.setup_failed = true;
            result.setup_error = "could not stage served fixture";
            return result;
        }
    } else {
        if (!write_bytes(upload_source, request.fixture->bytes)) {
            result.setup_failed = true;
            result.setup_error = "could not stage upload source";
            return result;
        }
        if (request.plant_existing_destination) {
            // Pre-plant a file so the WRQ hits the "already exists" path (A-12).
            write_bytes(served / remote_name,
                        std::vector<std::uint8_t>{'o', 'l', 'd'});
        }
    }

    // Bring up the server-under-test, then interpose the proxy in front of it.
    EndpointConfiguration real_server = request.server->start(served);
    Endpoint server_endpoint;
    if (!Endpoint::from_host_and_port(real_server.proxy_host,
                                      real_server.proxy_port, server_endpoint)) {
        result.setup_failed = true;
        result.setup_error = "server returned an unparseable endpoint";
        request.server->stop();
        return result;
    }

    ImpairmentProxy proxy;
    ImpairmentProxy::Config proxy_config;
    proxy_config.listen_host = "127.0.0.1";
    proxy_config.server_endpoint = server_endpoint;
    proxy_config.impairment = request.impairment;
    proxy_config.seed = request.seed;
    proxy_config.injection = request.injection;
    if (!proxy.start(proxy_config)) {
        result.setup_failed = true;
        result.setup_error = "impairment proxy failed to bind";
        request.server->stop();
        return result;
    }

    const Endpoint proxy_endpoint = proxy.listen_endpoint();
    EndpointConfiguration client_target;
    client_target.proxy_host = proxy_endpoint.host();
    client_target.proxy_port = proxy_endpoint.port();

    // Run the transfer on its own thread so a hang can be bounded by a watchdog.
    std::atomic<bool> finished{false};
    TransferResult app_result;
    const auto started = std::chrono::steady_clock::now();

    std::thread worker([&]() {
        if (request.direction == verify::TransferDirection::Download) {
            ReadRequestSpecification spec;
            spec.remote_filename = remote_name;
            spec.local_destination_path = download_destination;
            spec.mode = request.mode;
            spec.requested_options = request.options;
            app_result = request.client->perform_read_request(client_target, spec);
        } else {
            WriteRequestSpecification spec;
            spec.local_source_path = upload_source;
            spec.remote_filename = remote_name;
            spec.mode = request.mode;
            spec.requested_options = request.options;
            app_result =
                request.client->perform_write_request(client_target, spec);
        }
        finished.store(true);
    });

    // Wait for completion up to the watchdog.
    while (!finished.load()) {
        if (std::chrono::steady_clock::now() - started > request.watchdog) {
            result.watchdog_fired = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Tearing down the proxy and server removes the transfer's peer, so a client
    // that is still looping will hit its retransmission cap and return; then we
    // can safely join.
    result.trace = proxy.stop_and_take_trace();
    request.server->stop();
    worker.join();

    result.duration_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    result.app_result = app_result;

    // Observe the packet-level behavior.
    verify::ObserverContext observer_context;
    observer_context.direction = request.direction;
    observer_context.block_size = request.effective_block_size;
    observer_context.transfer_reported_complete =
        app_result.completed_successfully;
    result.observer = verify::observe(result.trace, observer_context);

    // Content integrity: compare what was supposed to move against what arrived.
    if (request.direction == verify::TransferDirection::Download) {
        result.integrity = verify::compare_files(served / remote_name,
                                                 download_destination, netascii);
    } else {
        result.integrity =
            verify::compare_files(upload_source, served / remote_name, netascii);
    }

    return result;
}

} // namespace tftp_test_harness::runner
