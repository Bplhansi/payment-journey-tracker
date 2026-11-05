// pjt_stream: the live engine. Reads events from a Redis stream as part of a consumer group,
// and writes alerts, state updates and metrics to another stream. Each input entry has one
// field, "data", holding one event as JSON. Each output entry has one field, "records",
// holding one or more JSON records separated by newlines.
//
// Crash safety, in short:
//   * About once a second it saves a snapshot that records the last stream ID it covers.
//   * Only then does it acknowledge (XACK) those events. Unacknowledged events stay in Redis.
//   * On restart it loads the snapshot and re-reads its unacknowledged events. Any it already
//     handled are skipped or ignored as duplicates, so the final result matches a clean run.
//
//   pjt_stream --redis tcp://127.0.0.1:6379 --config config/engine.ini --snapshot data/engine.snapshot
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include <sw/redis++/redis++.h>
#include <nlohmann/json.hpp>

#include "pjt/config.hpp"
#include "pjt/event.hpp"
#include "pjt/json_sink.hpp"
#include "pjt/tracker.hpp"
#include "pjt/util.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct Options {
    std::string redis_url = "tcp://127.0.0.1:6379";
    std::string config_path = "config/engine.ini";
    std::string snapshot_path = "data/engine.snapshot";
    std::string in_stream = "pjt:events";
    std::string out_stream = "pjt:out";
    std::string group = "engine";
    std::string consumer = "engine-1";
    long long idle_after_ms = 2000;  // no events for this long: let the clock run on wall time
};

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : fallback;
}

Options parse_args(int argc, char** argv) {
    Options o;
    o.redis_url = env_or("REDIS_URL", o.redis_url);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << a << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--redis") o.redis_url = value();
        else if (a == "--config") o.config_path = value();
        else if (a == "--snapshot") o.snapshot_path = value();
        else if (a == "--in") o.in_stream = value();
        else if (a == "--out") o.out_stream = value();
        else if (a == "--consumer") o.consumer = value();
        else if (a == "--idle-after-ms") o.idle_after_ms = std::stoll(value());
        else {
            std::cerr << "unknown option " << a << "\n"
                      << "usage: pjt_stream [--redis URL] [--config FILE] [--snapshot FILE] [--in STREAM] [--out STREAM]\n";
            std::exit(2);
        }
    }
    // Accept the redis:// form the Python side uses, too.
    if (o.redis_url.rfind("redis://", 0) == 0) o.redis_url = "tcp://" + o.redis_url.substr(8);
    return o;
}

// Stream IDs look like "1793700000000-3": milliseconds, then a sequence number.
std::pair<long long, long long> split_id(const std::string& id) {
    const auto dash = id.find('-');
    if (dash == std::string::npos) return {std::stoll(id), 0};
    return {std::stoll(id.substr(0, dash)), std::stoll(id.substr(dash + 1))};
}

bool id_after(const std::string& a, const std::string& b) {  // is a later than b?
    if (b.empty()) return true;
    return split_id(a) > split_id(b);
}

