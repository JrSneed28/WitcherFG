#include "wfg/support_bundle.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>

namespace wfg {
namespace {
constexpr std::uint32_t SchemaVersion = 1;
constexpr std::size_t MaximumEntries = 32;
constexpr std::size_t MaximumPayloadBytes = 10U * 1024U * 1024U;
constexpr std::size_t MaximumSummaryBytes = 16U * 1024U;
constexpr std::size_t MaximumRecords = 5;
constexpr std::size_t MaximumEventLogBytes = MaximumRecords * 4096U + MaximumRecords;

Error Fail(std::string_view operation) {
  return {ErrorCode::SupportBundleFailure, Stage::Support, operation, std::nullopt, std::nullopt};
}
Error StorageReadFailure(const Error& error, std::string_view operation) {
  // Preserve only the recognized local-storage refusal; other storage errors
  // keep the existing normalization rather than forwarding arbitrary details.
  constexpr std::uint32_t WindowsNotSupported = 50U;  // ERROR_NOT_SUPPORTED.
  if (error.code == ErrorCode::SupportBundleFailure && error.stage == Stage::Support &&
      error.operation.view() == "SupportLocalStorageRequired" && error.native_code == WindowsNotSupported)
    return {ErrorCode::SupportBundleFailure,Stage::Support,"SupportLocalStorageRequired",WindowsNotSupported,std::nullopt};
  return Fail(operation);
}

bool IsSafeName(std::string_view name) {
  if (name.empty() || name.size() > 96 || name.front() == '/' || name.back() == '/') return false;
  for (std::size_t index = 0; index < name.size(); ++index) {
    const char character = name[index];
    const bool allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-' || character == '/';
    if (!allowed || (character == '/' && (index == 0 || name[index - 1] == '/'))) return false;
  }
  return name.find("..") == std::string_view::npos && name.find('\\') == std::string_view::npos;
}

std::string SafeToken(std::string_view value) {
  if (value.empty() || value.size() > 64) return "unavailable";
  for (const char character : value) {
    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-')) return "unavailable";
  }
  return std::string(value);
}

const char* StageName(Stage stage) {
  switch (stage) {
    case Stage::configuration: return "configuration";
    case Stage::package_discovery: return "package_discovery";
    case Stage::activation: return "activation";
    case Stage::process_identity: return "process_identity";
    case Stage::debug_attach: return "debug_attach";
    case Stage::dependencies: return "dependencies";
    case Stage::build_validation: return "build_validation";
    case Stage::pre_factory_guard: return "pre_factory_guard";
    case Stage::import_transaction: return "import_transaction";
    case Stage::detach: return "detach";
    case Stage::runtime_observation: return "runtime_observation";
  }
  return "unknown";
}

const char* ErrorName(ErrorCode code) {
  switch (code) {
    case ErrorCode::MalformedImage: return "MalformedImage"; case ErrorCode::UnsupportedBuild: return "UnsupportedBuild";
    case ErrorCode::UnexpectedImports: return "UnexpectedImports"; case ErrorCode::UntrustedInterposer: return "UntrustedInterposer";
    case ErrorCode::UnreadableMemory: return "UnreadableMemory"; case ErrorCode::ModuleNotReady: return "ModuleNotReady";
    case ErrorCode::NoMatchingProcess: return "NoMatchingProcess"; case ErrorCode::IdentityChanged: return "IdentityChanged";
    case ErrorCode::AmbiguousProcess: return "AmbiguousProcess"; case ErrorCode::AlreadyRunning: return "AlreadyRunning";
    case ErrorCode::StartupTooLate: return "StartupTooLate"; case ErrorCode::AccessDenied: return "AccessDenied";
    case ErrorCode::LaunchFailed: return "LaunchFailed"; case ErrorCode::TimedOut: return "TimedOut";
    case ErrorCode::DebugFailure: return "DebugFailure"; case ErrorCode::MutationFailure: return "MutationFailure";
    case ErrorCode::DiagnosticFailure: return "DiagnosticFailure"; case ErrorCode::ConfigurationInvalid: return "ConfigurationInvalid";
    case ErrorCode::DllConflict: return "DllConflict"; case ErrorCode::UnsupportedObservation: return "UnsupportedObservation";
    case ErrorCode::SupportBundleFailure: return "SupportBundleFailure";
  }
  return "SupportBundleFailure";
}

const char* OutcomeName(SupportRunOutcome outcome) {
  switch (outcome) {
    case SupportRunOutcome::NotStarted: return "not_started"; case SupportRunOutcome::Applied: return "applied";
    case SupportRunOutcome::RoutingAlreadyPresent: return "routing_already_present"; case SupportRunOutcome::Refused: return "refused";
    case SupportRunOutcome::RolledBack: return "rolled_back"; case SupportRunOutcome::StartupAborted: return "startup_aborted";
    case SupportRunOutcome::Failed: return "failed";
  }
  return "not_started";
}

std::vector<std::byte> Encode(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

struct JsonValue {
  enum class Type { Null, Boolean, Number, Real, String, Object, Array } type{Type::Null};
  bool boolean{};
  std::uint64_t number{};
  std::string string;
  std::vector<std::pair<std::string, JsonValue>> object;
  std::vector<JsonValue> array;
};

bool ValidUtf8(std::string_view text) {
  for (std::size_t index = 0; index < text.size();) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80) { ++index; continue; }
    const std::size_t width = lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
    if (!width || width > text.size() - index) return false;
    for (std::size_t offset = 1; offset < width; ++offset) if (static_cast<unsigned char>(text[index + offset]) < 0x80 || static_cast<unsigned char>(text[index + offset]) > 0xbf) return false;
    const auto second = static_cast<unsigned char>(text[index + 1]);
    if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second > 0x9f) || (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second > 0x8f)) return false;
    index += width;
  }
  return true;
}

