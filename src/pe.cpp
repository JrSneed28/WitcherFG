#include "wfg/pe.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <limits>

namespace wfg {
namespace {
constexpr std::size_t kMaxRead = 1024U * 1024U;
constexpr std::size_t kMaxString = 4096U;
Error Fail(ErrorCode code, const char* operation) { return {code, Stage::Inspect, operation}; }
template <class T> Result<T> ReadValue(const IMemory& memory, Address address) {
  auto bytes = memory.Read(address, sizeof(T));
  if (!IsOk(bytes)) return Failure(bytes);
  if (Value(bytes).size() != sizeof(T)) return Fail(ErrorCode::UnreadableMemory, "short_read");
  T value{}; std::memcpy(&value, Value(bytes).data(), sizeof(value)); return value;
}
bool Add(Address left, std::uint64_t right, Address& result) { if (right > std::numeric_limits<Address>::max() - left) return false; result = left + right; return true; }
bool InImage(const PeImage& image, Rva rva, std::uint64_t size = 1) { return rva <= image.size_of_image && size <= static_cast<std::uint64_t>(image.size_of_image) - rva; }
Result<Address> At(const PeImage& image, Rva rva, std::uint64_t size = 1) { Address address{}; if (!InImage(image, rva, size) || !Add(image.base, rva, address)) return Fail(ErrorCode::MalformedImage, "rva_range"); return address; }
Result<std::string> ReadString(const IMemory& memory, const PeImage& image, Rva rva) {
  if (!InImage(image, rva)) return Fail(ErrorCode::MalformedImage, "string_range");
  const auto available = std::min<std::uint64_t>(kMaxString, static_cast<std::uint64_t>(image.size_of_image) - rva);
  auto address = At(image, rva, available); if (!IsOk(address)) return Failure(address);
  auto bytes = memory.Read(Value(address), static_cast<std::size_t>(available)); if (!IsOk(bytes)) return Failure(bytes);
  for (std::size_t index = 0; index < Value(bytes).size(); ++index) if (Value(bytes)[index] == std::byte{}) return std::string(reinterpret_cast<const char*>(Value(bytes).data()), index);
  return Fail(ErrorCode::MalformedImage, "unterminated_string");
}
std::string Lower(std::string_view value) { std::string result(value); std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }); return result; }
bool SameModule(std::string_view left, std::string_view right) { return Lower(left) == Lower(right); }
Result<Unit> Directory(const PeImage& image, std::size_t index, DataDirectory& directory) {
  directory = image.directories[index];
  if (directory.rva == 0 && directory.size == 0) return Fail(ErrorCode::MalformedImage, "missing_directory");
  if (directory.size > kMaxRead || !InImage(image, directory.rva, directory.size)) return Fail(ErrorCode::MalformedImage, "directory_range");
  return Unit{};
}
Result<ExportEntry> ExportBy(const IMemory& memory, const PeImage& image, std::string_view name, std::optional<std::uint16_t> ordinal) {
  DataDirectory directory{}; auto valid = Directory(image, 0, directory); if (!IsOk(valid)) return Failure(valid);
  if (directory.size < 40) return Fail(ErrorCode::MalformedImage, "export_table");
  auto table_address = At(image, directory.rva, 40); if (!IsOk(table_address)) return Failure(table_address);
  auto table = memory.Read(Value(table_address), 40); if (!IsOk(table)) return Failure(table);
  const auto u32 = [&](std::size_t offset) { std::uint32_t out{}; std::memcpy(&out, Value(table).data() + offset, 4); return out; };
  const auto base = u32(16), functions = u32(20), names = u32(24), functions_rva = u32(28), names_rva = u32(32), ordinals_rva = u32(36);
  if (functions == 0 || functions > kMaxRead / 4 || !InImage(image, functions_rva, static_cast<std::uint64_t>(functions) * 4)) return Fail(ErrorCode::MalformedImage, "export_functions");
  std::uint32_t function_index{};
  if (ordinal) { if (*ordinal < base || static_cast<std::uint32_t>(*ordinal - base) >= functions) return Fail(ErrorCode::MalformedImage, "export_ordinal"); function_index = *ordinal - base; }
  else {
    if (names > kMaxRead / 4 || !InImage(image, names_rva, static_cast<std::uint64_t>(names) * 4) || !InImage(image, ordinals_rva, static_cast<std::uint64_t>(names) * 2)) return Fail(ErrorCode::MalformedImage, "export_names");
    bool found{};
    for (std::uint32_t index = 0; index < names; ++index) { auto entry = ReadValue<std::uint32_t>(memory, Value(At(image, names_rva + index * 4, 4))); if (!IsOk(entry)) return Failure(entry); auto text = ReadString(memory, image, Value(entry)); if (!IsOk(text)) return Failure(text); if (Value(text) == name) { auto ord = ReadValue<std::uint16_t>(memory, Value(At(image, ordinals_rva + index * 2, 2))); if (!IsOk(ord)) return Failure(ord); if (Value(ord) >= functions) return Fail(ErrorCode::MalformedImage, "export_ordinal"); function_index = Value(ord); found = true; break; } }
    if (!found) return Fail(ErrorCode::MalformedImage, "export_not_found");
  }
  auto rva = ReadValue<std::uint32_t>(memory, Value(At(image, functions_rva + function_index * 4, 4))); if (!IsOk(rva)) return Failure(rva);
  if (!InImage(image, Value(rva))) return Fail(ErrorCode::MalformedImage, "export_rva");
  ExportEntry result{Value(rva), std::nullopt};
  if (Value(rva) >= directory.rva && static_cast<std::uint64_t>(Value(rva) - directory.rva) < directory.size) { auto forwarder = ReadString(memory, image, Value(rva)); if (!IsOk(forwarder) || Value(forwarder).size() > directory.rva + directory.size - Value(rva)) return Fail(ErrorCode::MalformedImage, "forwarder"); result.forwarder = Value(forwarder); }
  return result;
}
}

