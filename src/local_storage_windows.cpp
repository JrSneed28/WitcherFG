#include "local_storage_windows.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <shlobj.h>
#include <sddl.h>
#include <winternl.h>
namespace wfg::local_storage {
namespace {
bool Component(std::wstring_view name) {
  return !name.empty() && name.size() <= 120 && name != L"." && name != L".." &&
    name.find_first_of(L"\\/:\0", 0, 4) == std::wstring_view::npos;
}
}
NativeHandle RelativeOpen(HANDLE directory, std::wstring_view name, ACCESS_MASK access, ULONG sharing, ULONG disposition, bool is_directory) {
  using OpenFunction = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
  using ErrorFunction = ULONG (WINAPI*)(NTSTATUS);
  const auto module = GetModuleHandleW(L"ntdll.dll");
  const auto open = reinterpret_cast<OpenFunction>(GetProcAddress(module,"NtCreateFile"));
  const auto error = reinterpret_cast<ErrorFunction>(GetProcAddress(module,"RtlNtStatusToDosError"));
  if (!open || !error || !Component(name)) { SetLastError(ERROR_INVALID_PARAMETER); return NativeHandle{}; }
  UNICODE_STRING text{};
  text.Buffer = const_cast<PWSTR>(name.data());
  text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t)); text.MaximumLength = text.Length;
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes); attributes.RootDirectory = directory; attributes.ObjectName = &text;
  attributes.Attributes = 0x40U | 0x1000U;
  IO_STATUS_BLOCK status{}; HANDLE handle{};
  const auto result = open(&handle,access | SYNCHRONIZE,&attributes,&status,nullptr,FILE_ATTRIBUTE_NORMAL,sharing,disposition,
      (is_directory ? 0x1U : 0x40U) | 0x20U | 0x00200000U,nullptr,0);
  if (result < 0) { SetLastError(error(result)); return NativeHandle{}; }
  return NativeHandle(handle);
}
bool Same(const BY_HANDLE_FILE_INFORMATION& first, const BY_HANDLE_FILE_INFORMATION& second) {
  return first.dwVolumeSerialNumber == second.dwVolumeSerialNumber && first.nFileIndexHigh == second.nFileIndexHigh &&
         first.nFileIndexLow == second.nFileIndexLow && first.nNumberOfLinks == second.nNumberOfLinks &&
         first.nFileSizeHigh == second.nFileSizeHigh && first.nFileSizeLow == second.nFileSizeLow &&
         first.ftLastWriteTime.dwHighDateTime == second.ftLastWriteTime.dwHighDateTime &&
         first.ftLastWriteTime.dwLowDateTime == second.ftLastWriteTime.dwLowDateTime;
}

