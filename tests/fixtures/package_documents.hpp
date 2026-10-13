#pragma once
#include "wfg/windows_process.hpp"
#include <filesystem>
#include <fstream>
#include <windows.h>

namespace test_fixture {
struct PackageDocuments {
  std::filesystem::path root, cleanup_root;
  wfg::process_detail::RegisteredPackageMetadata identity{L"CDPROJEKTSA.Witcher3GOTY", L"CN=Test Publisher", L"5.0.4.0", L"TEST", L"x64"};
  std::string manifest = R"(<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"><Identity Name="CDPROJEKTSA.Witcher3GOTY" Publisher="CN=Test Publisher" Version="5.0.4.0" ResourceId="TEST" ProcessorArchitecture="x64"/><Applications><Application Id="bin.Gaming.Desktop.x64.witcher3" Executable="GameLaunchHelper.exe" EntryPoint="Windows.FullTrustApplication"/></Applications></Package>)";
  std::string config = R"(<Game configVersion="1"><Identity Name="CDPROJEKTSA.Witcher3GOTY" Publisher="CN=Test Publisher" Version="5.0.4.0" ResourceId="TEST"/><ExecutableList><Executable Name="bin\Gaming.Desktop.x64\witcher3.exe" TargetDeviceFamily="PC"/></ExecutableList></Game>)";
  PackageDocuments() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i=0;i<10;++i) {
      root=std::filesystem::temp_directory_path()/("wfg-package-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(nonce)+"-"+std::to_string(i));
      if(std::filesystem::create_directory(root)){cleanup_root=root;return;}
    }
    throw std::runtime_error("package fixture directory");
  }
  ~PackageDocuments(){std::error_code error;std::filesystem::remove_all(root,error);if(cleanup_root!=root)std::filesystem::remove_all(cleanup_root,error);}
  static void Replace(std::string& text,std::string_view from,std::string_view to){const auto at=text.find(from);CHECK(at!=std::string::npos);text.replace(at,from.size(),to);}
  wfg::Result<std::wstring> Resolve(bool write_config=true){
    {std::ofstream file(root/L"AppxManifest.xml",std::ios::binary);file<<manifest;CHECK(file.good());}
    if(write_config){std::ofstream file(root/L"MicrosoftGame.Config",std::ios::binary);file<<config;CHECK(file.good());}
    return wfg::process_detail::ResolvePackageExecutable(root.wstring(),identity);
  }
};
}
