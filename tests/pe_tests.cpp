#include "fixtures/pe_images.hpp"
#include "wfg/pe.hpp"

#include <array>
#include <string>
#include <type_traits>

TEST("pe.certificate_file_offset_outside_image_is_not_read_as_rva") {
  auto image = FixturePe::WithDirectory(4, 0x2000, 0x100);
  class HeaderMemory final : public wfg::IMemory {
  public:
    explicit HeaderMemory(const wfg::IMemory& memory) : memory_(memory) {}
    wfg::Result<wfg::Bytes> Read(wfg::Address address, std::size_t size) const override {
      ++read_count;
      return memory_.Read(address, size);
    }
    wfg::Result<wfg::MemoryRegion> Query(wfg::Address address) const override {
      return memory_.Query(address);
    }
    mutable std::size_t read_count{};
  private:
    const wfg::IMemory& memory_;
  } memory(image.memory);
  REQUIRE_ERROR(image.memory.Read(image.base + 0x2000, 0x100), wfg::ErrorCode::UnreadableMemory);
  const auto pe = REQUIRE_OK(wfg::InspectPe(memory, image.base));
  CHECK(pe.size_of_image == 0x1000);
  CHECK(pe.directories[4].rva == 0x2000);
  CHECK(pe.directories[4].size == 0x100);
  CHECK(memory.read_count == 4);
  CHECK(REQUIRE_OK(wfg::FindExport(image.memory, pe, "Direct")).rva == 0x500);
  CHECK(image.memory.write_count() == 0);
  CHECK(image.memory.protect_count() == 0);
}

TEST("pe.certificate_presence_fields_must_agree") {
  for (const auto fields : std::array<std::array<std::uint32_t, 2>, 2>{{{0, 0x100}, {0x2000, 0}}}) {
    auto image = FixturePe::WithDirectory(4, fields[0], fields[1]);
    REQUIRE_ERROR(wfg::InspectPe(image.memory, image.base), wfg::ErrorCode::MalformedImage);
    CHECK(image.memory.write_count() == 0);
  }
}

TEST("pe.noncertificate_directories_keep_image_bounds") {
  for (std::uint32_t index = 0; index < 16; ++index) {
    if (index == 4) continue;
    for (const auto fields : std::array<std::array<std::uint32_t, 2>, 2>{{{0x2000, 0x100}, {0xff8, 0x10}}}) {
      auto image = FixturePe::WithDirectory(index, fields[0], fields[1]);
      const auto inspected = wfg::InspectPe(image.memory, image.base);
      REQUIRE_ERROR(inspected, wfg::ErrorCode::MalformedImage);
      CHECK(wfg::Failure(inspected).operation.view() == "directory_range");
      CHECK(image.memory.write_count() == 0);
    }
  }
}

