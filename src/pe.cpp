#include "wfg/pe.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <functional>
#include <limits>

namespace wfg {
namespace {
constexpr std::size_t kMaxRead = 1024U * 1024U;
constexpr std::size_t kMaxString = 4096U;
constexpr std::size_t kMaxForwarderLinks = 8;
constexpr std::size_t kCertificateDirectoryIndex = 4;

Error Fail(ErrorCode code, const char* operation) { return {code, Stage::Inspect, operation}; }

template <class T> Result<T> ReadValue(const IMemory& memory, Address address) {
  auto bytes = memory.Read(address, sizeof(T));
  if (!IsOk(bytes)) return Failure(bytes);
  if (Value(bytes).size() != sizeof(T)) return Fail(ErrorCode::UnreadableMemory, "short_read");
  T value{};
  std::memcpy(&value, Value(bytes).data(), sizeof(value));
  return value;
}

bool Add(Address left, std::uint64_t right, Address& result) {
  if (right > std::numeric_limits<Address>::max() - left) return false;
  result = left + right;
  return true;
}

bool InImage(const PeImage& image, std::uint64_t rva, std::uint64_t size = 1) {
  return rva <= image.size_of_image && size <= static_cast<std::uint64_t>(image.size_of_image) - rva;
}

Result<Address> At(const PeImage& image, std::uint64_t rva, std::uint64_t size = 1) {
  Address address{};
  if (!InImage(image, rva, size) || !Add(image.base, rva, address)) return Fail(ErrorCode::MalformedImage, "rva_range");
  return address;
}

Result<std::string> ReadString(const IMemory& memory, const PeImage& image, std::uint64_t rva, std::uint64_t bound = kMaxString) {
  if (!InImage(image, rva)) return Fail(ErrorCode::MalformedImage, "string_range");
  const auto available = std::min<std::uint64_t>({kMaxString, bound, static_cast<std::uint64_t>(image.size_of_image) - rva});
  if (available == 0) return Fail(ErrorCode::MalformedImage, "string_range");
  auto address = At(image, rva, available);
  if (!IsOk(address)) return Failure(address);
  auto bytes = memory.Read(Value(address), static_cast<std::size_t>(available));
  if (!IsOk(bytes)) return Failure(bytes);
  if (Value(bytes).size() != available) return Fail(ErrorCode::UnreadableMemory, "short_read");
  for (std::size_t index = 0; index < Value(bytes).size(); ++index) {
    if (Value(bytes)[index] == std::byte{}) return std::string(reinterpret_cast<const char*>(Value(bytes).data()), index);
  }
  return Fail(ErrorCode::MalformedImage, "unterminated_string");
}

std::string Lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return result;
}

std::string NormalizeModule(std::string_view value) {
  auto result = Lower(value);
  if (result.find('.') == std::string::npos) result += ".dll";
  return result;
}

bool SameModule(std::string_view left, std::string_view right) { return NormalizeModule(left) == NormalizeModule(right); }

Result<Unit> Directory(const PeImage& image, std::size_t index, DataDirectory& directory) {
  if (index >= image.directories.size() || index == kCertificateDirectoryIndex) return Fail(ErrorCode::MalformedImage, "directory_index");
  directory = image.directories[index];
  if (directory.rva == 0 && directory.size == 0) return Fail(ErrorCode::MalformedImage, "missing_directory");
  if (directory.rva == 0 || directory.size == 0 || directory.size > kMaxRead || !InImage(image, directory.rva, directory.size)) return Fail(ErrorCode::MalformedImage, "directory_range");
  return Unit{};
}

Result<ExportEntry> ExportBy(const IMemory& memory, const PeImage& image, std::string_view name, std::optional<std::uint32_t> ordinal) {
  DataDirectory directory{};
  auto valid = Directory(image, 0, directory);
  if (!IsOk(valid)) return Failure(valid);
  if (directory.size < 40) return Fail(ErrorCode::MalformedImage, "export_table");
  auto table_address = At(image, directory.rva, 40);
  if (!IsOk(table_address)) return Failure(table_address);
  auto table = memory.Read(Value(table_address), 40);
  if (!IsOk(table)) return Failure(table);
  if (Value(table).size() != 40) return Fail(ErrorCode::UnreadableMemory, "short_read");
  const auto u32 = [&](std::size_t offset) {
    std::uint32_t output{};
    std::memcpy(&output, Value(table).data() + offset, sizeof(output));
    return output;
  };
  const auto ordinal_base = u32(16);
  const auto function_count = u32(20);
  const auto name_count = u32(24);
  const auto functions_rva = u32(28);
  const auto names_rva = u32(32);
  const auto ordinals_rva = u32(36);
  if (function_count == 0 || function_count > kMaxRead / 4 || !InImage(image, functions_rva, static_cast<std::uint64_t>(function_count) * 4)) return Fail(ErrorCode::MalformedImage, "export_functions");
  if (name_count > function_count || name_count > kMaxRead / 4 || !InImage(image, names_rva, static_cast<std::uint64_t>(name_count) * 4) || !InImage(image, ordinals_rva, static_cast<std::uint64_t>(name_count) * 2)) return Fail(ErrorCode::MalformedImage, "export_names");

  std::uint32_t function_index{};
  if (ordinal) {
    if (*ordinal < ordinal_base || static_cast<std::uint64_t>(*ordinal) - ordinal_base >= function_count) return Fail(ErrorCode::MalformedImage, "export_ordinal");
    function_index = *ordinal - ordinal_base;
  } else {
    bool found{};
    for (std::uint32_t index = 0; index < name_count; ++index) {
      auto name_rva = ReadValue<std::uint32_t>(memory, Value(At(image, static_cast<std::uint64_t>(names_rva) + static_cast<std::uint64_t>(index) * 4, 4)));
      if (!IsOk(name_rva)) return Failure(name_rva);
      auto export_name = ReadString(memory, image, Value(name_rva));
      if (!IsOk(export_name)) return Failure(export_name);
      if (Value(export_name) != name) continue;
      auto index_value = ReadValue<std::uint16_t>(memory, Value(At(image, static_cast<std::uint64_t>(ordinals_rva) + static_cast<std::uint64_t>(index) * 2, 2)));
      if (!IsOk(index_value)) return Failure(index_value);
      if (Value(index_value) >= function_count) return Fail(ErrorCode::MalformedImage, "export_ordinal");
      function_index = Value(index_value);
      found = true;
      break;
    }
    if (!found) return Fail(ErrorCode::MalformedImage, "export_not_found");
  }

  auto function_rva = ReadValue<std::uint32_t>(memory, Value(At(image, static_cast<std::uint64_t>(functions_rva) + static_cast<std::uint64_t>(function_index) * 4, 4)));
  if (!IsOk(function_rva)) return Failure(function_rva);
  if (Value(function_rva) == 0 || !InImage(image, Value(function_rva))) return Fail(ErrorCode::MalformedImage, "export_rva");
  ExportEntry result{Value(function_rva), std::nullopt};
  const auto directory_end = static_cast<std::uint64_t>(directory.rva) + directory.size;
  if (Value(function_rva) >= directory.rva && Value(function_rva) < directory_end) {
    auto forwarder = ReadString(memory, image, Value(function_rva), directory_end - Value(function_rva));
    if (!IsOk(forwarder)) return Fail(ErrorCode::MalformedImage, "forwarder");
    result.forwarder = Value(forwarder);
  }
  return result;
}

Result<std::uint32_t> ParseOrdinal(std::string_view symbol) {
  if (symbol.size() < 2 || symbol.front() != '#') return Fail(ErrorCode::MalformedImage, "forwarder_ordinal");
  std::uint32_t ordinal{};
  const auto parsed = std::from_chars(symbol.data() + 1, symbol.data() + symbol.size(), ordinal);
  if (parsed.ec != std::errc{} || parsed.ptr != symbol.data() + symbol.size() || ordinal > 0xffff) return Fail(ErrorCode::MalformedImage, "forwarder_ordinal");
  return ordinal;
}
}

