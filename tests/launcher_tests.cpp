#include "support.hpp"
#include "fixtures/runtime_images.hpp"
#include "fixtures/verified_identity.hpp"
#include "fixtures/package_documents.hpp"
#include "wfg/launcher.hpp"
#include <cstdlib>
#include <windows.h>

namespace {
using namespace wfg;
Error Fault(ErrorCode code=ErrorCode::LaunchFailed,Stage stage=Stage::activation){return {code,stage,"fixture_operation",5U};}
struct Backend final : IDebugBackend {
  std::chrono::steady_clock::time_point now{};
  int attaches{},policies{},continues{},detaches{},aborts{},detach_failures{};
  bool kill{true},held{},fault_observed{},fail_attach{},fail_first_false{},fail_true{},fail_abort{},exited{};
  void* pin{};int waits{},safe_continues_before_fault{},unhandled{};std::vector<DebugEvent> events;
  std::chrono::steady_clock::time_point Now() const override{return now;}
  Result<Unit> Attach(const OwnedProcess& owner) override{++attaches;pin=owner.native_handle();return fail_attach?Result<Unit>{Fault(ErrorCode::AccessDenied,Stage::debug_attach)}:Result<Unit>{Unit{}};}
  Result<Unit> SetKillOnExit(bool enabled) override{
    ++policies;if((policies==1&&fail_first_false)||(enabled&&fail_true))return Fault(ErrorCode::DebugFailure,Stage::debug_attach);
    kill=enabled;return Unit{};
  }
  Result<std::optional<DebugEvent>> Wait(std::chrono::milliseconds slice) override{
    now+=slice;held=true;++waits;
    if(!events.empty()&&static_cast<std::size_t>(waits)<=events.size())return std::optional<DebugEvent>{events[static_cast<std::size_t>(waits-1)]};
    if(waits==1)return std::optional<DebugEvent>{{DebugEventKind::ProcessCreated,42,7}};
    if(waits==2)return std::optional<DebugEvent>{{DebugEventKind::Exception,42,7,0x80000003,0x1234,true,true}};
    return std::optional<DebugEvent>{{DebugEventKind::DllLoaded,42,7}};
  }
  Result<Unit> Continue(const DebugEvent&,Continuation c) override{++continues;if(c==Continuation::NotHandled)++unhandled;CHECK(!kill);held=false;return Unit{};}
  Result<Unit> Detach() override{++detaches;if(detach_failures-->0)return Fault(ErrorCode::DebugFailure,Stage::detach);return Unit{};}
  Result<Unit> AbortOwnedStartup() override{++aborts;if(fail_abort)return Fault(ErrorCode::DebugFailure,Stage::debug_attach);exited=true;return Unit{};}
};
struct Storage final : ILogStorage {
  int writes{};bool fail{},throw_summary{};std::string summary,console,events;
  int* active_guard{};int guard_at_final{},final_writes{};std::string final_prefix;
  std::stop_source* external_cancel{};std::stop_token cooperative;bool cancellation_forwarded{};
  Result<Unit> AppendEvent(std::string_view text,std::uint32_t,std::uint32_t) override{++writes;events+=text;return fail?Result<Unit>{Fault(ErrorCode::DiagnosticFailure)}:Result<Unit>{Unit{}};}
  Result<Unit> WriteSummaryAtomically(std::string_view text) override{if(throw_summary)throw 1;++writes;summary=text;return fail?Result<Unit>{Fault(ErrorCode::DiagnosticFailure)}:Result<Unit>{Unit{}};}
  void WriteConsoleLine(std::string_view text) override{
    console+=text;
    if(!final_prefix.empty()&&text.starts_with(final_prefix)) {
      ++final_writes;guard_at_final=*active_guard;
      if(external_cancel){external_cancel->request_stop();cancellation_forwarded=cooperative.stop_requested();}
    }
  }
};
struct Memory final : IWritableMemory {
  SupportedRuntimeFixture& runtime;Backend& backend;
  mutable int reads{};int writes{},protects{};bool fail_write{},fail_once{},bad_capture{};
  std::array<std::pair<Address,std::uint32_t>,4> pages{};
  Memory(SupportedRuntimeFixture& r,Backend& b):runtime(r),backend(b){}
  Result<Bytes> Read(Address address,std::size_t count) const override{return runtime.memory.Read(address,count);}
  Result<MemoryRegion> Query(Address address) const override{
    auto found=runtime.memory.Query(address);if(!IsOk(found))return Failure(found);
    auto region=Value(found);region.protection=bad_capture?0U:2U;
    for(const auto& page:pages)if(page.first&&(address&~Address{4095})==page.first)region.protection=page.second;
    region.writable=region.protection==4;return region;
  }
  Result<Unit> ReadInto(Address address,std::span<std::byte> bytes) const override{
    ++reads;const auto value=Read(address,bytes.size());if(!IsOk(value))return Failure(value);
    std::memcpy(bytes.data(),Value(value).data(),bytes.size());return Unit{};
  }
  Result<Unit> Write(Address address,std::span<const std::byte> bytes) override{
    ++writes;CHECK(backend.kill&&backend.held);
    if(fail_write||(fail_once&&writes==1)){if(!backend.fault_observed){backend.safe_continues_before_fault=backend.continues;backend.fault_observed=true;}return Fault(ErrorCode::MutationFailure,Stage::import_transaction);}
    runtime.memory.Put(address,bytes);return Unit{};
  }
  Result<std::uint32_t> Protect(Address address,std::size_t,std::uint32_t protection) override{
    ++protects;CHECK(backend.kill&&backend.held);
    for(auto& page:pages)if(page.first==address||!page.first){const auto prior=page.first?page.second:2U;page={address,protection};return prior;}
    return Fault(ErrorCode::MutationFailure);
  }
};
struct MemoryView final : IWritableMemory {
  Memory& m;explicit MemoryView(Memory& value):m(value){}
  Result<Bytes> Read(Address a,std::size_t s)const override{return m.Read(a,s);}
  Result<MemoryRegion> Query(Address a)const override{return m.Query(a);}
  Result<Unit> ReadInto(Address a,std::span<std::byte>b)const override{return m.ReadInto(a,b);}
  Result<Unit> Write(Address a,std::span<const std::byte>b)override{return m.Write(a,b);}
  Result<std::uint32_t> Protect(Address a,std::size_t s,std::uint32_t p)override{return m.Protect(a,s,p);}
};
struct Guard final:IRunGuard {
  int& active;int id;
  Guard(int& value,int identity):active(value),id(identity){active=id;}
  ~Guard()override{Disarm();}
  void Disarm()override{if(active==id)active=0;}
};
struct Fixture final : ILaunchEnvironment {
  SupportedRuntimeFixture runtime;Backend backend;::Memory memory{runtime,backend};Storage storage;
  CompatibilityRecord compatibility=runtime.SyntheticBuild();
  EffectiveConfig config=DefaultConfiguration();std::vector<std::string_view> args;
  std::optional<std::string> configured_path;
  std::function<Result<PackageFacts>()> discovery;
  std::optional<Error> config_error,discover_error,inventory_error,collect_error,observe_error;
  EnvironmentSnapshot host;
  int configurations{},discoveries{},installed{},activated{},pins{},memory_opens{},inventories{},observations{},collections{};
  int handle{};void* memory_pin{};bool expire_inventory{},cancel_observation{};int missing_inventories{};int unresolved_inventories{};bool binding_sequence{};bool cancel_inventory{},deadline_child{};std::chrono::milliseconds inventory_elapsed{};int first_inventory_wait{};
  std::vector<Deadline> guards;std::stop_source* active_cancel{};int active_guard{};
  Fixture(){storage.active_guard=&active_guard;}
  std::span<const std::string_view> Arguments()const override{return args;}
  std::chrono::steady_clock::time_point Now()const override{return backend.now;}
  Result<EffectiveConfig> Configuration(const CommandLine& options)override{
    configured_path=options.game_path;++configurations;if(config_error)return *config_error;auto effective=config;
    if(options.log_level){effective.log_level=*options.log_level;effective.origins["logging.level"]=ConfigOrigin::CommandLine;}return effective;
  }
  ILogStorage& Diagnostics()override{return storage;}
  std::unique_ptr<IRunGuard> Guard(Deadline deadline,std::stop_source& cancel,std::stop_token)override{guards.push_back(deadline);active_cancel=&cancel;storage.cooperative=cancel.get_token();return std::make_unique<::Guard>(active_guard,static_cast<int>(guards.size()));}
  Result<PackageFacts> Discover()override{++discoveries;if(discovery)return discovery();if(discover_error)return *discover_error;return PackageFacts{compatibility.package_name,compatibility.package_version,compatibility.machine};}
  EnvironmentSnapshot Environment()override{return host;}
  Result<Unit> InstallObserver(Deadline)override{++installed;CHECK(activated==0);return Unit{};}
  Result<Unit> Activate()override{++activated;CHECK(installed==1);return Unit{};}
  Result<OwnedProcess> PinFresh()override{++pins;CHECK(activated==1);return VerifiedIdentityTestFactory::Owned(&handle,[](void*)noexcept{});}
  Result<std::unique_ptr<IWritableMemory>> Memory(const OwnedProcess& owner)override{++memory_opens;memory_pin=owner.native_handle();CHECK(memory_pin==&handle);return std::unique_ptr<IWritableMemory>(new MemoryView(memory));}
  IDebugBackend& Debugger()override{return backend;}
  Result<RuntimeInventory> Inventory(const OwnedProcess& owner,const IMemory&)override{
    ++inventories;if(inventories==1)first_inventory_wait=backend.waits;backend.now+=inventory_elapsed;if(cancel_inventory)active_cancel->request_stop();CHECK(backend.held);CHECK(owner.native_handle()==memory_pin&&memory_pin==backend.pin);
    if(binding_sequence){if(unresolved_inventories-->0){CHECK(memory.writes==0&&memory.protects==0);runtime.SetAllImportsUnresolved();}else runtime.SetAllImportsToNative();}
    if(expire_inventory)backend.now+=std::chrono::seconds(61);
    if(missing_inventories-->0)return Error{ErrorCode::ModuleNotReady,Stage::dependencies,"startup_modules_missing",5U};
    if(inventory_error)return *inventory_error;return runtime.inventory;
  }
  const CompatibilityRecord& Compatibility()const override{return compatibility;}
  Result<ObservationReport> Observe(const OwnedProcess* owner,const EffectiveConfig&,std::stop_token)override{
    ++observations;if(observe_error)return *observe_error;if(owner){CHECK(backend.detaches>0);CHECK(owner->native_handle()==memory_pin);}
    if(cancel_observation)active_cancel->request_stop();ObservationReport report;report.cancelled=cancel_observation;return report;
  }
  Result<Unit> Collect(const EffectiveConfig&)override{++collections;backend.now+=std::chrono::milliseconds(750);if(collect_error)return *collect_error;return Unit{};}
  [[noreturn]] void ExitHelper()noexcept override{if(deadline_child)std::_Exit(77);std::_Exit(backend.kill&&backend.continues==backend.safe_continues_before_fault&&backend.detaches==0&&storage.writes==0?91:92);}
};
void Refusal(Fixture& f,Status status){const auto result=RunLauncher(f,{});if(result.status!=status)std::fprintf(stderr,"Status expected=%d actual=%d operation=%s\n",static_cast<int>(status),static_cast<int>(result.status),result.failure?std::string(result.failure->operation.view()).c_str():"none");CHECK(result.status==status);CHECK(result.failure);CHECK(!result.next_action.empty());CHECK(!result.diagnostic.operation.empty());CHECK(f.memory.writes==0);}
}
TEST("launcher.late_factory_is_unmodified_refusal"){
  Fixture f;f.runtime.SetFactory(f.runtime.native_factory);Refusal(f,Status::StartupTooLate);CHECK(f.backend.detaches>0);
}
TEST("launcher.applied_does_not_claim_gameplay_verified"){
  Fixture f;const auto result=RunLauncher(f,{});CHECK(result.status==Status::Applied);CHECK(f.memory.writes==4);CHECK(f.pins==1&&f.memory_opens==1);
  CHECK(result.transaction==TransactionOutcome::Committed);CHECK(result.detach==DetachOutcome::Detached);CHECK(!f.backend.kill);
  CHECK(FormatStatus(result).find(L"FG verified")==std::wstring::npos);CHECK(f.storage.summary.find("C:\\Users\\")==std::string::npos);
  CHECK(f.storage.summary.find("\"configuration\"")!=std::string::npos);CHECK(f.storage.summary.find("\"build\"")!=std::string::npos);CHECK(f.observations==0);
}
TEST("launcher.all_present_and_mixed_imports_never_write"){
  Fixture present;present.runtime.SetAllImportsToInterposer();CHECK(RunLauncher(present,{}).status==Status::RoutingAlreadyPresent);CHECK(present.memory.writes==0);
  Fixture mixed;mixed.runtime.SetImportPointer("CreateDXGIFactory",mixed.runtime.InterposerExport("CreateDXGIFactory"));Refusal(mixed,Status::UnexpectedImports);
}
TEST("launcher.only_module_not_ready_retries_and_deadline_checks_after_inventory"){
  Fixture late;late.missing_inventories=2;CHECK(RunLauncher(late,{}).status==Status::Applied);CHECK(late.inventories==3);CHECK(late.memory.writes==4);
  Fixture bad;bad.inventory_error=Fault(ErrorCode::UntrustedInterposer,Stage::dependencies);Refusal(bad,Status::UntrustedInterposer);CHECK(bad.inventories==1);
  Fixture slow;slow.expire_inventory=true;Refusal(slow,Status::TimedOut);CHECK(slow.backend.detaches>0);
}
TEST("launcher.configuration_and_cli_refuse_before_activation"){
  Fixture invalid;invalid.config_error=Fault(ErrorCode::ConfigurationInvalid,Stage::configuration);Refusal(invalid,Status::ConfigurationInvalid);CHECK(invalid.discoveries==0&&invalid.activated==0&&invalid.backend.attaches==0);
  Fixture cli;cli.args={"--unknown"};Refusal(cli,Status::ConfigurationInvalid);CHECK(cli.configurations==0&&cli.activated==0);
}
TEST("launcher.readonly_modes_have_zero_activation_attach_and_memory"){
  for(auto mode:{"--help","--version","--diagnose","--collect-support"}){
    Fixture f;f.args={mode};CHECK(RunLauncher(f,{}).status==Status::ReadOnlyCompleted);CHECK(f.activated==0&&f.pins==0&&f.backend.attaches==0&&f.memory_opens==0&&f.memory.writes==0);
    if(std::string_view(mode)=="--help"||std::string_view(mode)=="--version")CHECK(f.configurations==0&&f.discoveries==0);
  }
}
TEST("launcher.version_reports_complete_embedded_identity_without_activation"){
  Fixture f;f.args={"--version"};CHECK(RunLauncherToConsole(f,{})==0);
  const auto& identity=EmbeddedBuildIdentity();
  CHECK(f.storage.console.find("channel="+std::string(identity.channel))!=std::string::npos);
  CHECK(f.storage.console.find(identity.dirty?"dirty=true":"dirty=false")!=std::string::npos);
  CHECK(f.storage.console.find(identity.archive?"archive=true":"archive=false")!=std::string::npos);
  CHECK(f.storage.console.find("diagnostic_schema="+std::to_string(identity.diagnostic_schema))!=std::string::npos);
  CHECK(f.storage.console.find("commit="+std::string(identity.commit))!=std::string::npos);
  CHECK(f.storage.console.find("input="+std::string(identity.input_digest))!=std::string::npos);
  CHECK(f.activated==0&&f.configurations==0&&f.backend.attaches==0&&f.memory_opens==0);
}
TEST("launcher.registration_failure_and_collector_failure_are_explicit"){
  Fixture absent;absent.discover_error=Fault(ErrorCode::NoMatchingProcess,Stage::package_discovery);Refusal(absent,Status::LaunchFailed);CHECK(absent.activated==0);
  Fixture collect;collect.args={"--collect-support"};collect.collect_error=Fault(ErrorCode::SupportBundleFailure);Refusal(collect,Status::SupportFailed);CHECK(collect.activated==0);
}
TEST("launcher.not_started_error_and_verified_rollback_are_not_success"){
  Fixture capture;capture.memory.bad_capture=true;const auto refused=RunLauncher(capture,{});CHECK(refused.status==Status::LaunchFailed);CHECK(refused.transaction==TransactionOutcome::NotStarted);CHECK(refused.failure);CHECK(capture.memory.writes==0);
  Fixture rollback;rollback.memory.fail_once=true;const auto reverted=RunLauncher(rollback,{});CHECK(reverted.status==Status::RolledBack);CHECK(reverted.transaction==TransactionOutcome::RolledBack);CHECK(rollback.backend.detaches>0&&!rollback.backend.kill);
  Fixture aborted;aborted.memory.fail_write=true;const auto stopped=RunLauncher(aborted,{});CHECK(stopped.status==Status::StartupAborted);CHECK(stopped.transaction==TransactionOutcome::Aborted);CHECK(aborted.backend.exited);CHECK(aborted.backend.continues==aborted.backend.safe_continues_before_fault);
}
TEST("launcher.detach_retries_without_repatch_and_log_failure_preserves_commit"){
  Fixture f;f.backend.detach_failures=2;f.storage.fail=true;const auto result=RunLauncher(f,{});CHECK(result.status==Status::Applied);CHECK(result.diagnostic_file_failed);CHECK(f.backend.detaches==3);CHECK(f.memory.writes==4);CHECK(f.storage.console.find("\"configuration\"")!=std::string::npos);
}
TEST("launcher.optional_observation_is_separately_bounded_after_detach"){
  Fixture f;f.config.runtime_observation=true;f.cancel_observation=true;const auto result=RunLauncher(f,{});CHECK(result.status==Status::Applied);CHECK(f.observations==1&&f.backend.aborts==0);CHECK(f.guards.size()==2);CHECK(f.guards[1].value>f.backend.now);
}
TEST("launcher.refusal_statuses_and_exit_codes_are_complete"){
  for(auto status:{Status::Applied,Status::RoutingAlreadyPresent,Status::ReadOnlyCompleted})CHECK(ExitCode(status)==0);
  for(auto status:{Status::AlreadyRunning,Status::UnsupportedBuild,Status::UnexpectedImports,Status::UntrustedInterposer,Status::StartupTooLate,Status::AccessDenied,Status::ConfigurationInvalid,Status::DllConflict})CHECK(ExitCode(status)==2);
  for(auto status:{Status::RolledBack,Status::LaunchFailed,Status::TimedOut,Status::SupportFailed})CHECK(ExitCode(status)==3);
  CHECK(ExitCode(Status::StartupAborted)==4);
  Fixture unsupported;unsupported.runtime.Mutate(Mutation::Timestamp);Refusal(unsupported,Status::UnsupportedBuild);
  Fixture denied;denied.backend.fail_attach=true;Refusal(denied,Status::AccessDenied);
  Fixture conflict;conflict.runtime.AddApplicationLocalLoader("dxgi.dll","1.0",false);Refusal(conflict,Status::DllConflict);
  Fixture running;running.discover_error=Fault(ErrorCode::AlreadyRunning);Refusal(running,Status::AlreadyRunning);
}