TEST("pe.ordinal_import_is_explicit_and_never_name_fallback") {
  auto image=FixturePe::OrdinalImport();const auto pe=REQUIRE_OK(wfg::InspectPe(image.memory,image.base));
  const auto slot=REQUIRE_OK(wfg::FindImport(image.memory,pe,"DXGI.DLL",wfg::PeOrdinal{101}));
  CHECK(slot.rva==0x2B524D8);CHECK(slot.current_pointer==0x1234567812345678ULL);
  REQUIRE_ERROR(wfg::FindImport(image.memory,pe,"dxgi.dll","CreateDXGIFactory2"),wfg::ErrorCode::MalformedImage);
  REQUIRE_ERROR(wfg::FindImport(image.memory,pe,"other.dll",wfg::PeOrdinal{101}),wfg::ErrorCode::MalformedImage);
  REQUIRE_ERROR(wfg::FindImport(image.memory,pe,"dxgi.dll",wfg::PeOrdinal{100}),wfg::ErrorCode::MalformedImage);
  REQUIRE_ERROR(wfg::FindImport(image.memory,pe,"dxgi.dll",wfg::PeOrdinal{0}),wfg::ErrorCode::MalformedImage);
  CHECK(image.memory.write_count()==0);CHECK(image.memory.protect_count()==0);
}
TEST("pe.ordinal_import_rejects_reserved_bits_and_named_impostor") {
  for(const auto token:{0x8000000000010065ULL,0x8000000100000065ULL,0xffff000000000065ULL,0x340ULL}){
    auto image=FixturePe::OrdinalImport(token);const auto pe=REQUIRE_OK(wfg::InspectPe(image.memory,image.base));
    REQUIRE_ERROR(wfg::FindImport(image.memory,pe,"dxgi.dll",wfg::PeOrdinal{101}),wfg::ErrorCode::MalformedImage);
  }
}
TEST("pe.ordinal_import_rejects_duplicate_bound_and_truncated_tables") {
  auto duplicate=FixturePe::OrdinalImport();auto bytes=wfg::Value(duplicate.memory.Read(duplicate.base,0x2B525D8));
  FixturePe::Put<std::uint64_t>(bytes,0x328,0x8000000000000065ULL);
  FixturePe::Image image{duplicate.base,FakeMemory(duplicate.base,std::move(bytes))};
  auto pe=REQUIRE_OK(wfg::InspectPe(image.memory,image.base));
  REQUIRE_ERROR(wfg::FindImport(image.memory,pe,"dxgi.dll",wfg::PeOrdinal{101}),wfg::ErrorCode::MalformedImage);
  auto bound=FixturePe::BoundImportWithoutOriginalNames();pe=REQUIRE_OK(wfg::InspectPe(bound.memory,bound.base));
  REQUIRE_ERROR(wfg::FindImport(bound.memory,pe,"dxgi.dll",wfg::PeOrdinal{101}),wfg::ErrorCode::MalformedImage);
  auto truncated=FixturePe::ImportThunkOutsideImage();pe=REQUIRE_OK(wfg::InspectPe(truncated.memory,truncated.base));
  CHECK(!wfg::IsOk(wfg::FindImport(truncated.memory,pe,"dxgi.dll",wfg::PeOrdinal{101})));
  auto unterminated=FixturePe::UnterminatedImports();pe=REQUIRE_OK(wfg::InspectPe(unterminated.memory,unterminated.base));
  REQUIRE_ERROR(wfg::FindImport(unterminated.memory,pe,"dxgi.dll",wfg::PeOrdinal{101}),wfg::ErrorCode::MalformedImage);
  auto short_read=FixturePe::OrdinalImport();pe=REQUIRE_OK(wfg::InspectPe(short_read.memory,short_read.base));short_read.memory.SetShortReads();
  REQUIRE_ERROR(wfg::FindImport(short_read.memory,pe,"dxgi.dll",wfg::PeOrdinal{101}),wfg::ErrorCode::UnreadableMemory);
}
TEST("pe.ordinal_export_binding_uses_same_image_entry_and_strict_bounds") {
  auto image=FixturePe::ExportImage("D3D12CreateDevice",0x600,{},101);const auto pe=REQUIRE_OK(wfg::InspectPe(image.memory,image.base));
  CHECK(REQUIRE_OK(wfg::FindExport(image.memory,pe,wfg::PeOrdinal{101})).rva==0x600);
  REQUIRE_OK(wfg::BindExportOrdinal(image.memory,pe,"D3D12CreateDevice",wfg::PeOrdinal{101}));
  REQUIRE_ERROR(wfg::BindExportOrdinal(image.memory,pe,"Other",wfg::PeOrdinal{101}),wfg::ErrorCode::MalformedImage);
  REQUIRE_ERROR(wfg::FindExport(image.memory,pe,wfg::PeOrdinal{100}),wfg::ErrorCode::MalformedImage);
  REQUIRE_ERROR(wfg::FindExport(image.memory,pe,wfg::PeOrdinal{102}),wfg::ErrorCode::MalformedImage);
  auto forwarded=FixturePe::ExportImage("D3D12CreateDevice",0,"other.Target",101);const auto fp=REQUIRE_OK(wfg::InspectPe(forwarded.memory,forwarded.base));
  REQUIRE_OK(wfg::BindExportOrdinal(forwarded.memory,fp,"D3D12CreateDevice",wfg::PeOrdinal{101}));
}

