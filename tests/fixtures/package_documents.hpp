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

namespace test_fixture {
struct LongPathPinnedPackage {
  struct Child {
    PROCESS_INFORMATION process{};
    ~Child() {
      if(process.hProcess) {
        if(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT){TerminateProcess(process.hProcess,0);WaitForSingleObject(process.hProcess,5000);}
        CloseHandle(process.hThread);CloseHandle(process.hProcess);
      }
    }
  };
  PackageDocuments documents;
  std::filesystem::path plain,extended;
  Child child;
  std::wstring raw_image;
  LongPathPinnedPackage() {
    const auto original=documents.root;CHECK(original.wstring().size()<242);
    plain=original/std::wstring(242-original.wstring().size()-1,L'x');
    extended=std::filesystem::path(L"\\\\?\\"+plain.wstring());
    std::filesystem::create_directory(extended);documents.root=extended;REQUIRE_OK(documents.Resolve());
    const auto directory=extended/L"bin"/L"Gaming.Desktop.x64";std::filesystem::create_directories(directory);
    const auto target=directory/L"witcher3.exe";
    std::array<wchar_t,32768> self{};const auto length=GetModuleFileNameW(nullptr,self.data(),static_cast<DWORD>(self.size()));CHECK(length>0&&length<self.size());
    std::filesystem::copy_file(std::filesystem::path(std::wstring(self.data(),length)),target);
    std::wstring command=L"\""+target.wstring()+L"\"";STARTUPINFOW startup{};startup.cb=sizeof(startup);
    CHECK(CreateProcessW(target.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,nullptr,&startup,&child.process));
    raw_image.resize(32768);DWORD size=static_cast<DWORD>(raw_image.size());
    CHECK(QueryFullProcessImageNameW(child.process.hProcess,0,raw_image.data(),&size));raw_image.resize(size);
    CHECK(raw_image.size()>MAX_PATH);CHECK(!raw_image.starts_with(L"\\\\?\\"));
  }
  wfg::Result<wfg::Candidate> Selected(bool owned,const std::wstring& canonical) const {
    FILETIME creation{},exit{},kernel{},user{};CHECK(GetProcessTimes(child.process.hProcess,&creation,&exit,&kernel,&user));
    const auto created=(static_cast<std::uint64_t>(creation.dwHighDateTime)<<32U)|creation.dwLowDateTime;
    wfg::Candidate candidate;candidate.pid=candidate.pinned_pid=GetProcessId(child.process.hProcess);
    candidate.creation_time=candidate.pinned_creation_time=created;candidate.session=1;
    candidate.user=L"fixture-user";candidate.package=L"fixture-package";candidate.executable=canonical;
    candidate.pin=std::shared_ptr<void>(child.process.hProcess,[](void*)noexcept{});
    const wfg::LaunchIdentity launch{owned?created-1:created+1,1,L"fixture-user",L"fixture-package",canonical,owned};
    return wfg::SelectCandidate(std::span(&candidate,1),launch);
  }
};
}
