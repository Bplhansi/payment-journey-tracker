// Small helpers shared by the command-line programs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pjt {

// Writes to a temporary file first, then renames it, so a crash never leaves half a file.
void write_file_atomic(const std::string& path, const std::string& content);
// Returns false when the file does not exist.
bool read_file(const std::string& path, std::string& out);

// The value below which `p` percent of samples fall (p from 0 to 100). Sorts `samples`.
std::int64_t percentile(std::vector<std::int64_t>& samples, double p);

}  // namespace pjt
