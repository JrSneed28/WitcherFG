#include "fixtures/runtime_images.hpp"
#include "support.hpp"
#include "wfg/compatibility.hpp"

#include <array>

namespace {
std::string HexString(const wfg::Hash256& hash) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : hash) {
    const auto value = std::to_integer<unsigned>(byte);
    result.push_back(digits[value >> 4]);
    result.push_back(digits[value & 15]);
  }
  return result;
}
}

TEST("compatibility.production_record_has_exact_reviewed_values") {
  const auto& build = wfg::SupportedBuild();
  CHECK(build.package_name == "CDPROJEKTSA.Witcher3GOTY");
  CHECK(build.package_version == "5.0.4.0");
  CHECK(build.machine == 0x8664);
  CHECK(build.pe_timestamp == 0x6AC55CE6U);
  CHECK(build.size_of_image == 0x658F000U);
  CHECK(build.interposer.version == "2.14.1");
  CHECK(build.interposer.length == 652928U);
  CHECK(HexString(build.interposer.sha256) == "8c87c9499461da561edd529aa9bf7831d67d7b94ebb1c1a5ed54ef4934e1ea4c");
  CHECK(build.fingerprints[0].rva == 0x1CB0384U);
  CHECK(build.fingerprints[0].length == 32U);
  CHECK(HexString(build.fingerprints[0].sha256) == "bea2f23fae85c0d5ae5a517bb573945525addd04bb52ce4ae239b7da944b7c09");
  CHECK(build.fingerprints[1].rva == 0x1CB061DU);
  CHECK(build.fingerprints[1].length == 32U);
  CHECK(HexString(build.fingerprints[1].sha256) == "88f7cb6bdd8c6d3285de7ab6688c00d98d10d189e2906136aa8432b5afe3e736");
  CHECK(build.factory_manager_rva == 0x5A75F30U);
  CHECK(build.factory_member_offset == 0x302F568U);
  CHECK(build.mappings[0].rva == 0x2B52450U);
  CHECK(build.mappings[1].rva == 0x2B524D0U);
  CHECK(build.mappings[2].rva == 0x2B524D8U);
  CHECK(build.mappings[3].rva == 0x2B524E0U);
  constexpr std::array modules{"d3d12.dll", "dxgi.dll", "dxgi.dll", "dxgi.dll"};
  constexpr std::array names{"D3D12CreateDevice", "CreateDXGIFactory1", "CreateDXGIFactory2", "CreateDXGIFactory"};
  for (std::size_t index = 0; index < names.size(); ++index) {
    CHECK(build.mappings[index].import_module == modules[index]);
    CHECK(build.mappings[index].import_name == names[index]);
    CHECK(build.mappings[index].native_module == modules[index]);
  }
}

TEST("compatibility.ordinal_record_keeps_four_mandatory_slots") {
  CHECK(wfg::SupportedBuild().mappings[0].import_ordinal.has_value());
  CHECK(wfg::SupportedBuild().mappings[0].import_ordinal->value==101);
  for(std::size_t i=1;i<4;++i)CHECK(!wfg::SupportedBuild().mappings[i].import_ordinal.has_value());
  SupportedRuntimeFixture f;const auto v=REQUIRE_OK(wfg::ValidateBuild(f.memory,f.inventory,f.SyntheticBuild()));
  CHECK(v.mappings.size()==4);CHECK(v.mappings[0].rva==0x2b52450);CHECK(f.memory.write_count()==0);CHECK(f.memory.protect_count()==0);
}
TEST("compatibility.ordinal_native_binding_mismatch_refuses_before_changes") {
  SupportedRuntimeFixture f;
  // Native name is now ordinal102 while the imported101 points at a different entry.
  f.memory.Put<std::uint32_t>(0x00007FF700000214ULL,2);
  f.memory.Put<std::uint32_t>(0x00007FF700000284ULL,0x610);
  f.memory.Put<std::uint16_t>(0x00007FF7000002c0ULL,1);
  const auto outcome=wfg::ValidateBuild(f.memory,f.inventory,f.SyntheticBuild());
  REQUIRE_ERROR(outcome,wfg::ErrorCode::MalformedImage);CHECK(wfg::Failure(outcome).operation.view()=="ordinal_name_binding");
  CHECK(f.memory.write_count()==0);CHECK(f.memory.protect_count()==0);
}
TEST("compatibility.ordinal_missing_wrong_rva_and_mixed_selector_refuse") {
  for(int change=0;change<4;++change){SupportedRuntimeFixture f;auto record=f.SyntheticBuild();
    switch(change){case 0:f.memory.Put<std::uint64_t>(f.inventory.executable.image.base+0x1000,0x8000000000000066ULL);break;
      case 1:record.mappings[0].rva+=8;break;case 2:f.memory.Put<std::uint64_t>(f.inventory.executable.image.base+0x1000,0);break;
      case 3:f.memory.Put<std::uint64_t>(f.inventory.executable.image.base+0x1008,0x600);break;}
    const auto outcome=wfg::ValidateBuild(f.memory,f.inventory,record);CHECK(!wfg::IsOk(outcome));
    if(change==3)CHECK(wfg::Failure(outcome).operation.view()=="ambiguous_import_selector");
    CHECK(f.memory.write_count()==0);CHECK(f.memory.protect_count()==0);
  }
}
TEST("compatibility.ordinal_binding_keeps_trusted_forwarder_resolution") {
  for(const bool trusted:{true,false}){SupportedRuntimeFixture f;
    RuntimeFixtureDetail::Text(f.memory,0x00007FF700000400ULL,"dxcore.CreateDXGIFactory1");
    f.memory.Put<std::uint32_t>(0x00007FF700000280ULL,0x400);
    f.SetImportPointer("D3D12CreateDevice",0x00007FF720000600ULL);
    f.inventory.trusted_system_modules[2].trusted_system=trusted;
    const auto outcome=wfg::ValidateBuild(f.memory,f.inventory,f.SyntheticBuild());
    CHECK(wfg::IsOk(outcome)==trusted);CHECK(f.memory.write_count()==0);CHECK(f.memory.protect_count()==0);
  }
}