TEST("launcher.diagnostic_exception_after_commit_preserves_applied") {
  Fixture fixture;fixture.storage.throw_summary=true;
  const auto result=RunLauncher(fixture,{});CHECK(result.status==Status::Applied);CHECK(result.diagnostic_file_failed);
  CHECK(fixture.memory.writes==4);CHECK(fixture.backend.detaches==1);CHECK(fixture.backend.aborts==0);CHECK(fixture.storage.console.find("\"configuration\"")!=std::string::npos);
}
TEST("launcher.fatal_child") {
  char mode[32]{};if(!GetEnvironmentVariableA("WFG_LAUNCHER_FATAL_CHILD",mode,sizeof(mode)))return;
  Fixture fixture;fixture.backend.fail_abort=true;
  if(std::string_view(mode)=="attach")fixture.backend.fail_first_false=true;
  else fixture.memory.fail_write=true;
  static_cast<void>(RunLauncher(fixture,{}));std::_Exit(93);
}
TEST("launcher.unsafe_terminal_results_die_before_logging_continue_or_detach") {
  wchar_t executable[32768]{};CHECK(GetModuleFileNameW(nullptr,executable,32768)>0);
  for(const auto* mode:{L"attach",L"transaction"}) {
    CHECK(SetEnvironmentVariableW(L"WFG_LAUNCHER_FATAL_CHILD",mode));
    std::wstring command=L"\""+std::wstring(executable)+L"\" --filter launcher.fatal_child";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    const bool created=CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;
    SetEnvironmentVariableW(L"WFG_LAUNCHER_FATAL_CHILD",nullptr);CHECK(created);
    const auto waited=WaitForSingleObject(process.hProcess,10000);DWORD code{};
    if(waited!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,94);
    CHECK(GetExitCodeProcess(process.hProcess,&code));CloseHandle(process.hThread);CloseHandle(process.hProcess);
    CHECK(waited==WAIT_OBJECT_0);CHECK(code==91);
  }
}

