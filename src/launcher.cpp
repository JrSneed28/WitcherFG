#include "wfg/launcher.hpp"
#include "wfg/support_storage_windows.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#define NOMINMAX
#include <windows.h>

namespace wfg {
namespace {
std::string_view StageText(Stage stage) {
  constexpr std::array names{"configuration","package_discovery","activation","process_identity","debug_attach","dependencies",
    "build_validation","pre_factory_guard","import_transaction","detach","runtime_observation"};
  const auto index=static_cast<std::size_t>(stage);return index<names.size()?names[index]:"configuration";
}
std::string_view StatusText(Status status) {
  switch(status) {
    case Status::Applied:return "Import routing applied for this session; gameplay frame generation remains unverified.";
    case Status::RoutingAlreadyPresent:return "Import routing was already present; gameplay frame generation remains unverified.";
    case Status::AlreadyRunning:return "The game is already running.";
    case Status::UnsupportedBuild:return "This game build is unsupported.";
    case Status::UnexpectedImports:return "The game has unexpected import routing.";
    case Status::UntrustedInterposer:return "The interposer could not be verified.";
    case Status::StartupTooLate:return "The game already created its graphics factory.";
    case Status::AccessDenied:return "Windows denied the required access.";
    case Status::RolledBack:return "The update failed and original imports were restored.";
    case Status::StartupAborted:return "The owned startup was observed to exit after abort.";
    case Status::LaunchFailed:return "The launcher could not complete startup.";
    case Status::TimedOut:return "The operation was cancelled or reached its deadline.";
    case Status::ConfigurationInvalid:return "The launcher configuration or command line is invalid.";
    case Status::DllConflict:return "An application-local DLL conflict prevents the update.";
    case Status::ReadOnlyCompleted:return "The read-only operation completed.";
    case Status::SupportFailed:return "The local support bundle could not be collected.";
  }
  return "The launcher could not complete the operation.";
}
Status FailureStatus(const Error& error) {
  switch(error.code) {
    case ErrorCode::ConfigurationInvalid:return Status::ConfigurationInvalid;
    case ErrorCode::AlreadyRunning:return Status::AlreadyRunning;
    case ErrorCode::MalformedImage:case ErrorCode::UnsupportedBuild:case ErrorCode::UnreadableMemory:return Status::UnsupportedBuild;
    case ErrorCode::UnexpectedImports:return Status::UnexpectedImports;
    case ErrorCode::UntrustedInterposer:return Status::UntrustedInterposer;
    case ErrorCode::StartupTooLate:return Status::StartupTooLate;
    case ErrorCode::AccessDenied:return Status::AccessDenied;
    case ErrorCode::TimedOut:return Status::TimedOut;
    case ErrorCode::DllConflict:return Status::DllConflict;
    case ErrorCode::SupportBundleFailure:return Status::SupportFailed;
    default:return Status::LaunchFailed;
  }
}
Error Timeout(Stage stage){return {ErrorCode::TimedOut,stage,"launcher_deadline"};}
std::string HashText(const Hash256& hash){constexpr char digits[]="0123456789abcdef";std::string text;text.reserve(64);for(auto b:hash){const auto v=std::to_integer<unsigned>(b);text+=digits[v>>4];text+=digits[v&15];}return text;}
void InventoryFacts(DiagnosticRecord& record,const RuntimeInventory& inventory) {
  record.game_package={inventory.package_name,Availability::Observed,"package_manifest",""};
  record.game_version={inventory.package_version,Availability::Observed,"package_manifest",""};
  record.executable_timestamp={inventory.executable.image.timestamp,Availability::Observed,"pe_inspection",""};
  if(inventory.interposer.loaded_module.image.base) {
    record.interposer_version={inventory.interposer.version,Availability::Observed,"version_resource",""};
    record.interposer_sha256={HashText(inventory.interposer.sha256),Availability::Observed,"file_sha256",""};
    record.interposer_signature_valid={inventory.interposer.trusted_nvidia_signer,Availability::Observed,"authenticode",""};
  }
  for(const auto& dependency:inventory.dependencies)if(dependency.role.availability==Availability::Observed&&dependency.role.value&&
    (*dependency.role.value==DependencyRole::StreamlinePlugin||*dependency.role.value==DependencyRole::NgxPlugin)) {
    record.loaded_plugin={true,Availability::Observed,"runtime_inventory",""};break;
  }
}
void UnavailableObservation(DiagnosticRecord& record,const Error& error) {
  record.runtime_observation.emplace();
  auto& runtime=*record.runtime_observation;
  const auto availability=error.code==ErrorCode::NoMatchingProcess?Availability::NotObserved:Availability::ReadFailed;
  const auto missing=[&](auto& field) {field={std::nullopt,availability,"read_only_runtime","Observation target could not be opened."};};
  missing(runtime.latest.requested_mode);missing(runtime.latest.requested_count);
  missing(runtime.latest.sdk_status);missing(runtime.latest.sdk_code);missing(runtime.latest.frame_id);
  missing(runtime.latest.actual_presentations);missing(runtime.latest.generation_active);
  missing(runtime.latest.factory_created);missing(runtime.latest.factory_proxy);
  missing(runtime.latest.swap_chain_intercepted);missing(runtime.latest.decoded_rejection);missing(runtime.import_routing);
}
void ObservationFacts(DiagnosticRecord& record,const ObservationReport& report) {
  record.runtime_observation=RuntimeDiagnosticRecord{report.latest,report.import_routing,report.samples,
    static_cast<std::uint64_t>(std::max<std::int64_t>(0,report.duration.count())),report.cancelled,report.process_lost,report.acquisition_overran,report.activity};
  record.environment=report.environment;
  if(report.inventory.availability==Availability::Observed&&report.inventory.value)InventoryFacts(record,*report.inventory.value);
  switch(report.activity) {
    case ActivityAssessment::ActivityObserved:record.frame_generation=FrameGenerationState::ActivityObserved;break;
    case ActivityAssessment::NotObservedInWindow:record.frame_generation=FrameGenerationState::ActivityNotObserved;break;
    case ActivityAssessment::RejectionObserved:
      if(report.sdk_rejection){record.frame_generation=FrameGenerationState::Rejected;record.decoded_rejection_code=report.sdk_rejection->code;}break;
    default:record.frame_generation=FrameGenerationState::Unknown;break;
  }
}
std::array<char,20> UtcStamp() {
  const auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());std::tm utc{};gmtime_s(&utc,&t);
  std::array<char,21> buffer{};std::strftime(buffer.data(),buffer.size(),"%Y-%m-%dT%H:%M:%SZ",&utc);
  std::array<char,20> result{};std::copy_n(buffer.begin(),20,result.begin());return result;
}
constexpr std::string_view Help="WitcherFG\nUsage: WitcherFG [--help | --version | --diagnose | --collect-support] [--log-level error|info|debug]\nNo arguments: launch the registered Xbox PC game and attempt guarded session routing.\n--diagnose: observe an existing registered game without activation or debugger attach.\n--collect-support: create a local sanitized support archive.\nRuntime observation is off by default; no telemetry or automatic elevation.\n";
}