TEST("compatibility.supported_runtime_authorizes_native_routing") {
  auto fixture = SupportedRuntimeFixture();
  const auto build = fixture.SyntheticBuild();
  const auto validated = REQUIRE_OK(wfg::ValidateBuild(fixture.memory, fixture.inventory, build));
  CHECK(validated.routing == wfg::Routing::Native);
  CHECK(validated.mappings.size() == 4);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility.update_or_hook_cannot_authorize_writes") {
  auto fixture = SupportedRuntimeFixture();
  fixture.SetImportPointer("CreateDXGIFactory2", fixture.foreign_hook);
  REQUIRE_ERROR(wfg::ValidateBuild(fixture.memory, fixture.inventory, fixture.SyntheticBuild()), wfg::ErrorCode::UnexpectedImports);
  CHECK(fixture.memory.protect_count() == 0);
  CHECK(fixture.memory.write_count() == 0);
}

TEST("compatibility.created_factory_blocks_patch") {
  auto fixture = SupportedRuntimeFixture();
  const auto build = REQUIRE_OK(wfg::ValidateBuild(fixture.memory, fixture.inventory, fixture.SyntheticBuild()));
  fixture.SetFactory(fixture.native_factory);
  CHECK(REQUIRE_OK(wfg::CheckFactory(fixture.memory, build)) == wfg::FactoryState::Created);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility.each_required_build_and_trust_value_refuses") {
  constexpr std::array mutations{Mutation::Package, Mutation::Version, Mutation::Machine, Mutation::Timestamp, Mutation::ImageSize, Mutation::InterposerVersion, Mutation::InterposerLength, Mutation::InterposerHash, Mutation::Signer, Mutation::Correspondence, Mutation::FirstHash, Mutation::SecondHash};
  for (const auto mutation : mutations) {
    auto fixture = SupportedRuntimeFixture();
    auto build = fixture.SyntheticBuild();
    fixture.Mutate(mutation);
    const auto expected = mutation == Mutation::Machine ? wfg::ErrorCode::MalformedImage
        : mutation == Mutation::Signer || mutation == Mutation::Correspondence ? wfg::ErrorCode::UntrustedInterposer
        : wfg::ErrorCode::UnsupportedBuild;
    REQUIRE_ERROR(wfg::ValidateBuild(fixture.memory, fixture.inventory, build), expected);
    CHECK(fixture.memory.write_count() == 0);
    CHECK(fixture.memory.protect_count() == 0);
  }
}

TEST("compatibility.each_fingerprint_field_is_required") {
  enum class Field { Rva, Length, Digest };
  for (std::size_t index = 0; index < 2; ++index) {
    for (const auto field : {Field::Rva, Field::Length, Field::Digest}) {
      auto fixture = SupportedRuntimeFixture();
      auto record = fixture.SyntheticBuild();
      switch (field) {
        case Field::Rva: ++record.fingerprints[index].rva; break;
        case Field::Length: --record.fingerprints[index].length; break;
        case Field::Digest: record.fingerprints[index].sha256[0] ^= std::byte{1}; break;
      }
      REQUIRE_ERROR(wfg::ValidateBuild(fixture.memory, fixture.inventory, record), wfg::ErrorCode::UnsupportedBuild);
      CHECK(fixture.memory.write_count() == 0);
      CHECK(fixture.memory.protect_count() == 0);
    }
  }
}

