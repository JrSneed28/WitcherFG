# Configure WitcherFG

The optional `FrameGenFix.toml` is read beside `WitcherFG.exe`, independently of the shell's current directory. Start with [the example](../config/FrameGenFix.toml). A missing file uses defaults.

## Supported commands

Run these from the executable's folder in PowerShell:

| Command | Behavior |
| --- | --- |
| `.\WitcherFG.exe` | Activate the registered Xbox game and attempt guarded session routing |
| `.\WitcherFG.exe --help` | Show usage without activation |
| `.\WitcherFG.exe --version` | Show version, build configuration, source commit and input digest |
| `.\WitcherFG.exe --diagnose` | Read an eligible existing registered process; no activation or debugger attach |
| `.\WitcherFG.exe --collect-support` | Create a local sanitized support ZIP; no game activation or patching |
| `.\WitcherFG.exe --game-path "D:\XboxGames\The Witcher 3\Content"` | Supply an ephemeral installation-location hint for a guarded launch |
| `.\WitcherFG.exe --diagnose --game-path "D:\XboxGames\The Witcher 3\Content\bin\Gaming.Desktop.x64\witcher3.exe"` | Use the hint for read-only diagnosis |
| `.\WitcherFG.exe --log-level error` | Override verbosity; `info` and `debug` are also allowed |

Choose at most one explicit mode. Unknown options, duplicate modes/verbosity, missing verbosity values and invalid values refuse with `ConfigurationInvalid`. The verbosity override can accompany a mode; it changes only logging.

The path may name `witcher3.exe`, its containing folder, `bin`, or the registered game root. Windows Unicode paths are supported. The supplied location must match the current user's registered installation by Windows file handles and package documents. Copies, unrelated folders, missing targets, ambiguous registrations and mismatched package identities refuse. A hint grants no permission to skip any guard or start an executable directly.

The path option works only for launch and read-only diagnosis. Duplicates, missing/empty/oversized values, invalid UTF-8 and combinations with help, version or support collection refuse before discovery. It is never written to TOML, diagnostics or support archives; provide it again on a later run. A real console may offer entry only after a pathname failure that an independently validated canonical representation can resolve. Ordinary folder hints work for long-path recovery; missing or invalid mandatory assets refuse without entry. Redirected input/output refuses immediately and prints the CLI retry syntax. Empty input, Escape, cancellation or the overall deadline refuses without activation.

## Schema 1 settings

| Key | Default | Accepted value |
| --- | --- | --- |
| `schema_version` | `1` | Integer `1`; required in a present file |
| `logging.level` | `"error"` | `"error"`, `"info"`, `"debug"` |
| `logging.file_logging` | `true` | Boolean |
| `logging.max_file_mib` | `2` | Integer 1–8; event-file rotation size in MiB |
| `logging.retained_files` | `5` | Integer 1–10; retained event-file count |
| `diagnostics.runtime_observation` | `false` | Boolean; automatic observation after successful detach |
| `diagnostics.sample_interval_ms` | `1000` | Integer 1000–10000 |
| `diagnostics.observation_seconds` | `30` | Integer 5–120; separate observation duration |

Precedence is defaults → valid executable-relative file → command-line verbosity. Origins are recorded with the effective configuration. `--diagnose` explicitly requests observation even when automatic post-launch observation is Off.

Malformed TOML, duplicate keys, unknown keys, wrong types, unsupported schema, out-of-range values and files over 64 KiB refuse before activation. A present file must be regular and single-link in a pinned local directory. Fixed physical-volume and reparse restrictions can also refuse configuration access. Moving the utility to an ordinary local directory can resolve a storage restriction; disabling file logging does not bypass safe configuration loading.

Configuration cannot authorize memory writes, add compatibility records, load arbitrary DLLs, skip guards, change driver profiles or modify the game. Preserve your copy during updates; automatic migration is not implemented.

## Logging cost and availability

Error retains failures and critical warnings. Info adds initialization, stage completion and state changes; Debug adds stage details. More verbosity uses additional bounded disk space and processing. File logging maintains a compact `last-run.json` independent of event verbosity and rotated JSONL events. It does not keep five historical run summaries: the last summary is replaced.

Failed file logging falls back to the console; retain that result. Directory ownership may refuse while another same-session instance holds the storage mutex. Enabling observation adds a separately bounded read-only interval after detach, but the production SDK-layout catalog is empty. It cannot currently reveal real SDK generation activity. See [diagnostics](diagnostics.md).
