#include "fixtures/pe_images.hpp"
#include "wfg/pe.hpp"

TEST("pe.named_import_survives_aslr") {
  auto image = FixturePe::SupportedHeaders(0x00007FF600000000ULL);
  auto pe = REQUIRE_OK(wfg::InspectPe(image.memory, image.base));
  CHECK(REQUIRE_OK(wfg::FindImport(image.memory, pe, "dxgi.dll", "CreateDXGIFactory2")).rva == 0x2B524D8);
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