TEST("launcher.missing_diagnostic_target_keeps_explicit_unavailable_fields") {
  Fixture fixture;fixture.args={"--diagnose"};fixture.observe_error=Fault(ErrorCode::NoMatchingProcess,Stage::runtime_observation);
  const auto result=RunLauncher(fixture,{});CHECK(result.status==Status::ReadOnlyCompleted);
  CHECK(fixture.storage.summary.find("\"requested_mode\":{\"availability\":\"not_observed\"")!=std::string::npos);
  CHECK(fixture.storage.summary.find("target_unavailable")!=std::string::npos);
  CHECK(fixture.activated==0&&fixture.backend.attaches==0);
}
TEST("launcher.summary_escapes_observations_and_redacts_private_paths") {
  Fixture fixture;fixture.host.gpu={std::string("NVIDIA \"fixture\""),Availability::Observed,"dxgi_observation",""};
  fixture.host.selected_adapter={std::string("C:\\Users\\PRIVATE_SECRET"),Availability::Observed,"runtime_observation",""};
  CHECK(RunLauncher(fixture,{}).status==Status::Applied);
  CHECK(fixture.storage.summary.find("NVIDIA \\\"fixture\\\"")!=std::string::npos);
  CHECK(fixture.storage.summary.find("PRIVATE_SECRET")==std::string::npos);
}
TEST("launcher.already_cancelled_request_never_activates") {
  Fixture fixture;std::stop_source cancel;cancel.request_stop();
  CHECK(RunLauncher(fixture,cancel.get_token()).status==Status::TimedOut);
  CHECK(fixture.activated==0&&fixture.backend.attaches==0&&fixture.memory.writes==0);
}

