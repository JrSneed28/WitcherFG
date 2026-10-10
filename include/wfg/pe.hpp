#pragma once

#include "wfg/core.hpp"

#include <string_view>

namespace wfg {

struct Section { Rva virtual_address{}; std::uint32_t virtual_size{}; std::uint32_t raw_size{}; };
struct DataDirectory { Rva rva{}; std::uint32_t size{}; };
struct PeImage { Address base{}; std::uint16_t machine{}; std::uint32_t timestamp{}; std::uint32_t size_of_image{}; std::vector<Section> sections; std::array<DataDirectory, 16> directories{}; };
struct ImportSlot { Rva rva{}; Address address{}; Address current_pointer{}; };
struct ExportEntry { Rva rva{}; std::optional<std::string> forwarder; };
struct ModuleImage { std::string name; PeImage image; };

Result<PeImage> InspectPe(const IMemory& memory, Address base);
Result<ImportSlot> FindImport(const IMemory& memory, const PeImage& image, std::string_view module, std::string_view name);
Result<ExportEntry> FindExport(const IMemory& memory, const PeImage& image, std::string_view name);
Result<Address> ResolveNativeExport(const IMemory& memory, std::span<const ModuleImage> modules, std::string_view module, std::string_view name);

}  // namespace wfg
