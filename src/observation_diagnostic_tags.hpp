#pragma once
#include <array>
#include <string_view>
#include <utility>
namespace wfg::diagnostic_detail {
inline std::string_view ObservationSource(std::string_view value) {
  for(const auto known:{"read_only_runtime","validated_imports","pinned_process_inventory","runtime_observation","factory_guard"})
    if(value==known)return known;
  return "unknown";
}
inline std::string_view ObservationReason(std::string_view value) {
  // Fixed vocabulary preserves meaning without persisting provider-supplied text.
  constexpr std::array<std::pair<std::string_view,std::string_view>,19> reasons{{
    {"No independently validated SDK diagnostic layout.","diagnostic_layout_unavailable"},
    {"Read-only process access unavailable.","read_only_access_unavailable"},
    {"Pinned process inventory unavailable.","inventory_unavailable"},
    {"Pinned inventory collection was not completed.","inventory_not_collected"},
    {"No runtime sample captured.","not_sampled"},
    {"Available observation lacked its source.","source_unavailable"},
    {"Observed field was missing its value.","value_unavailable"},
    {"Field not exposed by a validated observation.","field_unavailable"},
    {"Pinned process exited or identity changed; prior values discarded.","process_lost"},
    {"Pinned process exited or identity changed.","process_lost"},
    {"Import routing was not checked.","routing_not_checked"},
    {"Import routing was not checked because pinned inventory was unavailable.","inventory_unavailable"},
    {"Import routing skipped because pinned inventory was unavailable.","inventory_unavailable"},
    {"Import routing skipped because read-only process access was unavailable.","read_only_access_unavailable"},
    {"Approved build/import guards did not validate.","routing_unvalidated"},
    {"Initial approved build import snapshot.","initial_snapshot"},
    {"Initial read-only dependency snapshot.","initial_snapshot"},
    {"", "none"},
    {"Observation target could not be opened.","target_unavailable"}
  }};
  for(const auto& [message,code]:reasons)if(value==message||value==code)return code;
  return "redacted";
}
}