TEST("launcher.native_final_status_route_retains_guard_and_cancellation") {
  for(bool observation:{false,true}) {
    Fixture fixture;fixture.config.runtime_observation=observation;
    fixture.storage.final_prefix="Import routing applied";std::stop_source cancel;fixture.storage.external_cancel=&cancel;
    CHECK(RunLauncherToConsole(fixture,cancel.get_token())==0);
    CHECK(fixture.storage.final_writes==1);CHECK(fixture.storage.guard_at_final==(observation?2:1));
    CHECK(fixture.storage.cancellation_forwarded);CHECK(fixture.active_guard==0);
    CHECK(fixture.guards.size()==(observation?2U:1U));CHECK(fixture.guards.front().value==std::chrono::steady_clock::time_point{}+std::chrono::seconds(60));
    CHECK(fixture.backend.detaches==1&&fixture.backend.aborts==0);
  }
}
TEST("launcher.native_readonly_and_preconfig_status_routes_are_guarded") {
  for(auto mode:{"--help","--version","--collect-support","--invalid"}) {
    Fixture fixture;fixture.args={mode};const bool invalid=std::string_view(mode)=="--invalid";
    fixture.storage.final_prefix=invalid?"The launcher configuration":"The read-only operation completed";
    CHECK(RunLauncherToConsole(fixture,{})==(invalid?2:0));CHECK(fixture.storage.final_writes==1);
    CHECK(fixture.storage.guard_at_final==1);CHECK(fixture.active_guard==0);CHECK(fixture.guards.size()==1);
    CHECK(fixture.activated==0&&fixture.backend.attaches==0);
  }
}
namespace {
void CompletionUtc(std::string_view text) {
  constexpr std::string_view prefix="\"completed_utc\":\"";
  const auto position=text.find(prefix);CHECK(position!=std::string_view::npos);
  const auto utc=text.substr(position+prefix.size(),20);
  CHECK(utc.size()==20);CHECK(utc[4]=='-'&&utc[7]=='-'&&utc[10]=='T'&&utc[13]==':'&&utc[16]==':'&&utc[19]=='Z');
  for(std::size_t i=0;i<utc.size();++i)if(i!=4&&i!=7&&i!=10&&i!=13&&i!=16&&i!=19)CHECK(utc[i]>='0'&&utc[i]<='9');
}
}
TEST("launcher.success_error_summary_and_console_fallback_keep_completion_time") {
  for(bool fail:{false,true})for(bool present:{false,true}) {
    Fixture fixture;fixture.storage.fail=fail;if(present)fixture.runtime.SetAllImportsToInterposer();
    CHECK(RunLauncherToConsole(fixture,{})==0);
    CompletionUtc(fail?fixture.storage.console:fixture.storage.summary);
    CHECK(fixture.storage.summary.find("\"failure\":null")!=std::string::npos);CHECK(fixture.storage.events.empty());
  }
}
TEST("launcher.collect_completion_uses_actual_elapsed_time") {
  Fixture fixture;fixture.args={"--collect-support"};CHECK(RunLauncherToConsole(fixture,{})==0);
  CompletionUtc(fixture.storage.console);CHECK(fixture.storage.console.find("\"elapsed_ms\":750")!=std::string::npos);
  CHECK(fixture.storage.summary.empty());
}