TEST("pe.named_import_survives_aslr") {
  auto image = FixturePe::SupportedHeaders(0x00007FF600000000ULL);
  auto pe = REQUIRE_OK(wfg::InspectPe(image.memory, image.base));
  CHECK(pe.base == 0x00007FF600000000ULL);
  CHECK(pe.machine == 0x8664);
  CHECK(pe.timestamp == 0x6AC55CE6);
  CHECK(pe.size_of_image == 0x3000000);
  const auto imported = REQUIRE_OK(wfg::FindImport(image.memory, pe, "DXGI.DLL", "CreateDXGIFactory2"));
  CHECK(imported.rva == 0x2B524D8);
  CHECK(imported.address == image.base + 0x2B524D8);
  CHECK(imported.current_pointer == 0x1234567812345678ULL);
}
TEST("pe.overflow_is_refused") {
  auto image = FixturePe::DirectoryBeyondImage();
  REQUIRE_ERROR(wfg::InspectPe(image.memory, image.base), wfg::ErrorCode::MalformedImage);
  CHECK(image.memory.write_count() == 0);
}
TEST("pe.short_reads_are_refused") {
  auto image = FixturePe::SupportedHeaders(0x00007FF600000000ULL); image.memory.SetShortReads();
  REQUIRE_ERROR(wfg::InspectPe(image.memory, image.base), wfg::ErrorCode::UnreadableMemory);
}

TEST("pe.truncated_headers_are_refused") {
  auto image = FixturePe::TruncatedHeaders();
  REQUIRE_ERROR(wfg::InspectPe(image.memory, image.base), wfg::ErrorCode::UnreadableMemory);
}

TEST("pe.invalid_machine_is_refused") {
  auto image = FixturePe::InvalidMachine();
  REQUIRE_ERROR(wfg::InspectPe(image.memory, image.base), wfg::ErrorCode::MalformedImage);
}

TEST("pe.duplicate_named_imports_are_refused") {
  auto image = FixturePe::DuplicateNamedImports();
  auto pe = REQUIRE_OK(wfg::InspectPe(image.memory, image.base));
  REQUIRE_ERROR(wfg::FindImport(image.memory, pe, "dxgi.dll", "CreateDXGIFactory2"), wfg::ErrorCode::MalformedImage);
}

TEST("pe.bound_import_without_original_names_is_refused") {
  auto image = FixturePe::BoundImportWithoutOriginalNames();
  auto pe = REQUIRE_OK(wfg::InspectPe(image.memory, image.base));
  REQUIRE_ERROR(wfg::FindImport(image.memory, pe, "dxgi.dll", "CreateDXGIFactory2"), wfg::ErrorCode::MalformedImage);
}

TEST("pe.import_directory_requires_a_terminator") {
  auto image = FixturePe::UnterminatedImports();
  auto pe = REQUIRE_OK(wfg::InspectPe(image.memory, image.base));
  REQUIRE_ERROR(wfg::FindImport(image.memory, pe, "dxgi.dll", "CreateDXGIFactory2"), wfg::ErrorCode::MalformedImage);
}

TEST("pe.import_thunks_cannot_escape_image") {
  auto image = FixturePe::ImportThunkOutsideImage();
  auto pe = REQUIRE_OK(wfg::InspectPe(image.memory, image.base));
  REQUIRE_ERROR(wfg::FindImport(image.memory, pe, "dxgi.dll", "CreateDXGIFactory2"), wfg::ErrorCode::MalformedImage);
}

TEST("pe.direct_export_is_bounded") {
  auto image = FixturePe::ExportImage("Target", 0x500);
  auto pe = REQUIRE_OK(wfg::InspectPe(image.memory, image.base));
  auto exported = REQUIRE_OK(wfg::FindExport(image.memory, pe, "Target"));
  CHECK(exported.rva == 0x500);
  CHECK(!exported.forwarder.has_value());
}

TEST("pe.direct_export_cannot_escape_image") {
  auto image = FixturePe::ExportImage("Target", 0x1000);
  auto pe = REQUIRE_OK(wfg::InspectPe(image.memory, image.base));
  REQUIRE_ERROR(wfg::FindExport(image.memory, pe, "Target"), wfg::ErrorCode::MalformedImage);
}

TEST("pe.forwarder_string_cannot_escape_export_directory") {
  const std::string forwarder(0x1B1, 'x');
  auto image = FixturePe::ExportImage("Target", 0x350, forwarder);
  auto pe = REQUIRE_OK(wfg::InspectPe(image.memory, image.base));
  REQUIRE_ERROR(wfg::FindExport(image.memory, pe, "Target"), wfg::ErrorCode::MalformedImage);
}