DirectoryLease::~DirectoryLease() { if(mutex_)ReleaseMutex(mutex_.get()); }
bool DirectoryLease::Acquire(HANDLE directory) {
 if(mutex_)return true;
 BY_HANDLE_FILE_INFORMATION info{};
 if(!GetFileInformationByHandle(directory,&info))return false;
 const auto name=L"Local\\WitcherFG.storage."+std::to_wstring(info.dwVolumeSerialNumber)+L"."+
   std::to_wstring(info.nFileIndexHigh)+L"."+std::to_wstring(info.nFileIndexLow);
 // Initial ownership only on creation: an existing object always refuses,
 // including a second instance on the same thread (no recursive acquisition).
 HANDLE token_raw{};
 if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token_raw))return false;
 NativeHandle token(token_raw);
 DWORD length{};GetTokenInformation(token.get(),TokenUser,nullptr,0,&length);
 if(!length)return false;
 std::vector<std::byte> user(length);
 if(!GetTokenInformation(token.get(),TokenUser,user.data(),length,&length))return false;
 LPWSTR sid{};
 if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,&sid))return false;
 const auto descriptor=std::wstring(L"D:P(A;;GA;;;")+sid+L")";LocalFree(sid);
 PSECURITY_DESCRIPTOR security{};
 if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(),SDDL_REVISION_1,&security,nullptr))return false;
 SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),security,FALSE};
 HANDLE raw=CreateMutexW(&attributes,TRUE,name.c_str());
 const auto creation_error=GetLastError();LocalFree(security);SetLastError(creation_error);
 const auto error=GetLastError();
 if(!raw)return false;
 if(error==ERROR_ALREADY_EXISTS){CloseHandle(raw);SetLastError(ERROR_SHARING_VIOLATION);return false;}
 mutex_=NativeHandle(raw);return true;
}
bool LocalPathSyntax(const std::filesystem::path& root) {
 const auto drive=root.root_name().native();
 if (!root.is_absolute() || drive.size()!=2 || drive[1]!=L':' ||
    !((drive[0]>=L'A'&&drive[0]<=L'Z')||(drive[0]>=L'a'&&drive[0]<=L'z'))) return false;
 for(const auto& part:root.relative_path()) if(!Component(part.native())) return false;
 return true;
}
std::optional<std::wstring> ClassifyLocalVolume(const std::filesystem::path& root, UINT type, std::wstring_view device) {
 constexpr std::wstring_view prefix=L"\\Device\\HarddiskVolume";
 if (!LocalPathSyntax(root) || type!=DRIVE_FIXED || !device.starts_with(prefix) || device.size()==prefix.size() ||
   !std::all_of(device.begin()+prefix.size(),device.end(),[](wchar_t c){return c>=L'0'&&c<=L'9';})) {
  SetLastError(ERROR_NOT_SUPPORTED); return std::nullopt;
 }
 return std::wstring(L"\\\\?\\GLOBALROOT")+std::wstring(device)+L"\\";
}
std::optional<std::wstring> LocalVolumeRoot(const std::filesystem::path& root) {
 if(!LocalPathSyntax(root)){SetLastError(ERROR_NOT_SUPPORTED);return std::nullopt;}
 const auto drive=root.root_name().native();
 const auto type=GetDriveTypeW((drive+L"\\").c_str());
 if(type!=DRIVE_FIXED){SetLastError(ERROR_NOT_SUPPORTED);return std::nullopt;}
 std::array<wchar_t,512> targets{};
 const auto count=QueryDosDeviceW(drive.c_str(),targets.data(),static_cast<DWORD>(targets.size()));
 if(!count||count>=targets.size()){SetLastError(ERROR_NOT_SUPPORTED);return std::nullopt;}
 return ClassifyLocalVolume(root,type,targets.data());
}
std::optional<std::filesystem::path> LocalAppDataCandidate() {
 PWSTR path{};
 if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DONT_VERIFY,nullptr,&path))) return std::nullopt;
 std::filesystem::path result(path); CoTaskMemFree(path); return result;
}
bool DirectoriesValid(const std::vector<NativeHandle>& pins) {
 for(const auto& pin:pins) { BY_HANDLE_FILE_INFORMATION info{};
  if(!GetFileInformationByHandle(pin.get(),&info) || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
    (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
 } return !pins.empty();
}
bool PinDirectories(const std::filesystem::path& root,std::wstring_view physical,unsigned create_tail,
                    std::vector<NativeHandle>& pins,bool writer) {
 if(!pins.empty()||!LocalPathSyntax(root)){SetLastError(ERROR_INVALID_PARAMETER);return false;}
 std::vector<std::wstring> parts;
 for(const auto& part:root.relative_path()) parts.push_back(part.native());
 if(create_tail>parts.size()){SetLastError(ERROR_INVALID_PARAMETER);return false;}
 NativeHandle volume(CreateFileW(std::wstring(physical).c_str(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,
   FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
 if(!volume)return false;
 pins.push_back(std::move(volume));
 for(std::size_t i=0;i<parts.size();++i) {
  const bool last=i+1==parts.size();
  const auto access=FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY|((create_tail&&i+1>=parts.size()-create_tail)?FILE_ADD_SUBDIRECTORY:0U)|
    ((last&&writer)?FILE_ADD_FILE:0U);
  auto child=RelativeOpen(pins.back().get(),parts[i],access,FILE_SHARE_READ|FILE_SHARE_WRITE,
    i>=parts.size()-create_tail?3U:1U,true);
  if(!child)return false;
  pins.push_back(std::move(child));
  if(!DirectoriesValid(pins))return false;
 }
 return DirectoriesValid(pins);
}
bool RegularSingleLink(HANDLE handle,BY_HANDLE_FILE_INFORMATION* output) {
 BY_HANDLE_FILE_INFORMATION info{};
 if(!GetFileInformationByHandle(handle,&info)||info.nNumberOfLinks!=1||
   (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) {SetLastError(ERROR_INVALID_DATA);return false;}
 if(output)*output=info;return true;
}
bool DeleteFileHandle(HANDLE file) {FILE_DISPOSITION_INFO info{TRUE};return SetFileInformationByHandle(file,FileDispositionInfo,&info,sizeof(info))!=FALSE;}
bool WriteBytes(HANDLE file,std::string_view bytes) {
 if(bytes.size()>MAXDWORD){SetLastError(ERROR_INVALID_PARAMETER);return false;}
 DWORD written{};return WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size();
}
bool RenameWithin(HANDLE file,HANDLE directory,std::wstring_view name,bool replace) {
 if(!Component(name)){SetLastError(ERROR_INVALID_PARAMETER);return false;}
 std::vector<std::byte> bytes(sizeof(FILE_RENAME_INFO)+name.size()*sizeof(wchar_t));
 auto* info=reinterpret_cast<FILE_RENAME_INFO*>(bytes.data());
 info->ReplaceIfExists=replace?TRUE:FALSE;info->RootDirectory=directory;
 info->FileNameLength=static_cast<DWORD>(name.size()*sizeof(wchar_t));std::memcpy(info->FileName,name.data(),info->FileNameLength);
 using Rename=NTSTATUS(NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
 using Translate=ULONG(WINAPI*)(NTSTATUS);
 const auto module=GetModuleHandleW(L"ntdll.dll");
 const auto rename=reinterpret_cast<Rename>(GetProcAddress(module,"NtSetInformationFile"));
 const auto translate=reinterpret_cast<Translate>(GetProcAddress(module,"RtlNtStatusToDosError"));
 if(!rename||!translate){SetLastError(ERROR_NOT_SUPPORTED);return false;}
 IO_STATUS_BLOCK status{};const auto result=rename(file,&status,info,static_cast<ULONG>(bytes.size()),static_cast<FILE_INFORMATION_CLASS>(10));
 if(result<0){SetLastError(translate(result));return false;}return true;
}
}