TEST("launcher.binding_progresses_only_after_resolved_next_stop") {
  Fixture f;f.binding_sequence=true;f.unresolved_inventories=1;
  const auto result=RunLauncher(f,{});
  CHECK(result.status==Status::Applied);CHECK(f.inventories==2);CHECK(f.memory.writes==4);CHECK(result.detach==DetachOutcome::Detached);
}
TEST("launcher.presence_gate_retries_do_not_consume_the_qualification_budget") {
  // CompleteStartupInventory refuses startup_modules_missing BEFORE inspect() runs, so those
  // passes cost a module-path scan and the two path-only conflict guards -- no Authenticode, no
  // catalog enumeration, no backing-image compare. They are not what the qualification budget
  // exists to bound, and charging them broke the real target: the live run refused
  // dependencies_qualification_budget after 12.6 s with the interposer never observed, while the
  // successful live runs had taken 8.8-55.9 s. The game simply needs more than twelve debug
  // events before Streamline is loaded. Only passes that actually qualified may be charged.
  Fixture few; few.missing_inventories=20;
  const auto applied=RunLauncher(few,{});
  CHECK(applied.status==Status::Applied);
  CHECK(few.inventories==21);
  CHECK(few.memory.writes==4);

  Fixture many; many.missing_inventories=2000;
  const auto deadline=RunLauncher(many,{});
  CHECK(deadline.status==Status::TimedOut);
  CHECK(many.memory.writes==0&&many.memory.protects==0);
  // It must run out of clock, never out of a pass budget. A cap was tried and reverted: it
  // refused at 12.6 s counting every pass, and still refused at 30.8 s counting only qualified
  // ones, against real startups measured at 8.8-55.9 s.
  CHECK(many.storage.summary.find("dependencies_qualification_budget")==std::string::npos);
  Fixture binding; binding.binding_sequence=true; binding.unresolved_inventories=2000;
  const auto bound=RunLauncher(binding,{});
  CHECK(bound.status==Status::TimedOut);
  CHECK(binding.storage.summary.find("dependencies_qualification_budget")==std::string::npos);
}
TEST("launcher.binding_unresolved_is_bounded_and_never_writes") {
  // "Bounded" previously meant only "the 60 s deadline eventually fires". Each pass re-runs the
  // whole qualification from scratch -- Authenticode plus catalog enumeration and a backing-image
  // byte-compare for every loaded system module -- which measures at 6.9 ms per module warm and
  // 17.8 ms cold, so 50-80 modules cost roughly 0.5-1.5 s per pass. Thousands of passes cannot
  // fit, and spending the entire budget re-verifying identical modules is what produced an
  // opaque TimedOut instead of a prompt diagnosable refusal. Assert a real upper bound.
  Fixture f;f.binding_sequence=true;f.unresolved_inventories=2000;
  const auto result=RunLauncher(f,{});
  CHECK(result.status==Status::TimedOut);CHECK(f.inventories>1);
  CHECK(f.memory.writes==0&&f.memory.protects==0);CHECK(result.detach==DetachOutcome::Detached);CHECK(!f.storage.summary.empty());
}