class JsonParser {
public:
  explicit JsonParser(std::string_view source) : source_(source) {}
  bool Parse(JsonValue& value) { return ValidUtf8(source_) && Value(value, 0) && Skip() && position_ == source_.size(); }
private:
  bool Skip() { while (position_ < source_.size() && (source_[position_] == ' ' || source_[position_] == '\n' || source_[position_] == '\r' || source_[position_] == '\t')) ++position_; return true; }
  bool Consume(char character) { Skip(); if (position_ == source_.size() || source_[position_] != character) return false; ++position_; return true; }
  bool String(std::string& value) {
    if (!Consume('"')) return false;
    while (position_ < source_.size() && source_[position_] != '"') {
      const unsigned char character=static_cast<unsigned char>(source_[position_++]);
      if (character < 0x20) return false;
      if (character != '\\') { value.push_back(static_cast<char>(character)); continue; }
      if (position_ == source_.size()) return false;
      const char escaped=source_[position_++];
      if (escaped == '"' || escaped == '\\' || escaped == '/') value.push_back(escaped);
      else if (escaped == 'b') value.push_back('\b'); else if (escaped == 'f') value.push_back('\f');
      else if (escaped == 'n') value.push_back('\n'); else if (escaped == 'r') value.push_back('\r'); else if (escaped == 't') value.push_back('\t');
      else if (escaped == 'u') {
        auto hex = [&]() -> int { if (position_ + 4 > source_.size()) return -1; int result{}; for (int index = 0; index < 4; ++index) { const char digit = source_[position_++]; const int number = digit >= '0' && digit <= '9' ? digit - '0' : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1; if (number < 0) return -1; result = result * 16 + number; } return result; };
        const int code = hex(); if (code < 0 || (code >= 0xdc00 && code <= 0xdfff)) return false;
        if (code >= 0xd800 && code <= 0xdbff) { if (source_.substr(position_, 2) != "\\u") return false; position_ += 2; const int low = hex(); if (low < 0xdc00 || low > 0xdfff) return false; }
        value.push_back(code < 0x80 ? static_cast<char>(code) : '?');
      }
      else return false;
    }
    if (position_ == source_.size() || value.size() > MaximumSummaryBytes) return false;
    ++position_; return true;
  }
  bool Value(JsonValue& value, std::size_t depth) {
    if (depth > 12 || ++nodes_ > 4096) return false;
    Skip();
    if (position_ == source_.size()) return false;
    if (source_[position_] == '"') { value.type=JsonValue::Type::String; return String(value.string); }
    if (source_[position_] == '{') { value.type=JsonValue::Type::Object; ++position_; Skip(); if (position_ < source_.size() && source_[position_] == '}') { ++position_; return true; } while (value.object.size() < 64) { std::string key; JsonValue member; if (!String(key) || !Consume(':') || !Value(member,depth+1)) return false; for (const auto& previous : value.object) if (previous.first == key) return false; value.object.emplace_back(std::move(key),std::move(member)); Skip(); if (position_ < source_.size() && source_[position_] == '}') { ++position_; return true; } if (!Consume(',')) return false; } return false; }
    if (source_[position_] == '[') { value.type=JsonValue::Type::Array; ++position_; Skip(); if (position_ < source_.size() && source_[position_] == ']') { ++position_; return true; } while (value.array.size() < 64) { JsonValue member; if (!Value(member,depth+1)) return false; value.array.push_back(std::move(member)); Skip(); if (position_ < source_.size() && source_[position_] == ']') { ++position_; return true; } if (!Consume(',')) return false; } return false; }
    if (source_.substr(position_,4) == "true") { position_+=4; value.type=JsonValue::Type::Boolean; value.boolean=true; return true; }
    if (source_.substr(position_,5) == "false") { position_+=5; value.type=JsonValue::Type::Boolean; return true; }
    if (source_.substr(position_,4) == "null") { position_+=4; return true; }
    const auto start = position_;
    bool integral = true;
    if (source_[position_] == '-') { integral = false; ++position_; }
    if (position_ == source_.size() || !std::isdigit(static_cast<unsigned char>(source_[position_]))) return false;
    if (source_[position_] == '0' && position_ + 1 < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_ + 1]))) return false;
    while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_]))) ++position_;
    if (position_ < source_.size() && source_[position_] == '.') {
      integral = false; ++position_; const auto fraction = position_;
      while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_]))) ++position_;
      if (fraction == position_) return false;
    }
    if (position_ < source_.size() && (source_[position_] == 'e' || source_[position_] == 'E')) {
      integral = false; ++position_;
      if (position_ < source_.size() && (source_[position_] == '+' || source_[position_] == '-')) ++position_;
      const auto exponent = position_;
      while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_]))) ++position_;
      if (exponent == position_) return false;
    }
    const auto parsed = std::from_chars(source_.data() + start, source_.data() + position_, value.number);
    value.type = integral && parsed.ec == std::errc{} ? JsonValue::Type::Number : JsonValue::Type::Real;
    return true;
  }
  std::string_view source_; std::size_t position_{}; std::size_t nodes_{};
};

const JsonValue* Member(const JsonValue& object, std::string_view name) { if (object.type != JsonValue::Type::Object) return nullptr; for (const auto& member : object.object) if (member.first == name) return &member.second; return nullptr; }
const JsonValue* ObjectMember(const JsonValue& object, std::string_view name) { const auto* member=Member(object,name); return member && member->type == JsonValue::Type::Object ? member : nullptr; }
const JsonValue* StringMember(const JsonValue& object, std::string_view name) { const auto* member=Member(object,name); return member && member->type == JsonValue::Type::String ? member : nullptr; }
const JsonValue* NumberMember(const JsonValue& object, std::string_view name) { const auto* member=Member(object,name); return member && member->type == JsonValue::Type::Number ? member : nullptr; }
const JsonValue* BooleanMember(const JsonValue& object, std::string_view name) { const auto* member=Member(object,name); return member && member->type == JsonValue::Type::Boolean ? member : nullptr; }

