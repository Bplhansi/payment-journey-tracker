#include "pjt/util.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace pjt {

void write_file_atomic(const std::string& path, const std::string& content) {
    const std::filesystem::path target(path);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path());
    const std::filesystem::path tmp = target.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + tmp.string());
        f << content;
        f.flush();
        if (!f) throw std::runtime_error("cannot write " + tmp.string());
    }
    std::filesystem::rename(tmp, target);  // replaces the old file in one step
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream buf;
    buf << f.rdbuf();
    out = buf.str();
    return true;
}

std::int64_t percentile(std::vector<std::int64_t>& samples, double p) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    const double rank = p / 100.0 * static_cast<double>(samples.size() - 1);
    return samples[static_cast<std::size_t>(std::llround(rank))];
}

}  // namespace pjt
