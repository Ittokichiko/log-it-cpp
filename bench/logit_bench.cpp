#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "LatencyRecorder.hpp"
#include "Scenario.hpp"
#include "adapters/LogItAdapter.hpp"

#ifdef LOGIT_BENCH_HAVE_SPDLOG
#include "adapters/SpdlogAdapter.hpp"
#endif

namespace logit_bench {
namespace {

std::string make_message(std::size_t bytes, std::size_t index) {
    if (bytes == 0) return {};
    const char fill = static_cast<char>('A' + static_cast<int>(index % 26));
    return std::string(bytes, fill);
}

std::size_t get_env_size_t(const char* name, std::size_t def) {
    if (const char* v = std::getenv(name)) {
        try {
            return static_cast<std::size_t>(std::stoull(v));
        } catch (...) {
            // fallthrough
        }
    }
    return def;
}

/**
 * Run a workload:
 *  - producers start together (barrier),
 *  - each producer logs its portion of total_messages,
 *  - LatencyRecorder::begin(record) captures t0 and slot,
 *  - adapter.log(token, message) must eventually call recorder.complete(token) from sink/consumer,
 *  - returns total wall duration (for throughput).
 */
std::chrono::nanoseconds run_workload(
        ILoggerAdapter& adapter,
        LatencyRecorder& recorder,
        const Scenario& scenario,
        std::size_t total_messages,
        bool record_latency,
        bool measure_duration)
{
    if (scenario.producers == 0) {
        adapter.flush();
        return std::chrono::nanoseconds(0);
    }

    // Distribute messages across producers.
    std::vector<std::size_t> per_thread(scenario.producers, 0);
    const std::size_t base = total_messages / scenario.producers;
    std::size_t rem = total_messages % scenario.producers;
    for (std::size_t i = 0; i < scenario.producers; ++i) {
        per_thread[i] = base + (rem ? 1 : 0);
        if (rem) --rem;
    }

    // Barrier to start together.
    std::mutex start_mx;
    std::condition_variable start_cv;
    bool start_flag = false;
    std::size_t ready = 0;

    std::vector<std::thread> threads;
    threads.reserve(scenario.producers);

    for (std::size_t i = 0; i < scenario.producers; ++i) {
        threads.emplace_back([&, i]() {
            std::string message = make_message(scenario.message_bytes, i);
            {
                std::unique_lock<std::mutex> lk(start_mx);
                ++ready;
                if (ready == scenario.producers) start_cv.notify_one();
                start_cv.wait(lk, [&]{ return start_flag; });
            }
            for (std::size_t n = 0; n < per_thread[i]; ++n) {
                auto token = recorder.begin(record_latency);
                adapter.log(token, message);
            }
        });
    }

    std::chrono::steady_clock::time_point t0;
    {
        std::unique_lock<std::mutex> lk(start_mx);
        start_cv.wait(lk, [&]{ return ready == scenario.producers; });
        if (measure_duration) t0 = std::chrono::steady_clock::now();
        start_flag = true;
        start_cv.notify_all();
    }

    for (auto& th : threads) th.join();
    adapter.flush();

    if (!measure_duration) return std::chrono::nanoseconds(0);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
}

struct ScenarioResult {
    LatencyRecorder::Summary summary;
    double throughput = 0.0;
    std::chrono::nanoseconds duration{0};
};

ScenarioResult execute_scenario(
        ILoggerAdapter& adapter,
        const Scenario& scenario,
        std::size_t warmup_messages)
{
    LatencyRecorder recorder(scenario.total_messages);

    // Adapter should keep a pointer/ref to recorder and call complete(token) from its sink.
    adapter.prepare(scenario, recorder);

    // Warm-up (no recording, no duration).
    std::cout << "[logit_bench] Warm-up start lib=" << adapter.library_name()
              << " async=" << (scenario.async ? '1' : '0')
              << " sink=" << sink_name(scenario.sink)
              << " producers=" << scenario.producers
              << " bytes=" << scenario.message_bytes
              << " total=" << warmup_messages << std::endl;
    run_workload(adapter, recorder, scenario, warmup_messages, false, false);
    std::cout << "[logit_bench] Warm-up completed lib=" << adapter.library_name()
              << " async=" << (scenario.async ? '1' : '0')
              << " sink=" << sink_name(scenario.sink)
              << " producers=" << scenario.producers
              << " bytes=" << scenario.message_bytes
              << std::endl;

    // Measured run.
    std::cout << "[logit_bench] Measure start lib=" << adapter.library_name()
              << " async=" << (scenario.async ? '1' : '0')
              << " sink=" << sink_name(scenario.sink)
              << " producers=" << scenario.producers
              << " bytes=" << scenario.message_bytes
              << " total=" << scenario.total_messages << std::endl;
    const auto dur = run_workload(adapter, recorder, scenario, scenario.total_messages, true, true);
    std::cout << "[logit_bench] Measure completed lib=" << adapter.library_name()
              << " async=" << (scenario.async ? '1' : '0')
              << " sink=" << sink_name(scenario.sink)
              << " producers=" << scenario.producers
              << " bytes=" << scenario.message_bytes
              << std::endl;
    const auto sum = recorder.finalize();

    double thr = 0.0;
    if (dur.count() > 0) {
        const double sec = static_cast<double>(dur.count()) / 1'000'000'000.0;
        thr = static_cast<double>(scenario.total_messages) / sec;
    }
    return ScenarioResult{sum, thr, dur};
}

void append_csv(
        const std::string& library,
        const Scenario& scenario,
        const LatencyRecorder::Summary& summary,
        double throughput)
{
    namespace fs = std::filesystem;
    const fs::path csv_path{"bench/results/latency.csv"};
    fs::create_directories(csv_path.parent_path());

    const bool write_header = !fs::exists(csv_path) || fs::file_size(csv_path) == 0;

    std::ofstream out(csv_path, std::ios::app);
    if (!out) throw std::runtime_error("Failed to open latency.csv for writing");

    if (write_header) {
        out << "lib,async,sink,producers,msg_bytes,total,p50_ns,p99_ns,p999_ns,throughput\n";
    }
    out << library << ','
        << (scenario.async ? 1 : 0) << ','
        << sink_name(scenario.sink) << ','
        << scenario.producers << ','
        << scenario.message_bytes << ','
        << scenario.total_messages << ','
        << summary.p50_ns << ','
        << summary.p99_ns << ','
        << summary.p999_ns << ','
        << std::fixed << std::setprecision(2) << throughput << '\n';
}

void print_summary(
        const std::string& library,
        const Scenario& scenario,
        const ScenarioResult& result)
{
    std::cout << library
              << " async=" << (scenario.async ? '1' : '0')
              << " sink=" << sink_name(scenario.sink)
              << " producers=" << scenario.producers
              << " bytes=" << scenario.message_bytes
              << " total=" << scenario.total_messages
              << " p50=" << result.summary.p50_ns
              << "ns p99=" << result.summary.p99_ns
              << "ns p999=" << result.summary.p999_ns
              << "ns throughput=" << std::fixed << std::setprecision(2)
              << result.throughput << " msg/s\n";
}

} // namespace
} // namespace logit_bench