bool IsChoice(std::string_view value, std::initializer_list<std::string_view> choices) { return std::find(choices.begin(), choices.end(), value) != choices.end(); }
std::string JsonString(std::string_view value) { return "\"" + SafeToken(value) + "\""; }
std::string_view SafeOperation(std::string_view value) {
  // Persisted identifiers are untrusted even when their character shape is valid.
  // New producer operations must be explicitly admitted here before export.
  return IsChoice(value, {"ValidateBuild","WriteIat","VerifyIat","IatWrite","IatVerify","RestoreProtection",
      "ProtectionRestore","RollbackAbort","AbortAfterRollbackFailure","DebugActiveProcess","DebugActiveProcessStop",
      "DebugSetProcessKillOnExit","WaitForDebugEvent","ContinueDebugEvent","NtQueryInformationThread",
      "activation_deadline","activation_existing","activation_required","activation_state","discovery_deadline",
      "preexisting_process","no_candidate","multiple_candidates","package_identity","package_registration",
      "registered_activation","registration_changed","pinned_identity","pin_changed","read_exact","query_memory",
      "protect_memory","write_exact","debug_read_memory","debug_query_memory","debug_target_exited",
      "debug_wait_deadline","debug_detach_deadline","attach_cancelled","mutation_cancelled","abort_owned_startup",
      "executable_identity","interposer_identity","interposer_provenance","import_provenance","import_rva",
      "fingerprint_read","fingerprint_hash","factory_read","manager_read","mixed_routing","application_local_loader",
      "transaction_routing","transaction_stop","factory_created","pointer_verification","protection_verification",
      "restore_previous_protection","diagnostic_layout_unavailable","observation_creation_changed",
      "observation_process_not_current","diagnostic_io","invalid_operation"}) ? value : "redacted";
}