int ExitCode(Status status) noexcept {
  switch(status) {
    case Status::Applied:case Status::RoutingAlreadyPresent:case Status::ReadOnlyCompleted:return 0;
    case Status::AlreadyRunning:case Status::UnsupportedBuild:case Status::UnexpectedImports:case Status::UntrustedInterposer:
    case Status::StartupTooLate:case Status::AccessDenied:case Status::ConfigurationInvalid:case Status::DllConflict:return 2;
    case Status::StartupAborted:return 4;
    default:return 3;
  }
}
std::wstring FormatStatus(const RunResult& result) {
  std::string text(StatusText(result.status));
  if(result.failure) {
    const auto failure=ExplainFailure(*result.failure);
    text+="\n"+failure.code+" stage="+std::string(StageText(failure.stage))+" operation="+failure.operation;
    if(failure.raw_code)text+=" Windows="+std::to_string(*failure.raw_code);
    text+="\nNext action: "+(result.next_action.empty()?failure.action:result.next_action);
  }
  if(result.diagnostic_file_failed)text+="\nDiagnostics could not be saved; retain the console summary.";
  return {text.begin(),text.end()};
}

RunResult RunLauncher(ILaunchEnvironment& env,std::stop_token external) {
  const auto start=env.Now();const Deadline total{start+std::chrono::seconds(60)};
  std::stop_source cancellation;
  std::stop_callback forward(external,[&]{cancellation.request_stop();});
  std::unique_ptr<IRunGuard> guard;
  std::optional<DebugSession> session;
  std::optional<OwnedProcess> owned;
  std::unique_ptr<IWritableMemory> memory;
  DiagnosticRecord record;record.safety=SafetyDisposition::VerifiedNoMutation;
  RunResult result;
  Stage stage=Stage::configuration;
  const auto elapsed=[&]{return static_cast<std::uint64_t>(std::max<std::int64_t>(0,std::chrono::duration_cast<std::chrono::milliseconds>(env.Now()-start).count()));};
  const auto refuse=[&](Error error) {
    result.status=FailureStatus(error);result.failure=error;
    result.diagnostic={error.code,error.stage,std::string(error.operation.view()),std::chrono::system_clock::now()};
    auto failure=ExplainFailure(error);failure.elapsed_ms=elapsed();result.next_action=failure.action;record.failure=std::move(failure);
    if(result.status==Status::AlreadyRunning)result.next_action="Close the existing game, then start a fresh session through WitcherFG.";
    if(result.status==Status::SupportFailed)result.next_action="Close other diagnostic writers and retry collection; retain the console result.";
    record.failure->action=result.next_action;
    record.outcome=ExitCode(result.status)==2?RunOutcome::Refused:RunOutcome::Failed;
    record.hook_state=HookState::Refused;
  };
  const auto expired=[&]{return cancellation.stop_requested()||env.Now()>=total.value;};
  const auto cleanup=[&] {
    if(!session)return;
    if(session->Safety()==DebugSafety::AttachedUnverified)env.ExitHelper();
    if(session->Safety()==DebugSafety::AttachedSafe) {
      // A cancelled/expired inspection gets only its bounded cleanup grace.
      auto limit=total.value;
      if(cancellation.stop_requested())limit=std::min(limit+std::chrono::seconds(2),env.Now()+std::chrono::seconds(2));
      else if(env.Now()>=limit)limit+=std::chrono::seconds(2);
      const auto detached=session->Detach({limit});
      if(!IsOk(detached)){result.detach=DetachOutcome::Failed;env.ExitHelper();}
      result.detach=DetachOutcome::Detached;
    }
  };
  const auto stamp_completion=[&] {record.elapsed_ms=elapsed();record.completed_utc=UtcStamp();};
  const auto complete=[&] {
    cleanup();
    if(record.completed_utc[0]==0)stamp_completion();
    // This is the native final-status route. Keep the same guard and stop
    // callback alive through formatting and the sink's potentially blocking flush.
    const auto wide=FormatStatus(result);
    std::string text;for(const auto ch:wide)text.push_back(static_cast<char>(ch));
    text+="\nCompleted UTC: "+std::string(record.completed_utc.data(),record.completed_utc.size())+
      " elapsed_ms="+std::to_string(record.elapsed_ms);
    env.Diagnostics().WriteConsoleLine(text);
    if(guard)guard->Disarm();return result;
  };
  const auto finish=[&] {
    cleanup();stamp_completion();
    try {
    RunLogger logger(record.effective_config,env.Diagnostics());
    if(result.failure){DiagnosticEvent failure{EventLevel::Failure,EventCode::Failure,result.failure->stage,result.failure->operation.view()};failure.failure=result.failure;failure.elapsed_ms=elapsed();failure.time_utc=UtcStamp();static_cast<void>(logger.Emit(failure));}
    const auto written=logger.Finish(record);
    result.diagnostic_file_failed=result.diagnostic_file_failed||!IsOk(written);
    } catch(...) {
      result.diagnostic_file_failed=true;
      const auto summary=SerializeSummary(record);
      if(IsOk(summary))env.Diagnostics().WriteConsoleLine(Value(summary));
      else env.Diagnostics().WriteConsoleLine("Diagnostic serialization failed; retain the final status.");
    }
    return complete();
  };
  try {
    guard=env.Guard(total,cancellation,external);
    const auto options=ParseOptions(env.Arguments());
    if(!IsOk(options)){refuse(Failure(options));return finish();}
    if(Value(options).mode==Mode::Help||Value(options).mode==Mode::Version) {
      result.status=Status::ReadOnlyCompleted;
      if(Value(options).mode==Mode::Help)env.Diagnostics().WriteConsoleLine(Help);
      else {const auto& build=EmbeddedBuildIdentity();env.Diagnostics().WriteConsoleLine(std::string("WitcherFG ")+std::string(build.version)+" "+std::string(build.build_type)+" commit="+std::string(build.commit)+" input="+std::string(build.input_digest)+" channel="+std::string(build.channel)+(build.dirty?" dirty=true":" dirty=false")+(build.archive?" archive=true":" archive=false")+" diagnostic_schema="+std::to_string(build.diagnostic_schema));}
      return complete();
    }
    if(expired()){refuse(Timeout(stage));return finish();}
    const auto config=env.Configuration(Value(options));
    if(!IsOk(config)){refuse(Failure(config));return finish();}
    record.effective_config=Value(config);
    if(expired()){refuse(Timeout(stage));return finish();}
    RunLogger stages(record.effective_config,env.Diagnostics());
    const auto event=[&](Stage where,EventCode code,std::string_view operation) {
      stage=where;DiagnosticEvent entry{EventLevel::Information,code,where,operation};entry.elapsed_ms=elapsed();entry.time_utc=UtcStamp();static_cast<void>(stages.Emit(entry));
    };
    event(stage,EventCode::StageCompleted,"configuration_completed");
    if(Value(options).mode==Mode::CollectSupport) {
      // Collection owns opening the persisted producer files after closing logging.
      const auto collected=env.Collect(record.effective_config);
      if(!IsOk(collected))refuse(Failure(collected));else result.status=Status::ReadOnlyCompleted;
      // Collection preserves the last launch summary for a later support run.
      cleanup();stamp_completion();
      const auto summary=SerializeSummary(record);
      if(IsOk(summary))env.Diagnostics().WriteConsoleLine(Value(summary));
      return complete();
    }
    event(Stage::package_discovery,EventCode::Initialization,"package_discovery_entered");
    const auto package=env.Discover();
    if(!IsOk(package)){refuse(Failure(package));return finish();}
    record.game_package={Value(package).name,Availability::Observed,"package_manifest",""};
    record.game_version={Value(package).version,Availability::Observed,"package_manifest",""};
    record.environment=env.Environment();
    event(stage,EventCode::StageCompleted,"package_discovery_completed");
    if(expired()){refuse(Timeout(stage));return finish();}
    const auto observe=[&](const OwnedProcess* process) {
      guard->Disarm();
      guard=env.Guard({env.Now()+std::chrono::seconds(record.effective_config.observation_seconds)},cancellation,external);
      event(Stage::runtime_observation,EventCode::Initialization,"runtime_observation_entered");
      const auto observed=env.Observe(process,record.effective_config,cancellation.get_token());
      if(IsOk(observed))ObservationFacts(record,Value(observed));
      else {UnavailableObservation(record,Failure(observed));auto unavailable=Failure(observed);unavailable.code=ErrorCode::UnsupportedObservation;record.failure=ExplainFailure(unavailable);}
      DiagnosticEvent outcome{EventLevel::CriticalWarning,EventCode::ObservationUnavailable,stage,"runtime_observation_completed"};
      outcome.elapsed_ms=elapsed();outcome.time_utc=UtcStamp();
      std::array<DiagnosticField,1> decoded{};
      if(record.frame_generation==FrameGenerationState::Rejected&&record.decoded_rejection_code) {
        outcome.code=EventCode::FgRejected;outcome.vendor_code=record.decoded_rejection_code;
        decoded[0]={DiagnosticFieldKey::DecodedRejectionCode,DiagnosticFieldSource::RuntimeObservation,std::uint64_t{*record.decoded_rejection_code}};
        outcome.fields=decoded;
      }
      else if(record.frame_generation==FrameGenerationState::ActivityNotObserved)outcome.code=EventCode::FgActivityNotObserved;
      if(record.frame_generation!=FrameGenerationState::ActivityObserved)static_cast<void>(stages.Emit(outcome));
      event(stage,EventCode::StageCompleted,"runtime_observation_completed");
    };
    if(Value(options).mode==Mode::Diagnose) {observe(nullptr);result.status=Status::ReadOnlyCompleted;return finish();}
    event(Stage::activation,EventCode::Initialization,"activation_entered");
    const auto installed=env.InstallObserver({start+std::chrono::seconds(30)});
    if(!IsOk(installed)){refuse(Failure(installed));return finish();}
    if(expired()){refuse(Timeout(stage));return finish();}
    const auto activated=env.Activate();if(!IsOk(activated)){refuse(Failure(activated));return finish();}
    event(stage,EventCode::StageCompleted,"activation_completed");
    event(Stage::process_identity,EventCode::Initialization,"process_identity_entered");
    auto pin=env.PinFresh();if(!IsOk(pin)){refuse(Failure(pin));return finish();}
    owned.emplace(std::move(std::get<OwnedProcess>(pin)));
    if(expired()){refuse(Timeout(stage));return finish();}
    auto opened=env.Memory(*owned);if(!IsOk(opened)){refuse(Failure(opened));return finish();}
    memory=std::move(std::get<std::unique_ptr<IWritableMemory>>(opened));
    event(stage,EventCode::StageCompleted,"process_identity_completed");
    event(Stage::debug_attach,EventCode::Initialization,"debug_attach_entered");
    session.emplace(env.Debugger(),cancellation.get_token());
    const auto attached=session->Attach(*owned);
    // No formatting, allocations, logging, continuation or FALSE before containment.
    if(session->Safety()==DebugSafety::AttachedUnverified)env.ExitHelper();
    if(!IsOk(attached)) {
      refuse(Failure(attached));
      if(session->Safety()==DebugSafety::Exited){result.status=Status::StartupAborted;record.safety=SafetyDisposition::VerifiedAbort;record.outcome=RunOutcome::StartupAborted;record.hook_state=HookState::StartupAborted;}
      return finish();
    }
    event(stage,EventCode::StageCompleted,"debug_attach_completed");
    bool attach_ready{};
    for(;;) {
      stage=Stage::dependencies;
      if(expired()){refuse(Timeout(stage));return finish();}
      auto stopped=session->WaitStopped(total);if(!IsOk(stopped)){refuse(Failure(stopped));return finish();}
      auto& stop=std::get<DebugStop>(stopped);
      if(!attach_ready) {
        const auto& held=stop.event();
        attach_ready=held.kind==DebugEventKind::Exception&&held.attach_breakpoint_identity&&stop.continuation()==Continuation::Handled;
        if(!attach_ready) {
          const auto continued=session->Continue(stop,stop.continuation());
          if(!IsOk(continued)){refuse(Failure(continued));return finish();}
          continue;
        }
      }
      event(stage,EventCode::Initialization,"dependencies_entered");
      const auto inventory=env.Inventory(*owned,*memory);
      if(expired()){refuse(Timeout(stage));return finish();}
      if(!IsOk(inventory)) {
        if(Failure(inventory).code==ErrorCode::ModuleNotReady) {
          const auto continued=session->Continue(stop,stop.continuation());
          if(!IsOk(continued)){refuse(Failure(continued));return finish();}
          continue;
        }
        refuse(Failure(inventory));return finish();
      }
      InventoryFacts(record,Value(inventory));event(stage,EventCode::StageCompleted,"dependencies_completed");
      event(Stage::build_validation,EventCode::Initialization,"build_validation_entered");
      const auto conflicts=ClassifyLoaderConflicts(Value(inventory),env.Compatibility());
      if(!IsOk(conflicts)){refuse(Failure(conflicts));return finish();}
      const auto validated=ValidateBuild(*memory,Value(inventory),env.Compatibility());
      if(!IsOk(validated)) {
        if(Failure(validated).code==ErrorCode::ModuleNotReady&&Failure(validated).operation.view()=="import_binding_not_ready") {
          if(expired()){refuse(Timeout(stage));return finish();}
          const auto continued=session->Continue(stop,stop.continuation());
          if(!IsOk(continued)){refuse(Failure(continued));return finish();}
          continue;
        }
        refuse(Failure(validated));return finish();
      }
      event(stage,EventCode::StageCompleted,"build_validation_completed");
      if(expired()){refuse(Timeout(stage));return finish();}
      if(Value(validated).routing==Routing::AlreadyPresent) {result.status=Status::RoutingAlreadyPresent;record.outcome=RunOutcome::RoutingAlreadyPresent;record.hook_state=HookState::RoutingAlreadyPresent;break;}
      event(Stage::pre_factory_guard,EventCode::Initialization,"pre_factory_guard_entered");
      const auto factory=CheckFactory(*memory,Value(validated));
      if(!IsOk(factory)){refuse(Failure(factory));return finish();}
      if(Value(factory)!=FactoryState::NotCreated){refuse({ErrorCode::StartupTooLate,stage,"factory_created"});return finish();}
      event(stage,EventCode::StageCompleted,"pre_factory_guard_completed");
      if(expired()){refuse(Timeout(stage));return finish();}
      DiagnosticEvent transaction{EventLevel::Information,EventCode::MutationStep,Stage::import_transaction,"import_transaction_entered"};
      transaction.elapsed_ms=elapsed();transaction.time_utc=UtcStamp();static_cast<void>(stages.DeferMutationEvent(transaction));
      const auto applied=ApplyImports(*session,stop,*memory,Value(validated));
      if(IsOk(applied)&&Value(applied).immediate_helper_exit_required)env.ExitHelper();
      if(session->Safety()==DebugSafety::AttachedUnverified)env.ExitHelper();
      if(!IsOk(applied)){refuse(Failure(applied));break;}
      const auto& outcome=Value(applied);result.transaction=outcome.outcome;
      if(outcome.outcome==TransactionOutcome::Committed) {
        result.status=Status::Applied;record.safety=SafetyDisposition::VerifiedCommit;record.outcome=RunOutcome::Applied;record.hook_state=HookState::Applied;
      } else {
        refuse(outcome.original_error.value_or(Error{ErrorCode::MutationFailure,Stage::import_transaction,"transaction_failed"}));
        if(outcome.outcome==TransactionOutcome::RolledBack) {result.status=Status::RolledBack;record.safety=SafetyDisposition::VerifiedRollback;record.outcome=RunOutcome::RolledBack;record.hook_state=HookState::RolledBack;}
        else if(outcome.outcome==TransactionOutcome::Aborted) {result.status=Status::StartupAborted;record.safety=SafetyDisposition::VerifiedAbort;record.outcome=RunOutcome::StartupAborted;record.hook_state=HookState::StartupAborted;}
      }
      break;
    }
    event(Stage::detach,EventCode::Initialization,"detach_entered");cleanup();
    event(stage,EventCode::StageCompleted,"detach_completed");
    // Deferred transaction events are flushed only after verified safety and detach.
    stamp_completion();
    try {result.diagnostic_file_failed=!IsOk(stages.Finish(record));}
    catch(...) {result.diagnostic_file_failed=true;}
    if((result.status==Status::Applied||result.status==Status::RoutingAlreadyPresent)&&record.effective_config.runtime_observation&&!cancellation.stop_requested())observe(&*owned);
    const bool prior_failure=result.diagnostic_file_failed;auto final=finish();final.diagnostic_file_failed=final.diagnostic_file_failed||prior_failure;return final;
  } catch(...) {
    // Check this before constructing error records or invoking a sink.
    if(session&&session->Safety()==DebugSafety::AttachedUnverified)env.ExitHelper();
    if(result.transaction==TransactionOutcome::Committed||result.transaction==TransactionOutcome::RolledBack||result.transaction==TransactionOutcome::Aborted) {
      result.diagnostic_file_failed=true;
      record.failure=ExplainFailure({ErrorCode::DiagnosticFailure,stage,"launcher_exception"});
    } else refuse({ErrorCode::LaunchFailed,stage,"launcher_exception"});
    return finish();
  }
}