long long wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Events waiting in the queue for this group (Redis 7+ reports it as "lag"). -1 if unknown.
long long backlog(sw::redis::Redis& redis, const Options& o) {
    try {
        auto reply = redis.command("XINFO", "GROUPS", o.in_stream);
        if (!reply || reply->type != REDIS_REPLY_ARRAY) return -1;
        for (size_t g = 0; g < reply->elements; ++g) {
            const redisReply* grp = reply->element[g];
            std::string name;
            long long lag = -1;
            for (size_t k = 0; k + 1 < grp->elements; k += 2) {
                const std::string key(grp->element[k]->str, grp->element[k]->len);
                const redisReply* v = grp->element[k + 1];
                if (key == "name" && v->str) name.assign(v->str, v->len);
                if (key == "lag" && v->type == REDIS_REPLY_INTEGER) lag = v->integer;
            }
            if (name == o.group) return lag;
        }
    } catch (const sw::redis::Error&) {
    }
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const Options opt = parse_args(argc, argv);
    try {
        std::ifstream probe(opt.config_path);
        const auto config = probe ? pjt::load_config(opt.config_path) : pjt::EngineConfig{};
        sw::redis::Redis redis(opt.redis_url);

        pjt::JsonSink sink;
        pjt::Tracker tracker(config, sink);
        std::string covered;  // last stream ID inside the saved snapshot
        std::string text;
        if (pjt::read_file(opt.snapshot_path, text)) {
            covered = tracker.restore_json(text);
            std::cerr << "loaded snapshot covering stream ID " << covered << "\n";
        }
        try {
            redis.xgroup_create(opt.in_stream, opt.group, "0", true);  // read from the very start
        } catch (const sw::redis::ReplyError&) {
            // BUSYGROUP: the group already exists, which is normal after a restart
        }

        using Attrs = std::vector<std::pair<std::string, std::string>>;
        using Item = std::pair<std::string, sw::redis::Optional<Attrs>>;
        using ItemStream = std::vector<Item>;

        std::vector<std::string> unacked;  // handled, but not yet inside a saved snapshot
        std::string last_id = covered;
        bool recovering = true;                // first re-read our own unacknowledged events
        std::string recover_from = "0";
        long long last_event_wall = wall_ms(), last_tick_wall = wall_ms(), last_metrics_wall = wall_ms();
        long long last_snapshot_wall = wall_ms();
        long long events_since_metrics = 0;
        std::vector<std::int64_t> lat_ns, delay_ms;

        // Output goes out in batches: each stream entry has one field, "records", holding up to
        // 1,000 JSON records separated by newlines. One write per batch instead of one per
        // record is what lets the engine keep up with high event rates.
        auto publish = [&]() {
            auto lines = sink.take();
            if (lines.empty()) return;
            auto pipe = redis.pipeline(false);
            for (std::size_t i = 0; i < lines.size(); i += 1000) {
                std::string joined;
                for (std::size_t k = i; k < std::min(lines.size(), i + 1000); ++k) {
                    if (k > i) joined += '\n';
                    joined += lines[k];
                }
                const std::pair<std::string, std::string> field{"records", std::move(joined)};
                pipe.xadd(opt.out_stream, "*", &field, &field + 1);
            }
            pipe.exec();
        };
        auto checkpoint = [&]() {
            if (unacked.empty()) return;
            publish();  // records first, so nothing is acknowledged before it is written out
            pjt::write_file_atomic(opt.snapshot_path, tracker.snapshot_json(last_id));
            redis.xack(opt.in_stream, opt.group, unacked.begin(), unacked.end());
            unacked.clear();
        };

        std::cerr << "pjt_stream: reading " << opt.in_stream << " as " << opt.group << "/" << opt.consumer << "\n";
        while (!g_stop) {
            std::unordered_map<std::string, ItemStream> got;
            if (recovering) {
                redis.xreadgroup(opt.group, opt.consumer, opt.in_stream, recover_from, 1000,
                                 std::inserter(got, got.end()));
            } else {
                redis.xreadgroup(opt.group, opt.consumer, opt.in_stream, ">", std::chrono::milliseconds(500), 1000,
                                 std::inserter(got, got.end()));
            }
            std::size_t n = 0;
            std::vector<std::string> ack_now;
            for (auto& [stream, items] : got) {
                for (auto& [id, attrs] : items) {
                    ++n;
                    if (recovering) recover_from = id;
                    if (!id_after(id, covered)) {  // already inside the snapshot
                        ack_now.push_back(id);
                        continue;
                    }
                    if (attrs) {
                        for (const auto& [k, v] : *attrs) {
                            if (k != "data") continue;
                            std::string error;
                            auto e = pjt::parse_event(v, &error);
                            if (!e) {
                                std::cerr << "skipping bad event " << id << ": " << error << "\n";
                                continue;
                            }
                            const auto a = std::chrono::steady_clock::now();
                            tracker.process(*e);
                            lat_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now() - a)
                                                 .count());
                            delay_ms.push_back(wall_ms() - split_id(id).first);
                        }
                    }
                    unacked.push_back(id);
                    last_id = id;
                    ++events_since_metrics;
                }
            }
            if (!ack_now.empty()) redis.xack(opt.in_stream, opt.group, ack_now.begin(), ack_now.end());
            if (recovering && n == 0) {
                recovering = false;
                std::cerr << "recovery done, now reading new events\n";
            }

            const long long now = wall_ms();
            if (n > 0) {
                last_event_wall = now;
            } else if (!recovering && now - last_event_wall >= opt.idle_after_ms) {
                // Quiet queue: a stuck payment sends no event, so let the clock move on its own.
                tracker.advance_to(tracker.now() + (now - last_tick_wall));
            }
            last_tick_wall = now;
            publish();
            // Save a snapshot (then acknowledge) about once a second, or as soon as the queue is
            // quiet. Saving after every few thousand events was measured to cut throughput by a
            // third when many payments are in memory, so the timer bounds the cost instead.
            if (!unacked.empty() && (n == 0 || now - last_snapshot_wall >= config.snapshot_every_ms)) {
                checkpoint();
                last_snapshot_wall = now;
            }

            if (now - last_metrics_wall >= 1000) {
                const double secs = static_cast<double>(now - last_metrics_wall) / 1000.0;
                nlohmann::json m{{"type", "metrics"},
                                 {"wall_ms", now},
                                 {"events_total", tracker.stats().events},
                                 {"events_per_sec", static_cast<double>(events_since_metrics) / secs},
                                 {"latency_p50_us", static_cast<double>(pjt::percentile(lat_ns, 50)) / 1000.0},
                                 {"latency_p99_us", static_cast<double>(pjt::percentile(lat_ns, 99)) / 1000.0},
                                 {"processing_delay_ms", pjt::percentile(delay_ms, 50)},
                                 {"backlog", backlog(redis, opt)},
                                 {"payments_in_memory", tracker.payments().size()},
                                 {"engine_clock_ms", tracker.now()}};
                const std::pair<std::string, std::string> field{"records", m.dump()};
                redis.xadd(opt.out_stream, "*", &field, &field + 1);
                lat_ns.clear();
                delay_ms.clear();
                events_since_metrics = 0;
                last_metrics_wall = now;
            }
        }
        checkpoint();
        std::cerr << "pjt_stream: stopped cleanly\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