bool Version(std::string_view value) {
  if (value.empty() || value.size() > 40 || value.front() < '0' || value.front() > '9') return false;
  unsigned dots{};
  for (const char character : value) {
    if (character == '.') ++dots;
    else if (character < '0' || character > '9') return false;
  }
  return dots >= 1 && dots <= 3 && value.back() != '.' && value.find("..") == std::string_view::npos;
}
bool HexShape(std::string_view value, std::size_t length) {
  return value.size() == length && std::all_of(value.begin(), value.end(), [](char character) { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F'); });
}
bool StageValue(std::string_view value) {
  return IsChoice(value, {"configuration","package_discovery","activation","process_identity","debug_attach","dependencies","build_validation","pre_factory_guard","import_transaction","detach","runtime_observation"});
}
bool CodeValue(std::string_view value) {
  return IsChoice(value, {"WFG-E-LAUNCH_FAILED","WFG-E-CONFIG_INVALID","WFG-E-BUILD_UNSUPPORTED","WFG-E-DLL_CONFLICT","WFG-E-INTERPOSER_UNTRUSTED","WFG-E-ATTACH_DENIED","WFG-E-STARTUP_TOO_LATE","WFG-E-IAT_VERIFY","WFG-E-PROTECTION_RESTORE","WFG-E-ROLLBACK_ABORT","WFG-E-IAT_WRITE","WFG-E-DIAGNOSTIC_IO","WFG-W-OBSERVATION_UNAVAILABLE","WFG-W-FG_ACTIVITY_NOT_OBSERVED","WFG-E-FG_REJECTED"});
}
std::string StableMessage(std::string_view code) {
  if (code == "WFG-E-BUILD_UNSUPPORTED") return "Supported_build_required";
  if (code == "WFG-W-OBSERVATION_UNAVAILABLE") return "Observation_unavailable";
  if (code == "WFG-E-FG_REJECTED") return "Decoded_frame_generation_rejection";
  if (code == "WFG-W-FG_ACTIVITY_NOT_OBSERVED") return "Frame_generation_activity_not_observed";
  return "Recorded_launcher_result";
}
std::string Primitive(const JsonValue& value) {
  if (value.type == JsonValue::Type::Number) return std::to_string(value.number);
  if (value.type == JsonValue::Type::Boolean) return value.boolean ? "true" : "false";
  return "null";
}
std::string SafeUtc(std::string_view value) {
  if (value == "unknown") return "unknown";
  if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':' || value[19] != 'Z') return "unknown";
  for (std::size_t index = 0; index < value.size(); ++index) if (index != 4 && index != 7 && index != 10 && index != 13 && index != 16 && index != 19 && (value[index] < '0' || value[index] > '9')) return "unknown";
  const auto pair = [&](std::size_t index) { return (value[index] - '0') * 10 + value[index + 1] - '0'; };
  if (pair(5) < 1 || pair(5) > 12 || pair(8) < 1 || pair(8) > 31 || pair(11) > 23 || pair(14) > 59 || pair(17) > 59) return "unknown";
  return std::string(value);
}
bool ObservedJson(const JsonValue& parent, std::string_view key, std::string& output) {
  const auto* observed = ObjectMember(parent, key);
  if (!observed) return false;
  const auto* availability = StringMember(*observed, "availability");
  const auto* source = StringMember(*observed, "source");
  const auto* value = Member(*observed, "value");
  if (!availability || !source || !value || !IsChoice(availability->string, {"observed","not_observed","unsupported_layout","read_failed"})) return false;
  if (availability->string != "observed" && value->type != JsonValue::Type::Null) return false;
  std::string safe_value = "null";
  const bool text_shape = IsChoice(key, {"package","version","interposer_version","interposer_sha256","os","gpu","driver","selected_adapter"});
  const bool bool_shape = IsChoice(key, {"interposer_signature_valid","loaded_plugin","hags_configured","hags_active"});
  if (value->type != JsonValue::Type::Null && value->type != (text_shape ? JsonValue::Type::String : bool_shape ? JsonValue::Type::Boolean : JsonValue::Type::Number)) return false;
  if (value->type == JsonValue::Type::String) {
    const auto& text = value->string;
    const bool safe = key == "package" ? text == "CDPROJEKTSA.Witcher3GOTY" :
        key == "interposer_sha256" ? HexShape(text, 64) :
        key == "version" || key == "interposer_version" || key == "driver" ? Version(text) :
        key == "os" ? IsChoice(text, {"Windows","Windows 10","Windows 11"}) : false;
    safe_value = safe ? "\"" + text + "\"" : "\"redacted\"";
  } else {
    if (value->type == JsonValue::Type::Number && value->number > UINT32_MAX) return false;
    safe_value = Primitive(*value);
  }
  const auto safe_source = IsChoice(source->string, {"launcher","package_manifest","version_resource","authenticode","pe_inspection","factory_guard","dxgi_observation","runtime_observation","unknown","none"}) ? source->string : "unknown";
  output = "{\"availability\":" + JsonString(availability->string) + ",\"source\":" + JsonString(safe_source) + ",\"value\":" + safe_value + "}";
  return true;
}
bool FailureJson(const JsonValue& parent, std::string& output) {
  const auto* failure = Member(parent, "failure");
  if (!failure) return false;
  if (failure->type == JsonValue::Type::Null) { output = "null"; return true; }
  const auto* code = StringMember(*failure, "code"); const auto* stage = StringMember(*failure, "stage"); const auto* operation = StringMember(*failure, "operation");
  const auto* elapsed = NumberMember(*failure, "elapsed_ms");
  const auto* time = StringMember(*failure, "time_utc");
  if (!code || !stage || !operation || !elapsed || !time || !CodeValue(code->string) || !StageValue(stage->string)) return false;
  output = "{\"code\":" + JsonString(code->string) + ",\"stage\":" + JsonString(stage->string) + ",\"operation\":" +
      JsonString(SafeOperation(operation->string)) + ",\"elapsed_ms\":" + std::to_string(elapsed->number) + ",\"time_utc\":\"" + SafeUtc(time->string) + "\",\"message\":" + JsonString(StableMessage(code->string));
  for (const auto key : {"raw_code","vendor_code"}) {
    const auto* member = Member(*failure,key);
    if (!member || (member->type != JsonValue::Type::Null && (member->type != JsonValue::Type::Number || member->number > UINT32_MAX))) return false;
    output += ",\"" + std::string(key) + "\":" + Primitive(*member);
  }
  output += "}";
  return true;
}

Result<BundleEntry> SanitizeSummary(std::string_view text) {
  JsonValue root; JsonParser parser(text);
  if (!parser.Parse(root)) return Fail("ParseDiagnosticSummary");
  const auto* schema=NumberMember(root,"schema_version");
  if (!schema || schema->number != SchemaVersion) return Fail("ParseDiagnosticSummary");
  const auto* build=ObjectMember(root,"build"); const auto* configuration=ObjectMember(root,"configuration"); const auto* outcome=StringMember(root,"outcome"); const auto* elapsed=NumberMember(root,"elapsed_ms");
  if (!build || !configuration || !outcome || !elapsed || !IsChoice(outcome->string,{"not_started","applied","routing_already_present","refused","rolled_back","startup_aborted","failed"})) return Fail("ParseDiagnosticSummary");
  const auto* build_version=StringMember(*build,"version"); const auto* build_channel=StringMember(*build,"channel"); const auto* build_schema=NumberMember(*build,"diagnostic_schema");
  const auto* log_level=StringMember(*configuration,"log_level"); const auto* runtime=BooleanMember(*configuration,"runtime_observation");
  if (!build_version || !build_channel || !build_schema || build_schema->number != SchemaVersion || !log_level || !runtime || !IsChoice(log_level->string,{"error","info","debug"})) return Fail("ParseDiagnosticSummary");
  const auto safe_version = Version(build_version->string) || build_version->string == "0.1.0-preview.1" ? build_version->string : "redacted";
  const auto safe_channel = IsChoice(build_channel->string, {"preview","stable","development","release"}) ? build_channel->string : "redacted";
  std::string output="{\"schema_version\":1,\"build\":{\"version\":"+JsonString(safe_version)+",\"channel\":"+JsonString(safe_channel)+",\"diagnostic_schema\":1";
  for (const auto key : {"commit","input_digest"}) { const auto* member = StringMember(*build,key); if (!member) return Fail("ParseDiagnosticBuild"); output += ",\"" + std::string(key) + "\":" + JsonString(HexShape(member->string,key == std::string_view("commit") ? 40 : 64) ? member->string : "redacted"); }
  for (const auto key : {"dirty","archive"}) { const auto* member = BooleanMember(*build,key); if (!member) return Fail("ParseDiagnosticBuild"); output += ",\"" + std::string(key) + "\":" + Primitive(*member); }
  output += "},\"configuration\":{\"log_level\":"+JsonString(log_level->string)+",\"runtime_observation\":"+std::string(runtime->boolean ? "true" : "false");
  const auto* logging = BooleanMember(*configuration,"file_logging"); if (!logging) return Fail("ParseDiagnosticConfiguration");
  output += ",\"file_logging\":" + Primitive(*logging);
  for (const auto key : {"max_file_mib","retained_files","sample_interval_ms","observation_seconds"}) {
    const auto* member = NumberMember(*configuration,key);
    const auto minimum = std::string_view(key) == "sample_interval_ms" ? 1000U : std::string_view(key) == "observation_seconds" ? 5U : 1U;
    const auto maximum = std::string_view(key) == "max_file_mib" ? 8U : std::string_view(key) == "retained_files" ? 10U : std::string_view(key) == "sample_interval_ms" ? 10000U : 120U;
    if (!member || member->number < minimum || member->number > maximum) return Fail("ParseDiagnosticConfiguration");
    output += ",\"" + std::string(key) + "\":" + Primitive(*member);
  }
  const auto* origins = ObjectMember(*configuration,"origins"); if (!origins) return Fail("ParseDiagnosticConfiguration");
  output += ",\"origins\":{";
  bool first_origin = true;
  for (const auto key : {"schema_version","logging.level","logging.file_logging","logging.max_file_mib","logging.retained_files","diagnostics.runtime_observation","diagnostics.sample_interval_ms","diagnostics.observation_seconds"}) {
    if (const auto* origin = Member(*origins,key)) {
      if (origin->type != JsonValue::Type::String || !IsChoice(origin->string, {"default","file","command_line"})) return Fail("ParseDiagnosticConfiguration");
      if (!first_origin) output += ',';
      first_origin = false;
      output += "\"" + std::string(key) + "\":" + JsonString(origin->string);
    }
  }
  output += "}},\"outcome\":" + JsonString(outcome->string) + ",\"elapsed_ms\":" + std::to_string(elapsed->number);
  for (const auto group : {"game","environment","dependencies"}) {
    const auto* parent = ObjectMember(root,group); if (!parent) return Fail("ParseDiagnosticObservation");
    output += ",\"" + std::string(group) + "\":{";
    const std::vector<std::string_view> keys = std::string_view(group) == "game" ? std::vector<std::string_view>{"package","version","executable_timestamp"} :
        std::string_view(group) == "environment" ? std::vector<std::string_view>{"os","gpu","driver","selected_adapter","hags_configured","hags_active"} :
        std::vector<std::string_view>{"interposer_version","interposer_sha256","interposer_signature_valid","loaded_plugin"};
    bool first = true;
    for (const auto key : keys) { std::string value; if (!ObservedJson(*parent,key,value)) return Fail("ParseDiagnosticObservation"); if (!first) output += ','; first = false; output += "\"" + std::string(key) + "\":" + value; }
    output += "}";
  }
  const auto* hook = StringMember(root,"hook_state"); const auto* fg = StringMember(root,"frame_generation");
  if (!hook || !fg || !IsChoice(hook->string, {"unknown","applied","routing_already_present","refused","rolled_back","startup_aborted"}) || !IsChoice(fg->string, {"unknown","activity_not_observed","activity_observed","rejected"})) return Fail("ParseDiagnosticState");
  output += ",\"hook_state\":" + JsonString(hook->string) + ",\"frame_generation\":" + JsonString(fg->string);
  const auto* rejection = Member(root,"decoded_rejection_code");
  if (!rejection || (fg->string == "rejected" ? rejection->type != JsonValue::Type::Number || rejection->number > UINT32_MAX : rejection->type != JsonValue::Type::Null)) return Fail("ParseDiagnosticRejection");
  output += ",\"decoded_rejection_code\":" + Primitive(*rejection);
  std::string failure; if (!FailureJson(root,failure)) return Fail("ParseDiagnosticFailure");
  output += ",\"failure\":" + failure;
  output += "}";
  return BundleEntry{"last-run.json",Encode(output)};
}

Result<BundleEntry> SanitizeEvents(std::string_view text, bool& truncated) {
  if (!text.empty() && text.back() != '\n') { truncated = true; const auto last = text.rfind('\n'); text = last == std::string_view::npos ? std::string_view{} : text.substr(0,last + 1); }
  std::vector<std::string> output;
  std::size_t begin{};
  while (begin < text.size()) {
    const auto end=text.find('\n',begin); const auto line=text.substr(begin,(end == std::string_view::npos ? text.size() : end)-begin); begin=end == std::string_view::npos ? text.size() : end+1;
    if (line.empty()) continue;
    if (line.size() > 4096) return Fail("DiagnosticEventTooLarge");
    JsonValue event; JsonParser parser(line);
    if (!parser.Parse(event)) return Fail("ParseDiagnosticEvents");
    const auto* level=StringMember(event,"level"); const auto* code=StringMember(event,"code"); const auto* stage=StringMember(event,"stage"); const auto* operation=StringMember(event,"operation"); const auto* fields=Member(event,"fields");
    if (const auto* schema = Member(event,"schema_version"); schema && (schema->type != JsonValue::Type::Number || schema->number != 1)) return Fail("ParseDiagnosticEvents");
    const auto* elapsed = NumberMember(event,"elapsed_ms");
    const auto* time = StringMember(event,"time_utc");
    if (!elapsed || !time || !level || !code || !stage || !operation || !fields || fields->type != JsonValue::Type::Array || fields->array.size() > 8 ||
        !IsChoice(level->string,{"error","info","debug","critical_warning"}) ||
        !(CodeValue(code->string) || IsChoice(code->string,{"INITIALIZATION","STAGE_COMPLETED","STATE_CHANGED","DEPENDENCY_OBSERVED","MUTATION_STEP"})) ||
        !IsChoice(stage->string,{"configuration","package_discovery","activation","process_identity","debug_attach","dependencies","build_validation","pre_factory_guard","import_transaction","detach","runtime_observation"}) || SafeToken(operation->string) != operation->string) return Fail("ParseDiagnosticEvents");
    std::string safe_fields = "[";
    bool first_field = true;
    for (const auto& field : fields->array) {
      const auto* key=StringMember(field,"key"); const auto* source=StringMember(field,"source"); const auto* value=Member(field,"value");
      if (!key || !source || !value) return Fail("ParseDiagnosticEvents");
      if (!IsChoice(key->string,{"package_version","executable_version","interposer_version","interposer_signature","import_count","factory_state","adapter_name","fg_activity","decoded_rejection_code","elapsed_ms"})) continue;
      if (!IsChoice(source->string,{"launcher","package_manifest","version_resource","authenticode","pe_inspection","factory_guard","dxgi_observation","runtime_observation"})) return Fail("ParseDiagnosticEvents");
      std::string safe_value;
      if (IsChoice(key->string, {"package_version","executable_version","interposer_version"})) {
        if (value->type != JsonValue::Type::String) return Fail("ParseDiagnosticEvents");
        safe_value = JsonString(Version(value->string) ? value->string : "redacted");
      } else if (IsChoice(key->string, {"import_count","decoded_rejection_code","elapsed_ms"})) {
        if (value->type != JsonValue::Type::Number) return Fail("ParseDiagnosticEvents");
        safe_value = Primitive(*value);
      } else if (IsChoice(key->string, {"interposer_signature","fg_activity"})) {
        if (value->type != JsonValue::Type::Boolean) return Fail("ParseDiagnosticEvents");
        safe_value = Primitive(*value);
      } else { safe_value = "\"redacted\""; }
      if (!first_field) safe_fields += ',';
      first_field = false;
      safe_fields += "{\"key\":" + JsonString(key->string) + ",\"source\":" + JsonString(source->string) + ",\"value\":" + safe_value + "}";
    }
    safe_fields += "]";
    std::string failure; if (!FailureJson(event,failure)) return Fail("ParseDiagnosticEventFailure");
    output.push_back("{\"level\":"+JsonString(level->string)+",\"code\":"+JsonString(code->string)+",\"stage\":"+JsonString(stage->string)+",\"operation\":"+JsonString(SafeOperation(operation->string))+",\"elapsed_ms\":" + Primitive(*elapsed) + ",\"time_utc\":\"" + SafeUtc(time->string) + "\",\"message\":" + JsonString(StableMessage(code->string)) + ",\"failure\":" + failure + ",\"fields\":" + safe_fields + "}");
  }
  if (output.size() > MaximumRecords) truncated = true;
  const auto first=output.size() > MaximumRecords ? output.size()-MaximumRecords : 0U;
  std::string sanitized="{\"schema_version\":1,\"events\":[";
  for (std::size_t index=first; index<output.size(); ++index) { if (index != first) sanitized += ','; sanitized += output[index]; }
  sanitized += "],\"truncated\":" + std::string(truncated ? "true" : "false") + "}";
  return BundleEntry{"events.json",Encode(sanitized)};
}

std::uint32_t Crc32(std::span<const std::byte> bytes) {
  std::uint32_t crc = 0xffffffffU;
  for (const std::byte byte : bytes) {
    crc ^= static_cast<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
  }
  return ~crc;
}

struct Sha256 {
  std::array<std::uint32_t, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::byte, 64> block{};
  std::size_t used{};
  std::uint64_t bits{};
  static constexpr std::array<std::uint32_t, 64> K{0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
  static std::uint32_t Rotate(std::uint32_t value, int amount) { return (value >> amount) | (value << (32 - amount)); }
  void Transform() {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) words[index] = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(block[index * 4])) << 24U) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(block[index * 4 + 1])) << 16U) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(block[index * 4 + 2])) << 8U) | static_cast<std::uint8_t>(block[index * 4 + 3]);
    for (std::size_t index = 16; index < words.size(); ++index) { const auto s0 = Rotate(words[index - 15], 7) ^ Rotate(words[index - 15], 18) ^ (words[index - 15] >> 3U); const auto s1 = Rotate(words[index - 2], 17) ^ Rotate(words[index - 2], 19) ^ (words[index - 2] >> 10U); words[index] = words[index - 16] + s0 + words[index - 7] + s1; }
    auto a=state[0], b=state[1], c=state[2], d=state[3], e=state[4], f=state[5], g=state[6], h=state[7];
    for (std::size_t index = 0; index < 64; ++index) { const auto s1=Rotate(e,6)^Rotate(e,11)^Rotate(e,25); const auto choice=(e&f)^(~e&g); const auto t1=h+s1+choice+K[index]+words[index]; const auto s0=Rotate(a,2)^Rotate(a,13)^Rotate(a,22); const auto majority=(a&b)^(a&c)^(b&c); const auto t2=s0+majority; h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2; }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
  }
  void Add(std::span<const std::byte> input) { for (const auto byte : input) { block[used++] = byte; bits += 8; if (used == block.size()) { Transform(); used = 0; } } }
  std::array<std::byte, 32> Finish() { block[used++] = std::byte{0x80}; if (used > 56) { while (used < 64) block[used++] = std::byte{}; Transform(); used=0; } while (used < 56) block[used++] = std::byte{}; for (int index=7; index>=0; --index) block[used++] = static_cast<std::byte>((bits >> (index*8)) & 0xffU); Transform(); std::array<std::byte,32> output{}; for (std::size_t index=0; index<8; ++index) for (std::size_t byte=0; byte<4; ++byte) output[index*4+byte]=static_cast<std::byte>((state[index] >> (24-byte*8)) & 0xffU); return output; }
};