Result<PeImage> InspectPe(const IMemory& memory, Address base) {
  auto dos = memory.Read(base, 0x40);
  if (!IsOk(dos)) return Failure(dos);
  if (Value(dos).size() != 0x40) return Fail(ErrorCode::UnreadableMemory, "dos_header");
  std::uint16_t signature{};
  std::uint32_t nt_offset{};
  std::memcpy(&signature, Value(dos).data(), sizeof(signature));
  std::memcpy(&nt_offset, Value(dos).data() + 0x3c, sizeof(nt_offset));
  Address nt{};
  if (signature != 0x5a4d || nt_offset > kMaxRead || !Add(base, nt_offset, nt)) return Fail(ErrorCode::MalformedImage, "dos_header");

  auto file = memory.Read(nt, 24);
  if (!IsOk(file)) return Failure(file);
  if (Value(file).size() != 24) return Fail(ErrorCode::UnreadableMemory, "file_header");
  std::uint32_t pe_signature{};
  std::uint16_t machine{}, section_count{}, optional_size{};
  std::uint32_t timestamp{};
  std::memcpy(&pe_signature, Value(file).data(), 4);
  std::memcpy(&machine, Value(file).data() + 4, 2);
  std::memcpy(&section_count, Value(file).data() + 6, 2);
  std::memcpy(&timestamp, Value(file).data() + 8, 4);
  std::memcpy(&optional_size, Value(file).data() + 20, 2);
  if (pe_signature != 0x4550 || machine != 0x8664 || section_count == 0 || optional_size < 112 || optional_size > kMaxRead) return Fail(ErrorCode::MalformedImage, "file_header");

  Address optional_address{};
  if (!Add(nt, 24, optional_address)) return Fail(ErrorCode::MalformedImage, "optional_header");
  auto optional = memory.Read(optional_address, optional_size);
  if (!IsOk(optional)) return Failure(optional);
  if (Value(optional).size() != optional_size) return Fail(ErrorCode::UnreadableMemory, "optional_header");
  std::uint16_t magic{};
  std::uint32_t image_size{}, directory_count{};
  std::memcpy(&magic, Value(optional).data(), 2);
  std::memcpy(&image_size, Value(optional).data() + 0x38, 4);
  std::memcpy(&directory_count, Value(optional).data() + 0x6c, 4);
  if (magic != 0x20b || image_size == 0 || directory_count > 16 || 0x70ULL + static_cast<std::uint64_t>(directory_count) * 8 > optional_size) return Fail(ErrorCode::MalformedImage, "optional_header");

  PeImage image{base, machine, timestamp, image_size};
  for (std::uint32_t index = 0; index < directory_count; ++index) {
    auto& directory = image.directories[index];
    std::memcpy(&directory.rva, Value(optional).data() + 0x70 + index * 8, 4);
    std::memcpy(&directory.size, Value(optional).data() + 0x74 + index * 8, 4);
    if ((directory.rva == 0) != (directory.size == 0) || (index != kCertificateDirectoryIndex && directory.size != 0 && !InImage(image, directory.rva, directory.size))) return Fail(ErrorCode::MalformedImage, "directory_range");
  }

  const auto section_bytes_count = static_cast<std::uint64_t>(section_count) * 40;
  if (section_bytes_count > kMaxRead) return Fail(ErrorCode::MalformedImage, "sections");
  Address section_address{};
  if (!Add(optional_address, optional_size, section_address)) return Fail(ErrorCode::MalformedImage, "sections");
  auto section_bytes = memory.Read(section_address, static_cast<std::size_t>(section_bytes_count));
  if (!IsOk(section_bytes)) return Failure(section_bytes);
  if (Value(section_bytes).size() != section_bytes_count) return Fail(ErrorCode::UnreadableMemory, "sections");
  std::uint64_t header_end{};
  if (!Add(nt, 24ULL + optional_size + section_bytes_count, header_end) || header_end < base || !InImage(image, header_end - base, 0)) return Fail(ErrorCode::MalformedImage, "headers_range");
  for (std::uint16_t index = 0; index < section_count; ++index) {
    const auto* bytes = Value(section_bytes).data() + static_cast<std::size_t>(index) * 40;
    Section section{};
    std::memcpy(&section.virtual_size, bytes + 8, 4);
    std::memcpy(&section.virtual_address, bytes + 12, 4);
    std::memcpy(&section.raw_size, bytes + 16, 4);
    const auto mapped_size = std::max(section.virtual_size, section.raw_size);
    if (mapped_size != 0 && !InImage(image, section.virtual_address, mapped_size)) return Fail(ErrorCode::MalformedImage, "section_range");
    image.sections.push_back(section);
  }
  return image;
}