Result<PeImage> InspectPe(const IMemory& memory, Address base) {
  auto dos = memory.Read(base, 0x40); if (!IsOk(dos)) return Failure(dos); if (Value(dos).size() != 0x40) return Fail(ErrorCode::UnreadableMemory, "dos_header");
  std::uint16_t signature{}; std::uint32_t nt_offset{}; std::memcpy(&signature, Value(dos).data(), 2); std::memcpy(&nt_offset, Value(dos).data() + 0x3c, 4);
  Address nt{}; if (signature != 0x5A4D || !Add(base, nt_offset, nt)) return Fail(ErrorCode::MalformedImage, "dos_header");
  auto file = memory.Read(nt, 24); if (!IsOk(file)) return Failure(file); if (Value(file).size() != 24) return Fail(ErrorCode::UnreadableMemory, "file_header");
  std::uint32_t pe{}; std::uint16_t machine{}, sections{}, optional_size{}; std::uint32_t timestamp{}; std::memcpy(&pe, Value(file).data(), 4); std::memcpy(&machine, Value(file).data()+4,2); std::memcpy(&sections, Value(file).data()+6,2); std::memcpy(&timestamp, Value(file).data()+8,4); std::memcpy(&optional_size, Value(file).data()+20,2);
  if (pe != 0x4550 || machine != 0x8664 || sections == 0 || optional_size < 112 || optional_size > kMaxRead) return Fail(ErrorCode::MalformedImage, "file_header");
  Address optional{}; if (!Add(nt, 24, optional)) return Fail(ErrorCode::MalformedImage, "optional_header"); auto opt = memory.Read(optional, optional_size); if (!IsOk(opt)) return Failure(opt); if (Value(opt).size() != optional_size) return Fail(ErrorCode::UnreadableMemory, "optional_header");
  std::uint16_t magic{}; std::uint32_t image_size{}, directory_count{}; std::memcpy(&magic, Value(opt).data(), 2); std::memcpy(&image_size, Value(opt).data()+0x38,4); std::memcpy(&directory_count, Value(opt).data()+0x6c,4);
  if (magic != 0x20B || image_size == 0 || directory_count > 16 || 0x70ULL + directory_count * 8 > optional_size) return Fail(ErrorCode::MalformedImage, "optional_header");
  PeImage image{base, machine, timestamp, image_size};
  for (std::uint32_t index = 0; index < directory_count; ++index) { std::memcpy(&image.directories[index].rva, Value(opt).data()+0x70+index*8,4); std::memcpy(&image.directories[index].size, Value(opt).data()+0x74+index*8,4); if ((image.directories[index].rva == 0) != (image.directories[index].size == 0) || (image.directories[index].size && !InImage(image, image.directories[index].rva, image.directories[index].size))) return Fail(ErrorCode::MalformedImage, "directory_range"); }
  if (sections > kMaxRead / 40) return Fail(ErrorCode::MalformedImage, "sections"); Address section_base{}; if (!Add(optional, optional_size, section_base)) return Fail(ErrorCode::MalformedImage, "sections"); auto section_bytes = memory.Read(section_base, sections * 40U); if (!IsOk(section_bytes)) return Failure(section_bytes); if (Value(section_bytes).size() != sections * 40U) return Fail(ErrorCode::UnreadableMemory, "sections");
  for (std::uint16_t index = 0; index < sections; ++index) { const auto* part = Value(section_bytes).data()+index*40; Section section{}; std::memcpy(&section.virtual_size, part+8,4); std::memcpy(&section.virtual_address, part+12,4); std::memcpy(&section.raw_size, part+16,4); if (section.virtual_size && !InImage(image, section.virtual_address, section.virtual_size)) return Fail(ErrorCode::MalformedImage, "section_range"); image.sections.push_back(section); }
  return image;
}