std::string Hex(std::span<const std::byte> bytes) { static constexpr char Digits[] = "0123456789abcdef"; std::string result; result.reserve(bytes.size()*2); for (auto byte: bytes) { const auto value=static_cast<std::uint8_t>(byte); result.push_back(Digits[value>>4U]); result.push_back(Digits[value&15U]); } return result; }
std::string Hash(std::span<const std::byte> bytes) { Sha256 hash; hash.Add(bytes); return Hex(hash.Finish()); }

void U16(std::vector<std::byte>& target, std::uint16_t value) { target.push_back(static_cast<std::byte>(value & 0xffU)); target.push_back(static_cast<std::byte>((value >> 8U) & 0xffU)); }
void U32(std::vector<std::byte>& target, std::uint32_t value) { for (int index=0; index<4; ++index) target.push_back(static_cast<std::byte>((value >> (index*8)) & 0xffU)); }

Result<Unit> CheckEntries(std::span<const BundleEntry> entries) {
  if (entries.empty() || entries.size() > MaximumEntries) return Fail("ValidateSupportEntries");
  std::size_t total{};
  for (std::size_t index=0; index<entries.size(); ++index) {
    if (!IsChoice(entries[index].name, {"metadata.json","records.json","last-run.json","events.json","inventory.json","manifest.json"})) return Fail("UnexpectedSupportEntry");
    if (!IsSafeName(entries[index].name) || entries[index].name.size() > std::numeric_limits<std::uint16_t>::max() || entries[index].bytes.size() > std::numeric_limits<std::uint32_t>::max()) return Fail("ValidateSupportEntries");
    if (entries[index].bytes.size() > MaximumPayloadBytes - total) return Fail("ValidateSupportEntries");
    total += entries[index].bytes.size();
    for (std::size_t prior=0; prior<index; ++prior) if (entries[prior].name == entries[index].name) return Fail("ValidateSupportEntries");
  }
  return Unit{};
}

