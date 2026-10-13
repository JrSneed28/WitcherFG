#include "support.hpp"
#include "wfg/diagnostics.hpp"
#include "../src/local_storage_windows.hpp"
#include <array>
#include <cstdlib>

namespace {
using namespace wfg::local_storage;
struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
    (L"wfg-local-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  Fixture(){ std::filesystem::create_directory(root); }
  ~Fixture(){std::error_code error;std::filesystem::remove_all(root,error);}
};
}
TEST("local_storage.single_components_and_provenance_fail_before_open") {
  for(const auto name:{L"..",L".",L"a\\b",L"a/b",L"a:stream",L""}) {
    auto file=RelativeOpen(INVALID_HANDLE_VALUE,name,GENERIC_READ,FILE_SHARE_READ,1);
    CHECK(!file); CHECK(GetLastError()==ERROR_INVALID_PARAMETER);
  }
  CHECK(!ClassifyLocalVolume(L"\\\\server\\share\\a",DRIVE_FIXED,L"\\Device\\HarddiskVolume1"));
  CHECK(!ClassifyLocalVolume(L"C:\\a",DRIVE_REMOTE,L"\\Device\\HarddiskVolume1"));
  CHECK(!ClassifyLocalVolume(L"C:\\a",DRIVE_FIXED,L"\\Device\\Mup\\server"));
  CHECK(!ClassifyLocalVolume(L"C:\\a",DRIVE_FIXED,L"\\??\\D:\\alias"));
  CHECK(ClassifyLocalVolume(L"C:\\a",DRIVE_FIXED,L"\\Device\\HarddiskVolume1")==L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\");
}
TEST("local_storage.child_lease") {
  wchar_t root[32768]{}, event[64]{};
  if(!GetEnvironmentVariableW(L"WFG_STORAGE_CHILD_ROOT",root,32768))return;
  CHECK(GetEnvironmentVariableW(L"WFG_STORAGE_CHILD_EVENT",event,64));
  wfg::FileLogStorage writer(root);
  REQUIRE_OK(writer.WriteSummaryAtomically("{}"));
  CHECK(SetEvent(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::wcstoull(event,nullptr,10)))));
  Sleep(INFINITE);
}
TEST("local_storage.crashed_writer_releases_or_retained_object_refuses") {
  for(bool retain:{false,true}) {
    Fixture fixture;
    std::array<wchar_t,32768> executable{};CHECK(GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size())));
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    NativeHandle ready(CreateEventW(&security,TRUE,FALSE,nullptr));CHECK(ready.get()!=nullptr);
    CHECK(SetEnvironmentVariableW(L"WFG_STORAGE_CHILD_ROOT",fixture.root.c_str()));
    const auto event=std::to_wstring(reinterpret_cast<std::uintptr_t>(ready.get()));
    CHECK(SetEnvironmentVariableW(L"WFG_STORAGE_CHILD_EVENT",event.c_str()));
    std::wstring command=L"\""+std::wstring(executable.data())+L"\" --filter local_storage.child_lease";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    const bool created=CreateProcessW(executable.data(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;
    SetEnvironmentVariableW(L"WFG_STORAGE_CHILD_ROOT",nullptr);SetEnvironmentVariableW(L"WFG_STORAGE_CHILD_EVENT",nullptr);
    CHECK(created);
    NativeHandle child(process.hProcess), thread(process.hThread);
    struct Kill { HANDLE process; ~Kill(){TerminateProcess(process,123);WaitForSingleObject(process,2000);} } cleanup{child.get()};
    CHECK(WaitForSingleObject(ready.get(),5000)==WAIT_OBJECT_0);
    NativeHandle retained;
    if(retain) {
      NativeHandle directory(CreateFileW(fixture.root.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
      BY_HANDLE_FILE_INFORMATION info{};CHECK(GetFileInformationByHandle(directory.get(),&info));
      const auto name=L"Local\\WitcherFG.storage."+std::to_wstring(info.dwVolumeSerialNumber)+L"."+std::to_wstring(info.nFileIndexHigh)+L"."+std::to_wstring(info.nFileIndexLow);
      retained=NativeHandle(OpenMutexW(SYNCHRONIZE,FALSE,name.c_str()));CHECK(retained.get()!=nullptr);
    }
    CHECK(TerminateProcess(child.get(),123));CHECK(WaitForSingleObject(child.get(),2000)==WAIT_OBJECT_0);
    wfg::FileLogStorage next(fixture.root);
    if(retain) {REQUIRE_ERROR(next.AppendEvent("{}",2,5),wfg::ErrorCode::DiagnosticFailure);retained={};}
    REQUIRE_OK(next.AppendEvent("{}",2,5));
  }
}
