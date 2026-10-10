#include "fixtures/runtime_images.hpp"
#include "support.hpp"
#include "wfg/compatibility.hpp"

#include <array>

TEST("compatibility.production_record_has_exact_reviewed_values") {
  const auto& build = wfg::SupportedBuild();
  CHECK(build.package_name == "CDPROJEKTSA.Witcher3GOTY");
  CHECK(build.package_version == "5.0.4.0");
  CHECK(build.pe_timestamp == 0x6AC55CE6U);
  CHECK(build.size_of_image == 0x658F000U);
  CHECK(build.interposer.version == "2.14.1");
  CHECK(build.interposer.length == 652928U);
  CHECK(build.factory_manager_rva == 0x5A75F30U);
  CHECK(build.factory_member_offset == 0x302F568U);
  CHECK(build.mappings[0].rva == 0x2B52450U);
  CHECK(build.mappings[1].rva == 0x2B524D0U);
  CHECK(build.mappings[2].rva == 0x2B524D8U);
  CHECK(build.mappings[3].rva == 0x2B524E0U);
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
}

TEST("compatibility.each_required_build_and_trust_value_refuses") {
  constexpr std::array mutations{Mutation::Package, Mutation::Version, Mutation::Machine, Mutation::Timestamp, Mutation::ImageSize, Mutation::InterposerVersion, Mutation::InterposerLength, Mutation::InterposerHash, Mutation::Signer, Mutation::Correspondence, Mutation::FirstHash, Mutation::SecondHash};
  for (const auto mutation : mutations) {
    auto fixture = SupportedRuntimeFixture();
    auto build = fixture.SyntheticBuild();
    fixture.Mutate(mutation);
    CHECK(!wfg::IsOk(wfg::ValidateBuild(fixture.memory, fixture.inventory, build)));
    CHECK(fixture.memory.write_count() == 0);
    CHECK(fixture.memory.protect_count() == 0);
  }
}

TEST("compatibility.relocated_images_and_native_forwarder_are_valid") {
  auto fixture = SupportedRuntimeFixture(0x00007FF800000000ULL);
  fixture.EnableNativeForwarder();
  const auto validated = REQUIRE_OK(wfg::ValidateBuild(fixture.memory, fixture.inventory, fixture.SyntheticBuild()));
  CHECK(validated.mappings[0].slot != 0);
  CHECK(validated.routing == wfg::Routing::Native);
}

TEST("compatibility.forwarded_interposer_and_mixed_routes_refuse") {
  auto forwarded = SupportedRuntimeFixture();
  forwarded.EnableInterposerForwarder();
  REQUIRE_ERROR(wfg::ValidateBuild(forwarded.memory, forwarded.inventory, forwarded.SyntheticBuild()), wfg::ErrorCode::UnexpectedImports);
  auto mixed = SupportedRuntimeFixture();
  mixed.SetImportPointer("CreateDXGIFactory", mixed.InterposerExport("CreateDXGIFactory"));
  REQUIRE_ERROR(wfg::ValidateBuild(mixed.memory, mixed.inventory, mixed.SyntheticBuild()), wfg::ErrorCode::UnexpectedImports);
}

TEST("compatibility_all_interposer_routing_is_already_present") {
  auto fixture = SupportedRuntimeFixture();
  fixture.SetAllImportsToInterposer();
  const auto validated = REQUIRE_OK(wfg::ValidateBuild(fixture.memory, fixture.inventory, fixture.SyntheticBuild()));
  CHECK(validated.routing == wfg::Routing::AlreadyPresent);
}

TEST("compatibility.factory_guard_handles_null_and_unreadable_layout") {
  auto null_manager = SupportedRuntimeFixture();
  const auto build = REQUIRE_OK(wfg::ValidateBuild(null_manager.memory, null_manager.inventory, null_manager.SyntheticBuild()));
  null_manager.SetManager(0);
  CHECK(REQUIRE_OK(wfg::CheckFactory(null_manager.memory, build)) == wfg::FactoryState::NotCreated);

  auto null_factory = SupportedRuntimeFixture();
  const auto null_factory_build = REQUIRE_OK(wfg::ValidateBuild(null_factory.memory, null_factory.inventory, null_factory.SyntheticBuild()));
  null_factory.SetFactory(0);
  CHECK(REQUIRE_OK(wfg::CheckFactory(null_factory.memory, null_factory_build)) == wfg::FactoryState::NotCreated);

  auto unreadable = SupportedRuntimeFixture();
  const auto unreadable_build = REQUIRE_OK(wfg::ValidateBuild(unreadable.memory, unreadable.inventory, unreadable.SyntheticBuild()));
  unreadable.MakeFactoryUnreadable();
  REQUIRE_ERROR(wfg::CheckFactory(unreadable.memory, unreadable_build), wfg::ErrorCode::UnreadableMemory);

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

TEST("compatibility.unclassified_application_loader_refuses_without_file_changes") {
  auto fixture = SupportedRuntimeFixture();
  fixture.AddApplicationLocalLoader("dxgi.dll", "9.9.9.9", false);
  REQUIRE_ERROR(wfg::ClassifyLoaderConflicts(fixture.inventory, fixture.SyntheticBuild()), wfg::ErrorCode::DllConflict);
  CHECK(fixture.memory.write_count() == 0);
  CHECK(fixture.memory.protect_count() == 0);
}

TEST("compatibility_loader_filename_alone_never_identifies_mod_or_authorizes_chainload") {
  auto fixture = SupportedRuntimeFixture();
  fixture.AddApplicationLocalLoader("dxgi.dll", "", true);
  const auto assessment = REQUIRE_OK(wfg::ClassifyLoaderConflicts(fixture.inventory, fixture.SyntheticBuild()));
  CHECK(assessment.entries.size() == 1);
  CHECK(assessment.entries[0].classification == wfg::LoaderClassification::RecognizedOfficial);
  CHECK(assessment.entries[0].basename == "dxgi.dll");
  CHECK(fixture.memory.write_count() == 0);
}