TEST("launcher.startup_handshake_skips_replayed_events") {
  Fixture f;f.backend.events={
    {DebugEventKind::ProcessCreated,42,7},{DebugEventKind::DllLoaded,42,7},{DebugEventKind::ThreadCreated,42,8},
    {DebugEventKind::Exception,42,8,0x80000003,0x1234,true,true}};
  const auto result=RunLauncher(f,{});CHECK(result.status==Status::Applied);CHECK(f.first_inventory_wait==4);CHECK(f.inventories==1);CHECK(f.memory.writes==4);
}
TEST("launcher.deadline_cancellation_child") {
  char mode[32]{};if(!GetEnvironmentVariableA("WFG_DEADLINE_CHILD",mode,sizeof(mode)))return;
  Fixture f;f.cancel_inventory=true;f.deadline_child=true;
  const bool early=std::string_view(mode)=="early",exhausted=std::string_view(mode)=="exhausted";
  f.inventory_elapsed=std::chrono::milliseconds(early?100:exhausted?63000:61000);if(early)f.backend.detach_failures=2;
  const auto result=RunLauncher(f,{});CHECK(!exhausted);CHECK(result.status==Status::TimedOut);CHECK(result.detach==DetachOutcome::Detached);CHECK(!f.storage.summary.empty());CHECK(f.memory.writes==0&&f.memory.protects==0);CHECK(f.backend.detaches==(early?3:1));CHECK(f.backend.now.time_since_epoch()<=std::chrono::milliseconds(early?2200:62000));
}
TEST("launcher.deadline_cancellation_keeps_bounded_cleanup") {
  wchar_t executable[32768]{};CHECK(GetModuleFileNameW(nullptr,executable,32768)>0);
  for(const auto* mode:{L"early",L"late",L"exhausted"}) {
    CHECK(SetEnvironmentVariableW(L"WFG_DEADLINE_CHILD",mode));std::wstring command=L"\""+std::wstring(executable)+L"\" --filter launcher.deadline_cancellation_child";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    const bool created=CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;
    SetEnvironmentVariableW(L"WFG_DEADLINE_CHILD",nullptr);CHECK(created);const auto waited=WaitForSingleObject(process.hProcess,10000);DWORD code{};if(waited!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,94);CHECK(GetExitCodeProcess(process.hProcess,&code));CloseHandle(process.hThread);CloseHandle(process.hProcess);CHECK(waited==WAIT_OBJECT_0);CHECK(code==(std::wstring_view(mode)==L"exhausted"?77U:0U));
  }
}