Result<Unit> WriteStoreArchive(std::span<const BundleEntry> entries, ISupportStorage& storage) {
  if (!IsOk(CheckEntries(entries))) return Fail("WriteStoreArchive");
  struct Central { std::string_view name; std::uint32_t crc; std::uint32_t size; std::uint32_t offset; };
  std::vector<std::byte> archive;
  std::vector<Central> central;
  for (const auto& entry : entries) {
    if (archive.size() > std::numeric_limits<std::uint32_t>::max()) return Fail("WriteStoreArchive");
    const auto crc=Crc32(entry.bytes); const auto size=static_cast<std::uint32_t>(entry.bytes.size()); const auto offset=static_cast<std::uint32_t>(archive.size());
    U32(archive,0x04034b50U); U16(archive,20); U16(archive,0); U16(archive,0); U16(archive,0); U16(archive,0); U32(archive,crc); U32(archive,size); U32(archive,size); U16(archive,static_cast<std::uint16_t>(entry.name.size())); U16(archive,0);
    archive.insert(archive.end(), reinterpret_cast<const std::byte*>(entry.name.data()), reinterpret_cast<const std::byte*>(entry.name.data()+entry.name.size())); archive.insert(archive.end(),entry.bytes.begin(),entry.bytes.end()); central.push_back({entry.name,crc,size,offset});
  }
  const auto central_offset=static_cast<std::uint32_t>(archive.size());
  for (const auto& item: central) { U32(archive,0x02014b50U); U16(archive,20); U16(archive,20); U16(archive,0); U16(archive,0); U16(archive,0); U16(archive,0); U32(archive,item.crc); U32(archive,item.size); U32(archive,item.size); U16(archive,static_cast<std::uint16_t>(item.name.size())); U16(archive,0); U16(archive,0); U16(archive,0); U16(archive,0); U32(archive,0); U32(archive,item.offset); archive.insert(archive.end(),reinterpret_cast<const std::byte*>(item.name.data()),reinterpret_cast<const std::byte*>(item.name.data()+item.name.size())); }
  const auto central_size=static_cast<std::uint32_t>(archive.size())-central_offset;
  U32(archive,0x06054b50U); U16(archive,0); U16(archive,0); U16(archive,static_cast<std::uint16_t>(central.size())); U16(archive,static_cast<std::uint16_t>(central.size())); U32(archive,central_size); U32(archive,central_offset); U16(archive,0);
  const auto committed = storage.CommitArchiveAtomically(archive);
  if (!IsOk(committed)) return Failure(committed);
  return Unit{};
}

