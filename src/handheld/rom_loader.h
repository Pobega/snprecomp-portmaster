#pragma once

#include <filesystem>
#include <string>

namespace snp::handheld {

// Scans current directory, ./roms, and SNP_ROM_DIR environment variable for a
// Sin and Punishment ROM (.z64, .n64, .v64), normalizes endianness if needed,
// registers the ROM with librecomp, and returns the selected game id (e.g. "snp.n64.en.fan").
// Returns empty string if no valid ROM was found.
std::string find_and_select_rom();

std::filesystem::path get_selected_rom_path();

} // namespace snp::handheld