TEST("compatibility.each_mapping_field_is_required") {
  enum class Field { ImportModule, ImportName, NativeModule, Rva };
  for (std::size_t index = 0; index < 4; ++index) {
    for (const auto field : {Field::ImportModule, Field::ImportName, Field::NativeModule, Field::Rva}) {
      auto fixture = SupportedRuntimeFixture();
      auto record = fixture.SyntheticBuild();
      switch (field) {
        case Field::ImportModule: record.mappings[index].import_module = "other.dll"; break;
        case Field::ImportName: record.mappings[index].import_name = "OtherFunction"; break;
        case Field::NativeModule: record.mappings[index].native_module = "other.dll"; break;
        case Field::Rva: record.mappings[index].rva += 8; break;
      }
      const auto expected = field == Field::Rva ? wfg::ErrorCode::UnsupportedBuild
          : field == Field::NativeModule ? wfg::ErrorCode::UntrustedInterposer : wfg::ErrorCode::MalformedImage;
      REQUIRE_ERROR(wfg::ValidateBuild(fixture.memory, fixture.inventory, record), expected);
      CHECK(fixture.memory.write_count() == 0);
      CHECK(fixture.memory.protect_count() == 0);
    }
  }
}

TEST("compatibility.relocated_images_and_native_forwarder_are_valid") {
  auto fixture = SupportedRuntimeFixture(0x00007FF800000000ULL);
  fixture.EnableNativeForwarder();
  const auto validated = REQUIRE_OK(wfg::ValidateBuild(fixture.memory, fixture.inventory, fixture.SyntheticBuild()));
  CHECK(validated.mappings[0].slot != 0);
  CHECK(validated.routing == wfg::Routing::Native);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility.trusted_ordinal_forwarder_authorizes_remote_target") {
  auto fixture = SupportedRuntimeFixture();
  fixture.EnableNativeOrdinalForwarder();
  const auto validated = REQUIRE_OK(wfg::ValidateBuild(fixture.memory, fixture.inventory, fixture.SyntheticBuild()));
  CHECK(validated.routing == wfg::Routing::Native);
  CHECK(validated.mappings[1].original == 0x00007FF720000600ULL);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility.trusted_forwarder_cycle_refuses") {
  auto fixture = SupportedRuntimeFixture();
  fixture.EnableNativeForwarderCycle();
  const auto result = wfg::ValidateBuild(fixture.memory, fixture.inventory, fixture.SyntheticBuild());
  CHECK(!wfg::IsOk(result));
  CHECK(wfg::Failure(result).code == wfg::ErrorCode::MalformedImage);
  CHECK(wfg::Failure(result).operation.view() == "forwarder_cycle");
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility.forwarded_interposer_and_mixed_routes_refuse") {
  auto forwarded = SupportedRuntimeFixture();
  forwarded.EnableInterposerForwarder();
  REQUIRE_ERROR(wfg::ValidateBuild(forwarded.memory, forwarded.inventory, forwarded.SyntheticBuild()), wfg::ErrorCode::UnexpectedImports);
  CHECK(forwarded.memory.write_count() == 0);
  CHECK(forwarded.memory.protect_count() == 0);
  auto mixed = SupportedRuntimeFixture();
  mixed.SetImportPointer("CreateDXGIFactory", mixed.InterposerExport("CreateDXGIFactory"));
  REQUIRE_ERROR(wfg::ValidateBuild(mixed.memory, mixed.inventory, mixed.SyntheticBuild()), wfg::ErrorCode::UnexpectedImports);
  CHECK(mixed.memory.write_count() == 0);
  CHECK(mixed.memory.protect_count() == 0);
}

