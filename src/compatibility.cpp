#include "wfg/compatibility.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace wfg {
namespace {

Error Fail(ErrorCode code, const char* operation) { return {code, Stage::Compatibility, operation}; }

Hash256 Hex(std::string_view value) {
  Hash256 hash{};
  const auto nibble = [](char character) -> std::byte {
    if (character >= '0' && character <= '9') return static_cast<std::byte>(character - '0');
    if (character >= 'a' && character <= 'f') return static_cast<std::byte>(character - 'a' + 10);
    return static_cast<std::byte>(character - 'A' + 10);
  };
  for (std::size_t index = 0; index < hash.size(); ++index) hash[index] = (nibble(value[index * 2]) << 4) | nibble(value[index * 2 + 1]);
  return hash;
}

Result<Hash256> Sha256(std::span<const std::byte> bytes) {
  BCRYPT_ALG_HANDLE algorithm{};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return Fail(ErrorCode::UnsupportedBuild, "bcrypt_open");
  struct Close { BCRYPT_ALG_HANDLE algorithm; ~Close() { BCryptCloseAlgorithmProvider(algorithm, 0); } } close{algorithm};
  DWORD object_size{};
  DWORD received{};
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &received, 0) < 0 || received != sizeof(object_size)) return Fail(ErrorCode::UnsupportedBuild, "bcrypt_object_length");
  std::vector<std::byte> object(object_size);
  BCRYPT_HASH_HANDLE hash{};
  if (BCryptCreateHash(algorithm, &hash, reinterpret_cast<PUCHAR>(object.data()), object_size, nullptr, 0, 0) < 0) return Fail(ErrorCode::UnsupportedBuild, "bcrypt_create");
  struct Destroy { BCRYPT_HASH_HANDLE hash; ~Destroy() { BCryptDestroyHash(hash); } } destroy{hash};
  if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0) return Fail(ErrorCode::UnsupportedBuild, "bcrypt_data");
  Hash256 output{};
  if (BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(output.data()), static_cast<ULONG>(output.size()), 0) < 0) return Fail(ErrorCode::UnsupportedBuild, "bcrypt_finish");
  return output;
}

bool Add(Address base, std::uint64_t offset, Address& address) {
  if (offset > std::numeric_limits<Address>::max() - base) return false;
  address = base + offset;
  return true;
}

bool InImage(const PeImage& image, Rva rva, std::uint32_t size) {
  return rva <= image.size_of_image && size <= image.size_of_image - rva;
}

bool Readable(const IMemory& memory, Address address, std::size_t size) {
  if (size > std::numeric_limits<Address>::max() - address) return false;
  const auto region = memory.Query(address);
  return IsOk(region) && Value(region).committed && Value(region).readable && address >= Value(region).base && size <= Value(region).size && address - Value(region).base <= Value(region).size - size;
}

template <class T> Result<T> ReadValue(const IMemory& memory, Address address, const char* operation) {
  if (!Readable(memory, address, sizeof(T))) return Fail(ErrorCode::UnreadableMemory, operation);
  const auto bytes = memory.Read(address, sizeof(T));
  if (!IsOk(bytes)) return Failure(bytes);
  if (Value(bytes).size() != sizeof(T)) return Fail(ErrorCode::UnreadableMemory, operation);
  T value{};
  std::memcpy(&value, Value(bytes).data(), sizeof(value));
  return value;
}

const TrustedModule* NativeModule(const RuntimeInventory& inventory, std::string_view name) {
  const auto equal = [](std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) if (std::tolower(static_cast<unsigned char>(left[index])) != std::tolower(static_cast<unsigned char>(right[index]))) return false;
    return true;
  };
  for (const auto& module : inventory.trusted_system_modules) if (module.trusted_system && equal(module.module.name, name)) return &module;
  return nullptr;
}

}  // namespace

const CompatibilityRecord& SupportedBuild() {
  static const CompatibilityRecord record{
      "CDPROJEKTSA.Witcher3GOTY", "5.0.4.0", 0x8664, 0x6AC55CE6U, 0x658F000U,
      {"2.14.1", 652928U, Hex("8c87c9499461da561edd529aa9bf7831d67d7b94ebb1c1a5ed54ef4934e1ea4c")},
      {{{0x1CB0384U, 32U, Hex("bea2f23fae85c0d5ae5a517bb573945525addd04bb52ce4ae239b7da944b7c09")}, {0x1CB061DU, 32U, Hex("88f7cb6bdd8c6d3285de7ab6688c00d98d10d189e2906136aa8432b5afe3e736")}}},
      {{{"d3d12.dll", "D3D12CreateDevice", "d3d12.dll", 0x2B52450U}, {"dxgi.dll", "CreateDXGIFactory1", "dxgi.dll", 0x2B524D0U}, {"dxgi.dll", "CreateDXGIFactory2", "dxgi.dll", 0x2B524D8U}, {"dxgi.dll", "CreateDXGIFactory", "dxgi.dll", 0x2B524E0U}}},
      0x5A75F30U, 0x302F568U};
  return record;
}

