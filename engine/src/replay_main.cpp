// pjt_replay: runs the engine over an events file (one JSON event per line).
//
//   pjt_replay data/events.jsonl --out data/records.jsonl
//   pjt_replay data/events.jsonl --repeat 5                (speed test)
//   pjt_replay data/events.jsonl --snapshot data/replay.snapshot --stop-after 20000   (pretend to crash)
//   pjt_replay data/events.jsonl --snapshot data/replay.snapshot --resume --out ...    (recover)
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "pjt/config.hpp"
#include "pjt/event.hpp"
#include "pjt/json_sink.hpp"
#include "pjt/tracker.hpp"
#include "pjt/util.hpp"

namespace {

struct Options {
    std::string events_path;
    std::string config_path = "config/engine.ini";
    std::string out_path;
    std::string snapshot_path;
    int repeat = 1;
    bool resume = false;
    std::optional<long long> stop_after;
    bool drain = true;
};

[[noreturn]] void usage(const std::string& problem) {
    std::cerr << "error: " << problem << "\n"
              << "usage: pjt_replay EVENTS.jsonl [--config FILE] [--out FILE] [--repeat N]\n"
              << "                  [--snapshot FILE] [--resume] [--stop-after N] [--no-drain]\n";
    std::exit(2);
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage(a + " needs a value");
            return argv[++i];
        };
        if (a == "--config") o.config_path = value();
        else if (a == "--out") o.out_path = value();
        else if (a == "--snapshot") o.snapshot_path = value();
        else if (a == "--repeat") o.repeat = std::stoi(value());
        else if (a == "--stop-after") o.stop_after = std::stoll(value());
        else if (a == "--resume") o.resume = true;
        else if (a == "--no-drain") o.drain = false;
        else if (!a.empty() && a[0] == '-') usage("unknown option " + a);
        else o.events_path = a;
    }
    if (o.events_path.empty()) usage("missing EVENTS.jsonl");
    if (o.repeat < 1) usage("--repeat must be at least 1");
    if ((o.resume || o.stop_after) && o.snapshot_path.empty()) usage("--resume and --stop-after need --snapshot");
    return o;
}

pjt::EngineConfig load_config_or_default(const std::string& path) {
    std::ifstream probe(path);
    if (!probe) {
        std::cerr << "note: " << path << " not found, using built-in settings\n";
        return {};
    }
    return pjt::load_config(path);
}

std::vector<pjt::Event> read_events(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<pjt::Event> events;
    std::string line, error;
    long long n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (line.empty()) continue;
        auto e = pjt::parse_event(line, &error);
        if (!e) throw std::runtime_error(path + " line " + std::to_string(n) + ": " + error);
        events.push_back(std::move(*e));
    }
    return events;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options opt = parse_args(argc, argv);
        const auto config = load_config_or_default(opt.config_path);
        const auto events = read_events(opt.events_path);

        std::vector<double> rates;
        std::vector<std::int64_t> latencies;
        for (int run = 1; run <= opt.repeat; ++run) {
            pjt::JsonSink sink;
            pjt::Tracker tracker(config, sink);
            std::size_t start = 0;
            if (opt.resume) {
                std::string text;
                if (!pjt::read_file(opt.snapshot_path, text)) throw std::runtime_error("no snapshot at " + opt.snapshot_path);
                start = std::stoull(tracker.restore_json(text));
                std::cerr << "resumed from snapshot: replaying from event " << start + 1 << "\n";
            }
            std::ofstream out;
            const bool last_run = run == opt.repeat;
            if (last_run && !opt.out_path.empty()) {
                out.open(opt.out_path, opt.resume ? std::ios::app : std::ios::trunc);
                if (!out) throw std::runtime_error("cannot write " + opt.out_path);
            }
            auto flush = [&]() {
                for (auto& l : sink.take()) {
                    if (out.is_open()) out << l << '\n';
                }
            };

            latencies.assign(events.size() - start, 0);
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = start; i < events.size(); ++i) {
                const auto a = std::chrono::steady_clock::now();
                tracker.process(events[i]);
                const auto b = std::chrono::steady_clock::now();
                latencies[i - start] = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
                if (sink.size() >= 4096) flush();

                const auto done = static_cast<long long>(i + 1);
                if (!opt.snapshot_path.empty() && config.snapshot_every_events > 0 &&
                    done % config.snapshot_every_events == 0) {
                    flush();  // records first, then the snapshot that covers them
                    if (out.is_open()) out.flush();
                    pjt::write_file_atomic(opt.snapshot_path, tracker.snapshot_json(std::to_string(done)));
                }
                if (opt.stop_after && done >= *opt.stop_after) {
                    flush();
                    std::cerr << "stopped after event " << done << " without saving (pretend crash)\n";
                    return 3;
                }
            }
            if (opt.drain) tracker.drain();
            flush();
            const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const auto n = events.size() - start;
            rates.push_back(secs > 0 ? static_cast<double>(n) / secs : 0);
            if (last_run) {
                const auto& s = tracker.stats();
                std::cerr << "events " << s.events << ", applied " << s.applied << ", duplicates " << s.duplicates
                          << ", held " << s.held << ", stale " << s.stale << "\n";
            }
        }

        std::vector<double> sorted = rates;
        std::sort(sorted.begin(), sorted.end());
        std::cout << "runs: " << opt.repeat << "\n"
                  << "events per run: " << latencies.size() << "\n"
                  << "throughput (median of runs): " << static_cast<long long>(sorted[sorted.size() / 2]) << " events/s"
                  << " (min " << static_cast<long long>(sorted.front()) << ", max "
                  << static_cast<long long>(sorted.back()) << ")\n";
        std::cout << "latency per event (last run): p50 " << pjt::percentile(latencies, 50) << " ns, p99 "
                  << pjt::percentile(latencies, 99) << " ns, max " << pjt::percentile(latencies, 100) << " ns\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
