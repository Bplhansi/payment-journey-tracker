#include "pjt/config.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace pjt {

namespace {

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r");
    return std::string(s.substr(first, last - first + 1));
}

std::int64_t to_int(const std::string& key, const std::string& value) {
    std::size_t used = 0;
    long long v = 0;
    try {
        v = std::stoll(value, &used);
    } catch (const std::exception&) {
        used = 0;
    }
    if (used != value.size() || v < 0) throw std::runtime_error("config: " + key + " must be a whole number >= 0");
    return v;
}

double to_double(const std::string& key, const std::string& value) {
    std::size_t used = 0;
    double v = 0;
    try {
        v = std::stod(value, &used);
    } catch (const std::exception&) {
        used = 0;
    }
    if (used != value.size() || v <= 0) throw std::runtime_error("config: " + key + " must be a number > 0");
    return v;
}

}  // namespace

EngineConfig parse_config(const std::string& text) {
    EngineConfig c;
    std::istringstream in(text);
    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        const auto hash = line.find_first_of("#;");
        const std::string clean = trim(hash == std::string::npos ? line : line.substr(0, hash));
        if (clean.empty()) continue;
        if (clean.front() == '[' && clean.back() == ']') {
            section = trim(std::string_view(clean).substr(1, clean.size() - 2));
            continue;
        }
        const auto eq = clean.find('=');
        if (eq == std::string::npos) throw std::runtime_error("config: expected key = value, got: " + clean);
        const std::string key = trim(std::string_view(clean).substr(0, eq));
        const std::string value = trim(std::string_view(clean).substr(eq + 1));
        const std::string full = section + "." + key;

        if (section == "stuck_after_ms") {
            const auto stage = parse_stage(key);
            if (!stage || is_final(*stage)) throw std::runtime_error("config: no time limit allowed for stage " + key);
            c.stuck_after_ms[static_cast<std::size_t>(index_of(*stage))] = to_int(full, value);
        } else if (full == "ordering.reorder_wait_ms") {
            c.reorder_wait_ms = to_int(full, value);
        } else if (full == "ordering.dedupe_capacity") {
            c.dedupe_capacity = static_cast<std::size_t>(to_int(full, value));
        } else if (full == "memory.retention_ms") {
            c.retention_ms = to_int(full, value);
        } else if (full == "snapshot.every_events") {
            c.snapshot_every_events = to_int(full, value);
        } else if (full == "snapshot.every_ms") {
            c.snapshot_every_ms = to_int(full, value);
        } else if (full == "slowdown.enabled") {
            if (value != "true" && value != "false") throw std::runtime_error("config: " + full + " must be true or false");
            c.slowdown.enabled = value == "true";
        } else if (full == "slowdown.ratio") {
            c.slowdown.ratio = to_double(full, value);
        } else if (full == "slowdown.warmup") {
            c.slowdown.warmup = static_cast<int>(to_int(full, value));
        } else if (full == "slowdown.fast_alpha") {
            c.slowdown.fast_alpha = to_double(full, value);
        } else if (full == "slowdown.slow_alpha") {
            c.slowdown.slow_alpha = to_double(full, value);
        } else {
            throw std::runtime_error("config: unknown setting " + full);
        }
    }
    if (c.dedupe_capacity == 0) throw std::runtime_error("config: ordering.dedupe_capacity must be > 0");
    return c;
}

EngineConfig load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("config: cannot open " + path);
    std::stringstream buf;
    buf << f.rdbuf();
    return parse_config(buf.str());
}

}  // namespace pjt