namespace {
class NativeSink final : public ILogStorage {
public:
  Result<Unit> AppendEvent(std::string_view line,std::uint32_t size,std::uint32_t count) override {
    const auto ready=Ensure();if(!IsOk(ready))return Failure(ready);return storage_->AppendEvent(line,size,count);
  }
  Result<Unit> WriteSummaryAtomically(std::string_view text) override {
    const auto ready=Ensure();if(!IsOk(ready))return Failure(ready);return storage_->WriteSummaryAtomically(text);
  }
  void WriteConsoleLine(std::string_view text) override {std::fwrite(text.data(),1,text.size(),stdout);std::fputc('\n',stdout);std::fflush(stdout);}
  void Close() noexcept {storage_.reset();}
private:
  Result<Unit> Ensure() {
    if(storage_)return Unit{};
    const auto directory=LocalAppDataDiagnosticsDirectory();if(!IsOk(directory))return Failure(directory);
    storage_=std::make_unique<FileLogStorage>(Value(directory));return Unit{};
  }
  std::unique_ptr<FileLogStorage> storage_;
};
class NativeGuard final : public IRunGuard {
public:
  NativeGuard(Deadline limit,std::stop_source& cancellation,std::stop_token external):watchdog_(limit,cancellation,external){}
  void Disarm() override {watchdog_.Disarm();}
private:
  DeadlineWatchdog watchdog_;
};
class NativeEnvironment final : public ILaunchEnvironment {
public:
  explicit NativeEnvironment(std::span<const std::string_view> args):args_(args){}
  std::span<const std::string_view> Arguments()const override{return args_;}
  std::chrono::steady_clock::time_point Now()const override{return std::chrono::steady_clock::now();}
  Result<EffectiveConfig> Configuration(const CommandLine& options)override {
    std::array<wchar_t,32768> path{};
    const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!length||length>=path.size())return Error{ErrorCode::ConfigurationInvalid,Stage::configuration,"executable_directory",GetLastError()};
    return LoadConfiguration(std::filesystem::path(std::wstring_view(path.data(),length)).parent_path(),options);
  }
  ILogStorage& Diagnostics()override{return sink_;}
  std::unique_ptr<IRunGuard> Guard(Deadline limit,std::stop_source& cancel,std::stop_token external)override{return std::make_unique<NativeGuard>(limit,cancel,external);}
  Result<PackageFacts> Discover()override {
    auto found=DiscoverPackage();if(!IsOk(found))return Failure(found);
    package_.emplace(std::move(std::get<PackageIdentity>(found)));
    return PackageFacts{package_->name(),package_->version(),package_->architecture()};
  }
  EnvironmentSnapshot Environment()override{return ObserveEnvironment(nullptr);}
  Result<Unit> InstallObserver(Deadline deadline)override {
    auto installed=StartObserver(*package_,deadline);if(!IsOk(installed))return Failure(installed);
    observer_=std::move(std::get<std::unique_ptr<ProcessObserver>>(installed));return Unit{};
  }
  Result<Unit> Activate()override{return observer_->Activate();}
  Result<OwnedProcess> PinFresh()override{return observer_->WaitForFreshProcess();}
  Result<std::unique_ptr<IWritableMemory>> Memory(const OwnedProcess& process)override{return OpenMemory(process);}
  IDebugBackend& Debugger()override{return debugger_;}
  Result<RuntimeInventory> Inventory(const OwnedProcess& process,const IMemory& memory)override{return InventoryProcess(process,memory);}
  const CompatibilityRecord& Compatibility()const override{return SupportedBuild();}
  Result<ObservationReport> Observe(const OwnedProcess* process,const EffectiveConfig& config,std::stop_token cancellation)override {
    auto observed=process?DuplicateObservedProcess(*process):OpenDiagnosticTarget(*package_);
    if(!IsOk(observed))return Failure(observed);
    return ObserveRuntime(Value(observed),config,cancellation);
  }
  Result<Unit> Collect(const EffectiveConfig& config)override {
    sink_.Close();
    WindowsSupportStorage storage;
    const SupportInputs inputs{1,{1,std::string(EmbeddedBuildIdentity().input_digest),"",config.runtime_observation,""},{},{}};
    const auto result=CollectSupport(inputs,storage);if(!IsOk(result))return Failure(result);
    sink_.WriteConsoleLine("Support archive created in the local WitcherFG support directory.");return Unit{};
  }
  [[noreturn]] void ExitHelper()noexcept override {TerminateProcess(GetCurrentProcess(),3);std::_Exit(3);}
private:
  std::span<const std::string_view> args_;
  NativeSink sink_;
  std::optional<PackageIdentity> package_;
  std::unique_ptr<ProcessObserver> observer_;
  NativeDebugBackend debugger_;
};
}
Result<Unit> WriteDiagnostic(const DiagnosticRecord& record) {
  NativeSink sink;RunLogger logger(record.effective_config,sink);return logger.Finish(record);
}
int RunLauncherToConsole(ILaunchEnvironment& environment,std::stop_token cancellation) {
  return ExitCode(RunLauncher(environment,cancellation).status);
}
int RunNativeLauncher(std::span<const std::string_view> arguments,std::stop_token cancellation) {
  NativeEnvironment environment(arguments);
  return RunLauncherToConsole(environment,cancellation);
}
}