namespace {
Result<ImportSlot> FindImportBy(const IMemory& memory, const PeImage& image, std::string_view module, std::string_view name, std::optional<PeOrdinal> ordinal) {
  DataDirectory directory{};
  auto valid = Directory(image, 1, directory);
  if (!IsOk(valid)) return Failure(valid);
  if (directory.size < 20) return Fail(ErrorCode::MalformedImage, "import_table");
  bool descriptor_terminated{};
  bool found{};
  ImportSlot result{};
  for (std::uint32_t offset = 0; offset + 20 <= directory.size; offset += 20) {
    auto descriptor = memory.Read(Value(At(image, static_cast<std::uint64_t>(directory.rva) + offset, 20)), 20);
    if (!IsOk(descriptor)) return Failure(descriptor);
    if (Value(descriptor).size() != 20) return Fail(ErrorCode::UnreadableMemory, "short_read");
    std::uint32_t original{}, name_rva{}, first{};
    std::memcpy(&original, Value(descriptor).data(), 4);
    std::memcpy(&name_rva, Value(descriptor).data() + 12, 4);
    std::memcpy(&first, Value(descriptor).data() + 16, 4);
    if (std::all_of(Value(descriptor).begin(), Value(descriptor).end(), [](std::byte value) { return value == std::byte{}; })) {
      descriptor_terminated = true;
      break;
    }
    if (original == 0 || name_rva == 0 || first == 0) return Fail(ErrorCode::MalformedImage, "bound_import");
    auto module_name = ReadString(memory, image, name_rva);
    if (!IsOk(module_name)) return Failure(module_name);
    const bool matching_module = SameModule(Value(module_name), module);
    const auto max_entries = std::min<std::uint64_t>(kMaxRead / 8, (static_cast<std::uint64_t>(image.size_of_image) - std::min<std::uint64_t>(original, image.size_of_image)) / 8);
    bool thunk_terminated{};
    for (std::uint64_t index = 0; index < max_entries; ++index) {
      const auto thunk_rva = static_cast<std::uint64_t>(original) + index * 8;
      const auto slot_rva = static_cast<std::uint64_t>(first) + index * 8;
      if (!InImage(image, thunk_rva, 8) || !InImage(image, slot_rva, 8)) return Fail(ErrorCode::MalformedImage, "import_thunk");
      auto thunk = ReadValue<std::uint64_t>(memory, Value(At(image, thunk_rva, 8)));
      if (!IsOk(thunk)) return Failure(thunk);
      if (Value(thunk) == 0) {
        thunk_terminated = true;
        break;
      }
      if ((Value(thunk) & (1ULL << 63)) != 0) {
        if ((Value(thunk) & 0x7fffffffffff0000ULL) != 0) return Fail(ErrorCode::MalformedImage, "import_ordinal");
        if (!matching_module || !ordinal || (Value(thunk) & 0xffff) != ordinal->value) continue;
      } else {
        if (!InImage(image, Value(thunk), 3)) return Fail(ErrorCode::MalformedImage, "import_name");
        if (!matching_module) continue;
        auto import_name = ReadString(memory, image, static_cast<std::uint64_t>(Value(thunk)) + 2);
        if (!IsOk(import_name)) return Failure(import_name);
        if (ordinal || Value(import_name) != name) continue;
      }
      if (found) return Fail(ErrorCode::MalformedImage, "duplicate_import");
      auto slot_address = At(image, slot_rva, 8);
      if (!IsOk(slot_address)) return Failure(slot_address);
      auto pointer = ReadValue<std::uint64_t>(memory, Value(slot_address));
      if (!IsOk(pointer)) return Failure(pointer);
      result = {static_cast<Rva>(slot_rva), Value(slot_address), Value(pointer), Value(thunk)};
      found = true;
    }
    if (!thunk_terminated) return Fail(ErrorCode::MalformedImage, "unterminated_thunks");
  }
  if (!descriptor_terminated) return Fail(ErrorCode::MalformedImage, "unterminated_imports");
  return found ? Result<ImportSlot>(result) : Result<ImportSlot>(Fail(ErrorCode::MalformedImage, "import_not_found"));
}
}

