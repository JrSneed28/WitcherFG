# Interpret diagnostics and collect support

Import routing, requested settings and actual frame generation are different observations. `Applied` and `RoutingAlreadyPresent` confirm only guarded routing. Missing SDK/adapter/swap-chain fields remain Unknown, with source, availability and reason recorded.

## What can be observed

Automatic runtime observation is Off by default. Enabling it requests a bounded read-only interval after detach. `WitcherFG.exe --diagnose` explicitly observes an eligible existing registered game without activation, debugger attachment or memory writes.

The production SDK-layout catalog is deliberately empty: no plugin-internal offsets or rejection meanings have been independently approved. Dependency inventory, approved import routing and available environment facts can be recorded. SDK frame/activity/rejection fields remain unavailable. Native sampling currently caches the initial metadata capture; it is not a live SDK frame counter.

Availability values are `observed`, `not_observed`, `unsupported_layout` and `read_failed`. Unknown is neither success nor rejection. Configured HAGS is sourced separately from active HAGS; an enumerated NVIDIA GPU does not prove the selected game adapter. Loaded plugins, a menu toggle, an SDK zero status or an FPS ratio do not prove generated frames. Standard FG and Multi Frame Generation (MFG) require separate gameplay validation.

## Records and storage

Schema 1 records include effective config/origins, launcher build identity, available game/dependency/environment facts, routing/outcome, stage/operation, monotonic elapsed milliseconds and UTC completion time. Failures include their stable code, UTC time, native code where available and next action. Completion UTC may be `unknown` if unavailable; older support records without the additive field remain accepted.

When file logging is enabled, `%LOCALAPPDATA%\WitcherFG\diagnostics` holds `last-run.json`, `events.jsonl` and bounded rotated event files. Logging failure falls back to the console. Console control handling requests cancellation; normal cleanup remains bounded.

Storage pins fixed physical local volumes and rejects unsafe directory/file shapes. Network/removable/SUBST/reparse locations can refuse even when Windows can browse them. `Local\` directory mutexes protect same-session writers; they do not provide a cross-session exclusion guarantee. An existing retained mutex object refuses, including an abandoned object still retained by another handle. Close other instances/retaining processes before retrying. These restrictions trade storage convenience for guarded file access.

## Normal returned exit codes

| Exit code | Returned status |
| --- | --- |
| 0 | `Applied`, `RoutingAlreadyPresent`, `ReadOnlyCompleted` |
| 2 | `AlreadyRunning`, `UnsupportedBuild`, `UnexpectedImports`, `UntrustedInterposer`, `StartupTooLate`, `AccessDenied`, `ConfigurationInvalid`, `DllConflict` |
| 3 | `RolledBack`, `LaunchFailed`, `TimedOut`, `SupportFailed` |
| 4 | `StartupAborted` |

These codes describe returned `RunResult` statuses. `RolledBack` means restoration was verified; `StartupAborted` is returned only after owned target exit was observed. A returned success does not prove gameplay FG.

The watchdog can force helper termination with Windows code **1460**, falling back to **124**. The immediate unsafe terminal path exits the helper with **3**, bypassing `RunResult`, formatting and ordinary cleanup. Forced helper codes do not prove target exit or detach. Code 3 alone cannot distinguish a returned failure from immediate helper death; consult retained diagnostics and independently check the process state. Initialization failures can also return 3 before a result exists.

The ordinary startup has a 30-second discovery deadline and 60-second total work deadline; at most two additional seconds are reserved for cancellation cleanup. Observation after detach has a separate configured deadline. Detailed synthetic native lifecycle evidence is in [Windows fixture results](validation/windows-fixture-results.md).

## Explicit support collection

Run from the utility folder:

```powershell
.\WitcherFG.exe --collect-support
```

Success reports that a local archive was created. Look in `%LOCALAPPDATA%\WitcherFG\support` for `WitcherFG-support-*.zip`. Collection preserves the previous launch summary, does not launch/patch the game and never uploads anything.

The ZIP contains schema-whitelisted `metadata.json`, `records.json`, `inventory.json`, `manifest.json` and, when available, sanitized `last-run.json`/`events.json`. Current native collection does not populate a historical five-run list or arbitrary environment text. Payloads are capped at 10 MiB and 32 files; included payload hashes and truncation state are recorded. Unknown fields and free text are discarded. Raw configs, personal paths, binaries, saves, dumps, profiles and unrelated files are excluded.

Review the extracted JSON before public attachment. Keep a copy of the original archive and its SHA256 if submitting a report. The collector is a sanitization aid; review remains the user's responsibility. A private security-reporting route and ordinary hosted bug form are currently unavailable. See [SECURITY.md](../SECURITY.md) and [troubleshooting](troubleshooting.md).