TEST("launcher.startup_handshake_unrelated_exception_is_not_swallowed") {
  Fixture f;f.backend.events={{DebugEventKind::ProcessCreated,42,7},{DebugEventKind::Exception,42,8,0xc0000005,0x5555,true,false}};
  const auto result=RunLauncher(f,{});CHECK(result.status==Status::TimedOut);CHECK(f.inventories==0);CHECK(f.backend.unhandled==1);CHECK(f.memory.writes==0&&f.memory.protects==0);
}
TEST("launcher.startup_handshake_target_exit_never_inspects") {
  Fixture f;f.backend.events={{DebugEventKind::ProcessCreated,42,7},{DebugEventKind::ProcessExited,42,7}};
  const auto result=RunLauncher(f,{});CHECK(result.failure);CHECK(result.failure->code==ErrorCode::NoMatchingProcess);CHECK(f.inventories==0);CHECK(f.memory.writes==0&&f.memory.protects==0);
}

TEST("launcher.manual_path_refusal_never_activates_or_opens_memory_and_keeps_paths_private") {
  Fixture f;
  f.args={"--game-path","C:\\Users\\PrivatePlayer\\Game\\witcher3.exe"};
  f.discover_error=Error{ErrorCode::UnsupportedBuild,Stage::package_discovery,"manual_game_path_unrelated"};
  const auto result=RunLauncher(f,{});
  CHECK(f.discoveries==1); CHECK(f.configured_path == f.args[1]);
  CHECK(f.activated==0&&f.installed==0&&f.pins==0&&f.memory_opens==0&&f.backend.attaches==0&&f.memory.writes==0);
  CHECK(result.failure.has_value());
  CHECK(f.storage.summary.find("PrivatePlayer")==std::string::npos);
  CHECK(f.storage.console.find("PrivatePlayer")==std::string::npos);
}
TEST("launcher.manual_path_diagnose_remains_read_only") {
  Fixture f; f.args={"--diagnose","--game-path","C:\\Game"};
  const auto result=RunLauncher(f,{});
  CHECK(result.status==Status::ReadOnlyCompleted); CHECK(f.discoveries==1&&f.observations==1); CHECK(f.configured_path == f.args[2]);
  CHECK(f.activated==0&&f.installed==0&&f.pins==0&&f.memory_opens==0&&f.backend.attaches==0&&f.memory.writes==0);
}
TEST("launcher.manual_path_requires_retry_syntax_without_activation") {
  Fixture f; f.discover_error=Error{ErrorCode::LaunchFailed,Stage::package_discovery,"manual_game_path_required"};
  const auto result=RunLauncher(f,{});
  CHECK(result.next_action.find("WitcherFG.exe --game-path \"<folder-or-witcher3.exe>\"")!=std::string::npos);
  CHECK(f.activated==0&&f.backend.attaches==0&&f.memory.writes==0);
}

TEST("launcher.manual_path_does_not_bypass_factory_import_or_trust_guards") {
  Fixture factory;factory.args={"--game-path","C:\\Game"};factory.runtime.SetFactory(factory.runtime.native_factory);Refusal(factory,Status::StartupTooLate);
  Fixture mixed;mixed.args={"--game-path","C:\\Game"};mixed.runtime.SetImportPointer("CreateDXGIFactory",mixed.runtime.InterposerExport("CreateDXGIFactory"));Refusal(mixed,Status::UnexpectedImports);
  Fixture trust;trust.args={"--game-path","C:\\Game"};trust.inventory_error=Fault(ErrorCode::UntrustedInterposer,Stage::dependencies);Refusal(trust,Status::UntrustedInterposer);
}

