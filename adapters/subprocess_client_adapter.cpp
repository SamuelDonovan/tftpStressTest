#include "subprocess_client_adapter.hpp"

#include <chrono>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <ctime>
#endif

namespace tftp_test_harness {

#if !defined(_WIN32)

SubprocessResult run_subprocess(const std::vector<std::string>& argv,
                                const std::string& working_directory,
                                std::chrono::milliseconds timeout) {
    SubprocessResult result;
    if (argv.empty()) {
        result.launch_failed = true;
        return result;
    }

    int output_pipe[2];
    if (::pipe(output_pipe) != 0) {
        result.launch_failed = true;
        return result;
    }

    const auto start = std::chrono::steady_clock::now();
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        result.launch_failed = true;
        return result;
    }

    if (child == 0) {
        // Child: redirect stdout+stderr to the pipe, chdir, exec.
        ::dup2(output_pipe[1], 1);
        ::dup2(output_pipe[1], 2);
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        if (!working_directory.empty()) {
            if (::chdir(working_directory.c_str()) != 0) {
                ::_exit(127);
            }
        }
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (const auto& argument : argv) {
            raw.push_back(const_cast<char*>(argument.c_str()));
        }
        raw.push_back(nullptr);
        ::execvp(raw[0], raw.data());
        ::_exit(127); // exec failed
    }

    // Parent: read output while polling for completion, enforcing the timeout.
    ::close(output_pipe[1]);
    std::string captured;
    char buffer[1024];
    bool finished = false;
    int status = 0;
    while (true) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            finished = true;
        } else {
            // Drain any pending output without blocking indefinitely.
            timespec nap{0, 5 * 1000 * 1000};
            ::nanosleep(&nap, nullptr);
        }
        ssize_t got;
        while ((got = ::read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
            captured.append(buffer, static_cast<std::size_t>(got));
        }
        if (finished) break;
        if (std::chrono::steady_clock::now() - start > timeout) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            result.timed_out = true;
            break;
        }
    }
    ::close(output_pipe[0]);

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    result.captured_output = std::move(captured);
    if (!result.timed_out) {
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return result;
}

#else // _WIN32

SubprocessResult run_subprocess(const std::vector<std::string>&,
                                const std::string&,
                                std::chrono::milliseconds) {
    // The portable core is the socket shim; CLI-binary launching on Windows is
    // left to an integrator's CreateProcess variant. In-process adapters (the
    // reference implementations) work on every platform.
    SubprocessResult result;
    result.launch_failed = true;
    return result;
}

#endif

TransferResult SubprocessClientAdapter::perform_read_request(
    const EndpointConfiguration& endpoint,
    const ReadRequestSpecification& specification) {
    const auto argv = build_read_arguments(endpoint.proxy_host,
                                           endpoint.proxy_port, specification);
    const auto working_directory =
        specification.local_destination_path.parent_path().string();
    const SubprocessResult run =
        run_subprocess(argv, working_directory, transfer_timeout());

    TransferResult result;
    result.process_exit_code = run.exit_code;
    result.wall_clock_duration = run.duration;
    result.reported_error_message = run.captured_output;
    // A CLI TFTP client conventionally exits 0 on success. The harness's content
    // oracle independently verifies the delivered bytes regardless.
    result.completed_successfully = !run.timed_out && !run.launch_failed &&
                                    run.exit_code == 0;
    return result;
}

TransferResult SubprocessClientAdapter::perform_write_request(
    const EndpointConfiguration& endpoint,
    const WriteRequestSpecification& specification) {
    const auto argv = build_write_arguments(endpoint.proxy_host,
                                            endpoint.proxy_port, specification);
    const auto working_directory =
        specification.local_source_path.parent_path().string();
    const SubprocessResult run =
        run_subprocess(argv, working_directory, transfer_timeout());

    TransferResult result;
    result.process_exit_code = run.exit_code;
    result.wall_clock_duration = run.duration;
    result.reported_error_message = run.captured_output;
    result.completed_successfully = !run.timed_out && !run.launch_failed &&
                                    run.exit_code == 0;
    return result;
}

} // namespace tftp_test_harness