TEST("pe.forwarded_ordinal_resolves") {
  auto first = FixturePe::ExportImage("Forwarded", 0x350, "second.#7");
  auto second = FixturePe::ExportImage("Other", 0x600, {}, 7, 0x00007FF710000000ULL);
  const std::array modules{wfg::ModuleImage{"first.dll", REQUIRE_OK(wfg::InspectPe(first.memory, first.base))},
                           wfg::ModuleImage{"second.dll", REQUIRE_OK(wfg::InspectPe(second.memory, second.base))}};
  FakeAddressSpace memory;
  memory.Add(first.memory);
  memory.Add(second.memory);
  auto target = REQUIRE_OK(wfg::ResolveNativeExport(memory, modules, "FIRST.DLL", "Forwarded"));
  CHECK(target == second.base + 0x600);
}

TEST("pe.forwarder_cycles_are_refused") {
  auto first = FixturePe::ExportImage("A", 0x350, "second.B");
  auto second = FixturePe::ExportImage("B", 0x350, "first.A", 1, 0x00007FF710000000ULL);
  const std::array modules{wfg::ModuleImage{"first.dll", REQUIRE_OK(wfg::InspectPe(first.memory, first.base))},
                           wfg::ModuleImage{"second.dll", REQUIRE_OK(wfg::InspectPe(second.memory, second.base))}};
  FakeAddressSpace memory;
  memory.Add(first.memory);
  memory.Add(second.memory);
  REQUIRE_ERROR(wfg::ResolveNativeExport(memory, modules, "first.dll", "A"), wfg::ErrorCode::MalformedImage);
}

TEST("pe.forwarder_depth_is_limited") {
  std::vector<FixturePe::Image> images;
  std::vector<wfg::ModuleImage> modules;
  for (unsigned index = 0; index < 10; ++index) {
    const auto name = "m" + std::to_string(index);
    const auto target = index == 9 ? std::string{} : "m" + std::to_string(index + 1) + ".Symbol";
    images.push_back(FixturePe::ExportImage("Symbol", index == 9 ? 0x600 : 0x350, target, 1, 0x00007FF700000000ULL + index * 0x10000ULL));
    modules.push_back({name + ".dll", REQUIRE_OK(wfg::InspectPe(images.back().memory, images.back().base))});
  }
  FakeAddressSpace memory;
  for (const auto& image : images) memory.Add(image.memory);
  REQUIRE_ERROR(wfg::ResolveNativeExport(memory, modules, "m0.dll", "Symbol"), wfg::ErrorCode::MalformedImage);
}

TEST("pe.fixture_directories_stay_within_image") {
  auto image = FixturePe::DirectoryBeyondImage();
  REQUIRE_ERROR(wfg::InspectPe(image.memory, image.base), wfg::ErrorCode::MalformedImage);
  CHECK(image.memory.write_count() == 0);
}

TEST("pe.identity_handles_are_move_only_and_not_pid_constructible") {
  CHECK(!std::is_copy_constructible_v<wfg::OwnedProcess>);
  CHECK(std::is_move_constructible_v<wfg::OwnedProcess>);
  CHECK(std::is_move_assignable_v<wfg::OwnedProcess>);
  CHECK((!std::is_constructible_v<wfg::OwnedProcess, std::uint32_t>));
  CHECK((!std::is_constructible_v<wfg::OwnedProcess, void*, std::uint32_t, std::uint64_t, std::string, std::string, std::string>));
  CHECK(!std::is_copy_constructible_v<wfg::ObservedProcess>);
  CHECK(std::is_move_constructible_v<wfg::ObservedProcess>);
  CHECK(std::is_move_assignable_v<wfg::ObservedProcess>);
  CHECK((!std::is_constructible_v<wfg::ObservedProcess, std::uint32_t>));
  CHECK((!std::is_constructible_v<wfg::ObservedProcess, void*, std::uint32_t, std::uint64_t>));
}

TEST("pe.observation_reason_is_bounded") {
  const std::string long_reason(300, 'x');
  const wfg::Observed<std::string> value{std::string("Windows"), wfg::Availability::Observed, "win32", long_reason};
  CHECK(value.value == "Windows");
  CHECK(value.availability == wfg::Availability::Observed);
  CHECK(value.source == "win32");
  CHECK(value.reason.size() == 256);
}