Result<ImportSlot> FindImport(const IMemory& memory, const PeImage& image, std::string_view module, std::string_view name) { return FindImportBy(memory,image,module,name,std::nullopt); }
Result<ImportSlot> FindImport(const IMemory& memory, const PeImage& image, std::string_view module, PeOrdinal ordinal) { return FindImportBy(memory,image,module,{},ordinal); }
Result<ExportEntry> FindExport(const IMemory& memory, const PeImage& image, std::string_view name) { return ExportBy(memory, image, name, std::nullopt); }
Result<ExportEntry> FindExport(const IMemory& memory, const PeImage& image, PeOrdinal ordinal) { return ExportBy(memory,image,{},ordinal.value); }
Result<Unit> BindExportOrdinal(const IMemory& memory,const PeImage& image,std::string_view name,PeOrdinal ordinal) {
  const auto named=FindExport(memory,image,name);if(!IsOk(named))return Failure(named);
  const auto numbered=FindExport(memory,image,ordinal);if(!IsOk(numbered))return Failure(numbered);
  if(Value(named).rva!=Value(numbered).rva||Value(named).forwarder!=Value(numbered).forwarder)return Fail(ErrorCode::MalformedImage,"ordinal_name_binding");
  return Unit{};
}

Result<Address> ResolveNativeExport(const IMemory& memory, std::span<const ModuleImage> modules, std::string_view module, std::string_view name) {
  std::vector<std::string> chain;
  std::function<Result<Address>(std::string_view, std::string_view, std::size_t)> resolve;
  resolve = [&](std::string_view current_module, std::string_view symbol, std::size_t links) -> Result<Address> {
    if (links > kMaxForwarderLinks) return Fail(ErrorCode::MalformedImage, "forwarder_depth");
    const auto key = NormalizeModule(current_module) + "!" + Lower(symbol);
    if (std::find(chain.begin(), chain.end(), key) != chain.end()) return Fail(ErrorCode::MalformedImage, "forwarder_cycle");
    const ModuleImage* target_module{};
    for (const auto& candidate : modules) {
      if (SameModule(candidate.name, current_module)) {
        target_module = &candidate;
        break;
      }
    }
    if (target_module == nullptr) return Fail(ErrorCode::MalformedImage, "module_not_found");
    chain.push_back(key);
    std::optional<std::uint32_t> ordinal;
    if (!symbol.empty() && symbol.front() == '#') {
      auto parsed = ParseOrdinal(symbol);
      if (!IsOk(parsed)) {
        chain.pop_back();
        return Failure(parsed);
      }
      ordinal = Value(parsed);
    }
    auto entry = ExportBy(memory, target_module->image, symbol, ordinal);
    if (!IsOk(entry)) {
      chain.pop_back();
      return Failure(entry);
    }
    if (!Value(entry).forwarder) {
      auto address = At(target_module->image, Value(entry).rva);
      chain.pop_back();
      return address;
    }
    const auto separator = Value(entry).forwarder->find_last_of('.');
    if (separator == std::string::npos || separator == 0 || separator + 1 == Value(entry).forwarder->size()) {
      chain.pop_back();
      return Fail(ErrorCode::MalformedImage, "forwarder");
    }
    auto next_module = Value(entry).forwarder->substr(0, separator);
    auto next_symbol = Value(entry).forwarder->substr(separator + 1);
    auto answer = resolve(next_module, next_symbol, links + 1);
    chain.pop_back();
    return answer;
  };
  return resolve(module, name, 0);
}
}  // namespace wfg