TEST("compatibility_all_interposer_routing_is_already_present") {
  auto fixture = SupportedRuntimeFixture();
  fixture.SetAllImportsToInterposer();
  const auto validated = REQUIRE_OK(wfg::ValidateBuild(fixture.memory, fixture.inventory, fixture.SyntheticBuild()));
  CHECK(validated.routing == wfg::Routing::AlreadyPresent);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility.factory_guard_handles_null_and_unreadable_layout") {
  auto null_manager = SupportedRuntimeFixture();
  const auto build = REQUIRE_OK(wfg::ValidateBuild(null_manager.memory, null_manager.inventory, null_manager.SyntheticBuild()));
  null_manager.SetManager(0);
  CHECK(REQUIRE_OK(wfg::CheckFactory(null_manager.memory, build)) == wfg::FactoryState::NotCreated);
  CHECK(null_manager.memory.write_count() == 0);
  CHECK(null_manager.memory.protect_count() == 0);

  auto null_factory = SupportedRuntimeFixture();
  const auto null_factory_build = REQUIRE_OK(wfg::ValidateBuild(null_factory.memory, null_factory.inventory, null_factory.SyntheticBuild()));
  null_factory.SetFactory(0);
  CHECK(REQUIRE_OK(wfg::CheckFactory(null_factory.memory, null_factory_build)) == wfg::FactoryState::NotCreated);
  CHECK(null_factory.memory.write_count() == 0);
  CHECK(null_factory.memory.protect_count() == 0);

  auto unreadable = SupportedRuntimeFixture();
  const auto unreadable_build = REQUIRE_OK(wfg::ValidateBuild(unreadable.memory, unreadable.inventory, unreadable.SyntheticBuild()));
  unreadable.MakeFactoryUnreadable();
  REQUIRE_ERROR(wfg::CheckFactory(unreadable.memory, unreadable_build), wfg::ErrorCode::UnreadableMemory);
  CHECK(unreadable.memory.write_count() == 0);
  CHECK(unreadable.memory.protect_count() == 0);

  auto unreadable_manager = SupportedRuntimeFixture();
  const auto unreadable_manager_build = REQUIRE_OK(wfg::ValidateBuild(unreadable_manager.memory, unreadable_manager.inventory, unreadable_manager.SyntheticBuild()));
  unreadable_manager.MakeManagerUnreadable();
  REQUIRE_ERROR(wfg::CheckFactory(unreadable_manager.memory, unreadable_manager_build), wfg::ErrorCode::UnreadableMemory);
  CHECK(unreadable_manager.memory.write_count() == 0);
  CHECK(unreadable_manager.memory.protect_count() == 0);
}

TEST("compatibility.untrusted_system_export_refuses_before_any_mutation") {
  auto fixture = SupportedRuntimeFixture();
  fixture.SetNativeTrusted(false);
  REQUIRE_ERROR(wfg::ValidateBuild(fixture.memory, fixture.inventory, fixture.SyntheticBuild()), wfg::ErrorCode::UntrustedInterposer);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility.each_native_module_and_forwarder_target_requires_trust") {
  for (std::size_t index = 0; index < 3; ++index) {
    auto fixture = SupportedRuntimeFixture();
    fixture.EnableNativeForwarder();
    fixture.inventory.trusted_system_modules[index].trusted_system = false;
    const auto expected = index == 2 ? wfg::ErrorCode::MalformedImage : wfg::ErrorCode::UntrustedInterposer;
    REQUIRE_ERROR(wfg::ValidateBuild(fixture.memory, fixture.inventory, fixture.SyntheticBuild()), expected);
    CHECK(fixture.memory.write_count() == 0);
    CHECK(fixture.memory.protect_count() == 0);
  }
}

TEST("compatibility.unclassified_application_loader_refuses_without_file_changes") {
  auto fixture = SupportedRuntimeFixture();
  fixture.AddApplicationLocalLoader("dxgi.dll", "9.9.9.9", false);
  REQUIRE_ERROR(wfg::ClassifyLoaderConflicts(fixture.inventory, fixture.SyntheticBuild()), wfg::ErrorCode::DllConflict);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility_loader_filename_alone_never_identifies_mod_or_authorizes_chainload") {
  auto fixture = SupportedRuntimeFixture();
  fixture.AddApplicationLocalLoader("dxgi.dll", "", false);
  REQUIRE_ERROR(wfg::ClassifyLoaderConflicts(fixture.inventory, fixture.SyntheticBuild()), wfg::ErrorCode::DllConflict);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility.authenticated_official_loader_with_same_basename_passes") {
  auto fixture = SupportedRuntimeFixture();
  fixture.AddApplicationLocalLoader("dxgi.dll", "", true);
  const auto assessment = REQUIRE_OK(wfg::ClassifyLoaderConflicts(fixture.inventory, fixture.SyntheticBuild()));
  CHECK(assessment.entries.size() == 1);
  CHECK(assessment.entries[0].classification == wfg::LoaderClassification::RecognizedOfficial);
  CHECK(assessment.entries[0].basename == "dxgi.dll");
  CHECK(assessment.entries[0].version.empty());
  CHECK(assessment.entries[0].trusted_signature);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility.official_loader_without_authenticated_signature_refuses") {
  auto fixture = SupportedRuntimeFixture();
  fixture.AddApplicationLocalLoader("dxgi.dll", "", true);
  fixture.inventory.application_local_loaders[0].trusted_signature = false;
  REQUIRE_ERROR(wfg::ClassifyLoaderConflicts(fixture.inventory, fixture.SyntheticBuild()), wfg::ErrorCode::DllConflict);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}
