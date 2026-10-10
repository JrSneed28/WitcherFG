#pragma once

#include "wfg/pe.hpp"

#include <array>
#include <string>

namespace wfg {

struct CodeFingerprint { Rva rva{}; std::uint32_t length{}; Hash256 sha256{}; };
struct MappingRecord { std::string import_module; std::string import_name; std::string native_module; Rva rva{}; };
struct InterposerRequirement { std::string version; std::uint64_t length{}; Hash256 sha256{}; };
struct CompatibilityRecord {
  std::string package_name;
  std::string package_version;
  std::uint16_t machine{};
  std::uint32_t pe_timestamp{};
  std::uint32_t size_of_image{};
  InterposerRequirement interposer;
  std::array<CodeFingerprint, 2> fingerprints{};
  std::array<MappingRecord, 4> mappings{};
  Rva factory_manager_rva{};
  std::uint64_t factory_member_offset{};
};

struct TrustedModule { ModuleImage module; bool trusted_system{}; };
struct InterposerEvidence {
  ModuleImage loaded_module;
  std::string version;
  std::uint64_t length{};
  Hash256 sha256{};
  bool trusted_nvidia_signer{};
  bool backing_image_matches_loaded_module{};
};
struct LoaderCandidate { std::string basename; std::string version; bool trusted_signature{}; bool recognized_official{}; };
enum class DependencyRole { Interposer, StreamlinePlugin, NgxPlugin, ApplicationLocalLoader };
enum class DependencySigner { Nvidia, Microsoft };
struct DependencyIdentity {
  std::string basename;
  Observed<std::string> version;
  Observed<Hash256> sha256;
  Observed<DependencySigner> signer;
  Observed<DependencyRole> role;
  Availability availability{Availability::NotObserved};
  std::string source;
};
struct RuntimeInventory {
  std::string package_name;
  std::string package_version;
  ModuleImage executable;
  std::vector<TrustedModule> trusted_system_modules;
  InterposerEvidence interposer;
  std::vector<LoaderCandidate> application_local_loaders;
  std::vector<DependencyIdentity> dependencies;
};

struct PointerMapping { Address slot{}; Address original{}; Address replacement{}; Rva rva{}; };
enum class Routing { Native, AlreadyPresent };
struct ValidatedBuild {
  std::array<PointerMapping, 4> mappings{};
  Rva factory_manager_rva{};
  std::uint64_t factory_member_offset{};
  Address executable_base{};
  Routing routing{Routing::Native};
};
enum class FactoryState { NotCreated, Created };
enum class LoaderClassification { RecognizedOfficial, UnclassifiedApplicationLocal };
struct LoaderConflict { std::string basename; std::string version; bool trusted_signature{}; LoaderClassification classification{}; };
struct ConflictAssessment { std::vector<LoaderConflict> entries; };

const CompatibilityRecord& SupportedBuild();
Result<ValidatedBuild> ValidateBuild(const IMemory& memory, const RuntimeInventory& inventory, const CompatibilityRecord& record);
Result<FactoryState> CheckFactory(const IMemory& memory, const ValidatedBuild& build);
Result<ConflictAssessment> ClassifyLoaderConflicts(const RuntimeInventory& inventory, const CompatibilityRecord& record);

}  // namespace wfg