TEST("launcher.native_manual_console_pipe_refuses_without_waiting") {
  HANDLE read{},write{};CHECK(CreatePipe(&read,&write,nullptr,0));
  const auto original=GetStdHandle(STD_INPUT_HANDLE);CHECK(SetStdHandle(STD_INPUT_HANDLE,read));
  const auto start=std::chrono::steady_clock::now();
  const auto response=launcher_detail::ReadManualPath({}, {start+std::chrono::seconds(2)});
  const auto elapsed=std::chrono::steady_clock::now()-start;
  CHECK(SetStdHandle(STD_INPUT_HANDLE,original));CloseHandle(read);CloseHandle(write);
  CHECK(!IsOk(response));CHECK(Failure(response).operation.view()=="manual_game_path_required");
  CHECK(elapsed<std::chrono::seconds(1));
}
TEST("launcher.native_manual_console_child") {
  std::array<wchar_t,2> child{};
  if(GetEnvironmentVariableW(L"WFG_MANUAL_CONSOLE_CHILD",child.data(),static_cast<DWORD>(child.size()))!=1||child[0]!=L'1')return;
  const auto input=GetStdHandle(STD_INPUT_HANDLE);DWORD mode{};CHECK(GetConsoleMode(input,&mode));
  const auto send=[&](WORD key,wchar_t character){
    CHECK(FlushConsoleInputBuffer(input));INPUT_RECORD event{};event.EventType=KEY_EVENT;
    event.Event.KeyEvent.bKeyDown=TRUE;event.Event.KeyEvent.wRepeatCount=1;
    event.Event.KeyEvent.wVirtualKeyCode=key;event.Event.KeyEvent.uChar.UnicodeChar=character;
    DWORD written{};CHECK(WriteConsoleInputW(input,&event,1,&written));CHECK(written==1);
  };
  send(VK_RETURN,L'\r');
  const auto empty=launcher_detail::ReadManualPath({}, {std::chrono::steady_clock::now()+std::chrono::seconds(2)});
  CHECK(!IsOk(empty));CHECK(Failure(empty).operation.view()=="manual_game_path_cancelled");
  send(VK_ESCAPE,L'\0');
  const auto escape=launcher_detail::ReadManualPath({}, {std::chrono::steady_clock::now()+std::chrono::seconds(2)});
  CHECK(!IsOk(escape));CHECK(Failure(escape).operation.view()=="manual_game_path_cancelled");
  std::stop_source cancelled;cancelled.request_stop();
  const auto cancel=launcher_detail::ReadManualPath(cancelled.get_token(),{std::chrono::steady_clock::now()+std::chrono::seconds(2)});
  CHECK(!IsOk(cancel));CHECK(Failure(cancel).code==ErrorCode::TimedOut);
  const auto expired=launcher_detail::ReadManualPath({}, {std::chrono::steady_clock::now()-std::chrono::seconds(1)});
  CHECK(!IsOk(expired));CHECK(Failure(expired).code==ErrorCode::TimedOut);
}
TEST("launcher.native_manual_console_owned_child_cancels_and_honors_deadline") {
  std::array<wchar_t,32768> exe{};const auto length=GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()));CHECK(length>0&&length<exe.size());
  std::wstring command=L"\""+std::wstring(exe.data(),length)+L"\" --filter launcher.native_manual_console_child";
  STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
  PROCESS_INFORMATION process{};CHECK(SetEnvironmentVariableW(L"WFG_MANUAL_CONSOLE_CHILD",L"1"));
  const bool created=CreateProcessW(exe.data(),command.data(),nullptr,nullptr,FALSE,CREATE_NEW_CONSOLE,nullptr,nullptr,&startup,&process)!=FALSE;
  CHECK(SetEnvironmentVariableW(L"WFG_MANUAL_CONSOLE_CHILD",nullptr));CHECK(created);
  const auto wait=WaitForSingleObject(process.hProcess,10000);
  DWORD code{};
  if(wait!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,1000);}
  CHECK(GetExitCodeProcess(process.hProcess,&code));CloseHandle(process.hThread);CloseHandle(process.hProcess);
  CHECK(wait==WAIT_OBJECT_0);CHECK(code==0);
}

TEST("launcher.real_missing_package_assets_refuse_without_activation_or_retry_prompt") {
  for(const auto asset : {L"AppxManifest.xml",L"MicrosoftGame.Config",L"bin\\Gaming.Desktop.x64\\witcher3.exe"}) {
    test_fixture::PackageDocuments documents;REQUIRE_OK(documents.Resolve());
    std::filesystem::create_directories(documents.root/L"bin"/L"Gaming.Desktop.x64");
    {std::ofstream file(documents.root/L"bin"/L"Gaming.Desktop.x64"/L"witcher3.exe");file << "fixture";}
    CHECK(std::filesystem::remove(documents.root/asset));
    Fixture f;
    f.discovery=[&]() -> Result<PackageFacts> {
      const auto resolved=process_detail::ResolveRegisteredExecutable(documents.root.wstring(),documents.identity);
      if(!IsOk(resolved))return Failure(resolved);
      return PackageFacts{f.compatibility.package_name,f.compatibility.package_version,f.compatibility.machine};
    };
    const auto outcome=RunLauncher(f,{});
    CHECK(outcome.failure.has_value());CHECK(!process_detail::CanRequestManualGamePath(*outcome.failure));
    CHECK(outcome.next_action.find("--game-path")==std::string::npos);
    CHECK(f.discoveries==1&&f.installed==0&&f.activated==0&&f.pins==0&&f.memory_opens==0&&f.backend.attaches==0&&f.memory.writes==0);
  }
}