Result<ImportSlot> FindImport(const IMemory& memory, const PeImage& image, std::string_view module, std::string_view name) {
  DataDirectory directory{}; auto valid = Directory(image, 1, directory); if (!IsOk(valid)) return Failure(valid); if (directory.size < 20) return Fail(ErrorCode::MalformedImage, "import_table");
  bool found{}; ImportSlot result{};
  for (std::uint32_t offset = 0; offset + 20 <= directory.size; offset += 20) { auto descriptor = memory.Read(Value(At(image, directory.rva + offset, 20)), 20); if (!IsOk(descriptor)) return Failure(descriptor); std::uint32_t original{}, module_rva{}, first{}; std::memcpy(&original, Value(descriptor).data(),4); std::memcpy(&module_rva, Value(descriptor).data()+12,4); std::memcpy(&first, Value(descriptor).data()+16,4); if (original == 0 && module_rva == 0 && first == 0) break; if (original == 0 || first == 0) return Fail(ErrorCode::MalformedImage, "bound_import"); auto module_name = ReadString(memory, image, module_rva); if (!IsOk(module_name)) return Failure(module_name); if (!SameModule(Value(module_name), module)) continue;
    for (std::uint32_t index = 0; index < kMaxRead / 8; ++index) { const auto thunk_rva = static_cast<std::uint64_t>(original) + index * 8; const auto slot_rva = static_cast<std::uint64_t>(first) + index * 8; if (thunk_rva > std::numeric_limits<Rva>::max() || slot_rva > std::numeric_limits<Rva>::max() || !InImage(image, static_cast<Rva>(thunk_rva),8) || !InImage(image, static_cast<Rva>(slot_rva),8)) return Fail(ErrorCode::MalformedImage, "import_thunk"); auto thunk = ReadValue<std::uint64_t>(memory, Value(At(image, static_cast<Rva>(thunk_rva),8))); if (!IsOk(thunk)) return Failure(thunk); if (Value(thunk) == 0) break; if ((Value(thunk) & (1ULL<<63)) != 0) continue; if (Value(thunk) > std::numeric_limits<Rva>::max() || !InImage(image, static_cast<Rva>(Value(thunk)), 3)) return Fail(ErrorCode::MalformedImage, "import_name"); auto import_name = ReadString(memory, image, static_cast<Rva>(Value(thunk)+2)); if (!IsOk(import_name)) return Failure(import_name); if (Value(import_name) == name) { if (found) return Fail(ErrorCode::MalformedImage, "duplicate_import"); auto pointer = ReadValue<std::uint64_t>(memory, Value(At(image, static_cast<Rva>(slot_rva),8))); if (!IsOk(pointer)) return Failure(pointer); found = true; result = {static_cast<Rva>(slot_rva), Value(At(image, static_cast<Rva>(slot_rva),8)), Value(pointer)}; } }
  }
  return found ? Result<ImportSlot>(result) : Result<ImportSlot>(Fail(ErrorCode::MalformedImage, "import_not_found"));
}

Result<ExportEntry> FindExport(const IMemory& memory, const PeImage& image, std::string_view name) { return ExportBy(memory, image, name, std::nullopt); }

Result<Address> ResolveNativeExport(const IMemory& memory, std::span<const ModuleImage> modules, std::string_view module, std::string_view name) {
  std::vector<std::string> chain; std::function<Result<Address>(std::string_view, std::string_view, std::size_t)> resolve = [&](std::string_view mod, std::string_view symbol, std::size_t depth) -> Result<Address> { if (depth >= 8) return Fail(ErrorCode::MalformedImage, "forwarder_depth"); const auto key = Lower(mod) + "!" + Lower(symbol); if (std::find(chain.begin(), chain.end(), key) != chain.end()) return Fail(ErrorCode::MalformedImage, "forwarder_cycle"); const ModuleImage* image{}; for (const auto& candidate : modules) if (SameModule(candidate.name, mod)) { image = &candidate; break; } if (!image) return Fail(ErrorCode::MalformedImage, "module_not_found"); chain.push_back(key); std::optional<std::uint16_t> ordinal; if (!symbol.empty() && symbol.front() == '#') { try { const auto value = std::stoul(std::string(symbol.substr(1))); if (value > 0xffff) throw std::out_of_range("ordinal"); ordinal = static_cast<std::uint16_t>(value); } catch (...) { chain.pop_back(); return Fail(ErrorCode::MalformedImage, "forwarder_ordinal"); } } auto entry = ExportBy(memory, image->image, symbol, ordinal); if (!IsOk(entry)) { chain.pop_back(); return Failure(entry); } if (!Value(entry).forwarder) { auto address = At(image->image, Value(entry).rva); chain.pop_back(); return address; } const auto dot = Value(entry).forwarder->find_last_of('.'); if (dot == std::string::npos || dot == 0 || dot + 1 == Value(entry).forwarder->size()) { chain.pop_back(); return Fail(ErrorCode::MalformedImage, "forwarder"); } auto forwarded_module = Value(entry).forwarder->substr(0, dot); if (forwarded_module.find('.') == std::string::npos) forwarded_module += ".dll"; auto answer = resolve(forwarded_module, Value(entry).forwarder->substr(dot+1), depth+1); chain.pop_back(); return answer; };
  return resolve(module, name, 0);
}
}  // namespace wfg