Result<BundleEntry> ReadSummary(ISupportStorage& storage) {
  const auto handle=storage.OpenSanitizedSummary();
  if (!IsOk(handle)) return StorageReadFailure(Failure(handle),"OpenSanitizedSummary");
  if (Value(handle).identity == 0 && !Value(handle).regular && !Value(handle).reparse) return BundleEntry{};
  if (!Value(handle).regular || Value(handle).reparse) return Fail("OpenSanitizedSummary");
  const auto read=storage.ReadSanitizedSummary(Value(handle),MaximumSummaryBytes);
  if (!IsOk(read)) return StorageReadFailure(Failure(read),"ReadSanitizedSummary");
  if (!Value(read).regular || Value(read).reparse || Value(read).identity != Value(handle).identity || Value(read).bytes.size() > MaximumSummaryBytes) return Fail("ReadSanitizedSummary");
  return SanitizeSummary(std::string_view(reinterpret_cast<const char*>(Value(read).bytes.data()),Value(read).bytes.size()));
}
}

Result<std::vector<BundleEntry>> BuildSupportPayload(const SupportInputs& inputs) {
  if (inputs.schema_version != SchemaVersion || inputs.metadata.schema_version != SchemaVersion) return Fail("BuildSupportPayload");
  for (const auto& run : inputs.runs) {
    if (static_cast<unsigned>(run.outcome) > static_cast<unsigned>(SupportRunOutcome::Failed) ||
        static_cast<unsigned>(run.stage) > static_cast<unsigned>(Stage::runtime_observation) ||
        static_cast<unsigned>(run.error) > static_cast<unsigned>(ErrorCode::SupportBundleFailure)) return Fail("ValidateSupportRun");
  }
  for (const auto& event : inputs.events) {
    if (static_cast<unsigned>(event.stage) > static_cast<unsigned>(Stage::runtime_observation) ||
        static_cast<unsigned>(event.error) > static_cast<unsigned>(ErrorCode::SupportBundleFailure)) return Fail("ValidateSupportEvent");
  }
  std::vector<BundleEntry> entries;
  entries.push_back({"metadata.json",Encode("{\"schema_version\":1,\"build_id\":\"" + std::string(HexShape(inputs.metadata.build_id,40) || HexShape(inputs.metadata.build_id,64) ? inputs.metadata.build_id : "redacted") + "\",\"package_version\":\"" + std::string(Version(inputs.metadata.package_version) ? inputs.metadata.package_version : "redacted") + "\",\"runtime_observation\":" + (inputs.metadata.runtime_observation_enabled ? "true" : "false") + "}")});
  std::string records="{\"schema\":1,\"runs\":["; std::size_t included{};
  const auto append=[&](std::string_view value) { if (included++ != 0) records += ','; records += value; };
  for (const auto& run: inputs.runs) { if (run.schema_version != SchemaVersion) return Fail("BuildSupportPayload"); if (included<MaximumRecords) append("{\"outcome\":\""+std::string(OutcomeName(run.outcome))+"\",\"stage\":\""+StageName(run.stage)+"\",\"error\":\""+ErrorName(run.error)+"\",\"operation\":\""+std::string(SafeOperation(run.operation.view()))+"\",\"elapsed_ms\":"+std::to_string(run.elapsed_ms)+"}"); }
  for (const auto& event: inputs.events) { if (event.schema_version != SchemaVersion) return Fail("BuildSupportPayload"); if (included<MaximumRecords) append("{\"stage\":\""+std::string(StageName(event.stage))+"\",\"error\":\""+ErrorName(event.error)+"\",\"operation\":\""+std::string(SafeOperation(event.operation.view()))+"\",\"sequence\":"+std::to_string(event.sequence)+"}"); }
  records += "],\"truncated\":" + std::string(inputs.runs.size()+inputs.events.size()>MaximumRecords ? "true" : "false") + "}";
  entries.push_back({"records.json",Encode(records)});
  return entries;
}