int main() {
    using namespace logit_bench;
    std::atomic<bool> watchdog_done{false};
    std::thread watchdog;
    try {
        std::vector<std::unique_ptr<ILoggerAdapter>> adapters;
        adapters.emplace_back(std::make_unique<LogItAdapter>());
#ifdef LOGIT_BENCH_HAVE_SPDLOG
        adapters.emplace_back(std::make_unique<SpdlogAdapter>());
#endif

        // Matrix
        const std::array<bool, 2> async_modes{false, true};
        const std::array<SinkKind, 2> sinks{SinkKind::Null, SinkKind::File};
        const std::array<std::size_t, 3> producer_counts{1, 4, 16};
        const std::array<std::size_t, 3> message_sizes{40, 200, 1024};

        // Totals (can be overridden by env):
        const std::size_t total_messages  = get_env_size_t("LOGIT_BENCH_TOTAL", 200000);
        const std::size_t warmup_messages = get_env_size_t("LOGIT_BENCH_WARMUP", 4096);
        const std::size_t timeout_seconds = get_env_size_t("LOGIT_BENCH_TIMEOUT_SEC", 600);

        if (timeout_seconds > 0) {
            watchdog = std::thread([timeout_seconds, &watchdog_done]() {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
                while (!watchdog_done.load(std::memory_order_relaxed)) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        std::cerr << "[logit_bench] Timeout reached after "
                                  << timeout_seconds
                                  << " seconds. Terminating benchmark." << std::endl;
                        std::cerr.flush();
                        std::_Exit(124);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            });
        }

        for (auto& adapter : adapters) {
            for (bool async_mode : async_modes) {
                for (auto sink : sinks) {
                    for (std::size_t producers : producer_counts) {
                        for (std::size_t msg_bytes : message_sizes) {
                            Scenario scenario;
                            scenario.async          = async_mode;
                            scenario.sink           = sink;
                            scenario.producers      = producers;
                            scenario.message_bytes  = msg_bytes;
                            scenario.total_messages = total_messages;

                            std::cout << "[logit_bench] Scenario start lib=" << adapter->library_name()
                                      << " async=" << (scenario.async ? '1' : '0')
                                      << " sink=" << sink_name(scenario.sink)
                                      << " producers=" << scenario.producers
                                      << " bytes=" << scenario.message_bytes
                                      << " total=" << scenario.total_messages << std::endl;
                            auto result = execute_scenario(*adapter, scenario, warmup_messages);
                            append_csv(adapter->library_name(), scenario, result.summary, result.throughput);
                            print_summary(adapter->library_name(), scenario, result);
                        }
                    }
                }
            }
        }
        watchdog_done.store(true, std::memory_order_relaxed);
        if (watchdog.joinable()) watchdog.join();
    } catch (const std::exception& ex) {
        watchdog_done.store(true, std::memory_order_relaxed);
        if (watchdog.joinable()) watchdog.join();
        std::cerr << "Benchmark failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