Result<ValidatedBuild> ValidateBuild(const IMemory& memory, const RuntimeInventory& inventory, const CompatibilityRecord& record) {
  if (inventory.package_name != record.package_name || inventory.package_version != record.package_version) return Fail(ErrorCode::UnsupportedBuild, "package_identity");
  const auto executable = InspectPe(memory, inventory.executable.image.base);
  if (!IsOk(executable)) return Failure(executable);
  if (Value(executable).machine != record.machine || Value(executable).timestamp != record.pe_timestamp || Value(executable).size_of_image != record.size_of_image) return Fail(ErrorCode::UnsupportedBuild, "executable_identity");
  if (inventory.interposer.version != record.interposer.version || inventory.interposer.length != record.interposer.length || inventory.interposer.sha256 != record.interposer.sha256) return Fail(ErrorCode::UnsupportedBuild, "interposer_identity");
  if (!inventory.interposer.trusted_nvidia_signer || !inventory.interposer.backing_image_matches_loaded_module) return Fail(ErrorCode::UntrustedInterposer, "interposer_provenance");
  const auto interposer_image = InspectPe(memory, inventory.interposer.loaded_module.image.base);
  if (!IsOk(interposer_image)) return Failure(interposer_image);
  for (const auto& fingerprint : record.fingerprints) {
    Address address{};
    if (!InImage(Value(executable), fingerprint.rva, fingerprint.length) || !Add(Value(executable).base, fingerprint.rva, address) || !Readable(memory, address, fingerprint.length)) return Fail(ErrorCode::UnsupportedBuild, "fingerprint_range");
    const auto bytes = memory.Read(address, fingerprint.length);
    if (!IsOk(bytes)) return Failure(bytes);
    if (Value(bytes).size() != fingerprint.length) return Fail(ErrorCode::UnreadableMemory, "fingerprint_read");
    const auto hash = Sha256(Value(bytes));
    if (!IsOk(hash)) return Failure(hash);
    if (Value(hash) != fingerprint.sha256) return Fail(ErrorCode::UnsupportedBuild, "fingerprint_hash");
  }

  ValidatedBuild result{};
  result.factory_manager_rva = record.factory_manager_rva;
  result.factory_member_offset = record.factory_member_offset;
  result.executable_base = Value(executable).base;
  bool all_native = true;
  bool all_interposer = true;
  for (std::size_t index = 0; index < record.mappings.size(); ++index) {
    const auto& required = record.mappings[index];
    const auto imported = FindImport(memory, Value(executable), required.import_module, required.import_name);
    if (!IsOk(imported)) return Failure(imported);
    if (Value(imported).rva != required.rva) return Fail(ErrorCode::UnsupportedBuild, "import_rva");
    const auto* native = NativeModule(inventory, required.native_module);
    if (native == nullptr) return Fail(ErrorCode::UntrustedInterposer, "native_module_trust");
    std::vector<ModuleImage> native_images;
    native_images.reserve(inventory.trusted_system_modules.size());
    for (const auto& module : inventory.trusted_system_modules) {
      if (!module.trusted_system) continue;
      const auto image = InspectPe(memory, module.module.image.base);
      if (!IsOk(image)) return Failure(image);
      native_images.push_back({module.module.name, Value(image)});
    }
    const auto original = ResolveNativeExport(memory, native_images, required.native_module, required.import_name);
    if (!IsOk(original)) return Failure(original);
    const auto replacement_export = FindExport(memory, Value(interposer_image), required.import_name);
    if (!IsOk(replacement_export) || Value(replacement_export).forwarder) return Fail(ErrorCode::UnexpectedImports, "interposer_export");
    Address replacement{};
    if (!Add(inventory.interposer.loaded_module.image.base, Value(replacement_export).rva, replacement)) return Fail(ErrorCode::UnexpectedImports, "interposer_export");
    if (Value(imported).current_pointer == Value(original)) all_interposer = false;
    else if (Value(imported).current_pointer == replacement) all_native = false;
    else return Fail(ErrorCode::UnexpectedImports, "import_provenance");
    result.mappings[index] = {Value(imported).address, Value(original), replacement, required.rva};
  }
  for (std::size_t left = 0; left < result.mappings.size(); ++left) for (std::size_t right = left + 1; right < result.mappings.size(); ++right) if (result.mappings[left].slot == result.mappings[right].slot) return Fail(ErrorCode::UnexpectedImports, "duplicate_slot");
  if (!all_native && !all_interposer) return Fail(ErrorCode::UnexpectedImports, "mixed_routing");
  result.routing = all_interposer ? Routing::AlreadyPresent : Routing::Native;
  return result;
}

Result<FactoryState> CheckFactory(const IMemory& memory, const ValidatedBuild& build) {
  Address manager_address{};
  if (!Add(build.executable_base, build.factory_manager_rva, manager_address)) return Fail(ErrorCode::UnreadableMemory, "manager_address");
  const auto manager = ReadValue<Address>(memory, manager_address, "manager_read");
  if (!IsOk(manager)) return Failure(manager);
  if (Value(manager) == 0) return FactoryState::NotCreated;
  Address factory_address{};
  if (!Add(Value(manager), build.factory_member_offset, factory_address)) return Fail(ErrorCode::UnreadableMemory, "factory_address");
  const auto factory = ReadValue<Address>(memory, factory_address, "factory_read");
  if (!IsOk(factory)) return Failure(factory);
  return Value(factory) == 0 ? FactoryState::NotCreated : FactoryState::Created;
}

Result<ConflictAssessment> ClassifyLoaderConflicts(const RuntimeInventory& inventory, const CompatibilityRecord&) {
  ConflictAssessment assessment;
  for (const auto& candidate : inventory.application_local_loaders) {
    const auto classification = candidate.trusted_signature && candidate.recognized_official ? LoaderClassification::RecognizedOfficial : LoaderClassification::UnclassifiedApplicationLocal;
    assessment.entries.push_back({candidate.basename, candidate.version, candidate.trusted_signature, classification});
    if (classification == LoaderClassification::UnclassifiedApplicationLocal) return Fail(ErrorCode::DllConflict, "application_local_loader");
  }
  return assessment;
}

}  // namespace wfg