Result<SupportBundle> CollectSupport(const SupportInputs& inputs, ISupportStorage& storage) {
  auto payload=BuildSupportPayload(inputs); if (!IsOk(payload)) return Failure(payload);
  auto entries=std::get<std::vector<BundleEntry>>(std::move(payload));
  auto summary=ReadSummary(storage); if (!IsOk(summary)) return Failure(summary);
  if (!Value(summary).name.empty()) entries.push_back(std::get<BundleEntry>(std::move(summary)));
  bool truncated = inputs.runs.size() > MaximumRecords || inputs.events.size() > MaximumRecords - std::min(MaximumRecords,inputs.runs.size());
  const auto event_handle=storage.OpenRecentEvents();
  if (!IsOk(event_handle)) return StorageReadFailure(Failure(event_handle),"OpenRecentEvents");
  if (Value(event_handle).identity != 0 || Value(event_handle).regular || Value(event_handle).reparse) {
    if (!Value(event_handle).regular || Value(event_handle).reparse) return Fail("OpenRecentEvents");
    const auto event_read=storage.ReadRecentEvents(Value(event_handle), MaximumEventLogBytes);
    if (!IsOk(event_read)) return StorageReadFailure(Failure(event_read),"ReadRecentEvents");
    if (!Value(event_read).regular || Value(event_read).reparse || Value(event_read).identity != Value(event_handle).identity) return Fail("ReadRecentEvents");
    truncated = truncated || Value(event_read).truncated;
    const auto& bytes = Value(event_read).bytes;
    std::string_view text(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    if (text.size() > MaximumEventLogBytes) { truncated = true; text.remove_prefix(text.size() - MaximumEventLogBytes); const auto newline = text.find('\n'); text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1); }
    const auto events=SanitizeEvents(text,truncated);
    if (!IsOk(events)) return Failure(events);
    entries.push_back(Value(events));
  }
  entries.push_back({"inventory.json",Encode("{\"schema_version\":1,\"truncated\":" + std::string(truncated ? "true" : "false") + ",\"exclusions\":[\"raw_config\",\"raw_environment\",\"evidence\",\"binaries\",\"saves\",\"dumps\",\"profiles\",\"uploads\",\"unknown_fields\",\"free_text\"]}")});
  std::string manifest="{\"schema_version\":1,\"truncated\":" + std::string(truncated ? "true" : "false") + ",\"entries\":["; std::vector<std::byte> aggregate;
  for (std::size_t index=0; index<entries.size(); ++index) { const auto& entry=entries[index]; if (index) manifest += ','; manifest += "{\"name\":\""+entry.name+"\",\"sha256\":\""+Hash(entry.bytes)+"\",\"bytes\":"+std::to_string(entry.bytes.size())+"}"; aggregate.insert(aggregate.end(),entry.bytes.begin(),entry.bytes.end()); }
  const auto payload_hash=Hash(aggregate); manifest += "],\"payload_sha256\":\""+payload_hash+"\"}"; entries.push_back({"manifest.json",Encode(manifest)});
  const auto written = WriteStoreArchive(entries,storage);
  if (!IsOk(written)) return Failure(written);
  SupportBundle bundle; bundle.payload_sha256=payload_hash; bundle.truncated=truncated; bundle.exclusions={"raw_config","raw_environment","evidence","binaries","saves","dumps","profiles","uploads","unknown_fields","free_text"};
  for (const auto& entry: entries) if (entry.name!="manifest.json") bundle.entries.push_back({entry.name,Hash(entry.bytes),entry.bytes.size()});
  bundle.local_output=storage.CompletedArchiveLocation();
  if (bundle.local_output.empty()) return Fail("CollectSupport");
  return bundle;
}

#ifdef WFG_SUPPORT_TESTING
Result<Unit> ValidateSupportArchiveEntriesForTest(std::span<const BundleEntry> entries) { return CheckEntries(entries); }
#endif

}  // namespace wfg
