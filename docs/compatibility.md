# Compatibility and evidence scope

The canonical records are [compatibility.toml](compatibility.toml). A **Verified: diagnostic probe** row refers to the earlier temporary probe, not the native launcher or a player ZIP. Native release acceptance is Pending for every row below. [Native Windows fixtures](validation/windows-fixture-results.md) prove synthetic lifecycle behavior separately.

| Component | Evidence status | Native release status | Details |
| --- | --- | --- | --- |
| Xbox PC executable, menu 5.01 / package 5.0.4.0 | Verified: diagnostic probe | Pending | Measured fingerprints and temporary four-import redirection. |
| NVIDIA RTX 5070 Ti | Verified: diagnostic probe | Pending | Local Dynamic FG probe; visible world and smoother motion confirmed after manual RT reset. |
| NVIDIA driver 617.42 | Verified: diagnostic probe | Pending | Local probe used the original NVIDIA profile. |
| Windows 11 Pro build 26200, 2560 × 1440 | Verified: diagnostic probe | Pending | Measured local environment; other resolutions/window modes are untested. |
| Native WitcherFG launcher or installer ZIP | Pending | Pending | Native code and synthetic lifecycle tests exist; packaged game/player acceptance remains Pending. |
| Other NVIDIA GPUs | Untested | Pending | No extrapolation from the RTX 5070 Ti probe. |
| AMD GPUs | Untested | Pending | No support claim; no AMD FG backend is supplied. |
| Intel GPUs | Untested | Pending | No support claim; no Intel FG backend is supplied. |
| Steam executable | Untested | Pending | Initial compiled Xbox eligibility does not authorize this store/build. |
| Epic/GOG executables | Untested | Pending | Initial compiled Xbox eligibility does not authorize these stores/builds. |
| Other NVIDIA driver versions | Untested | Pending | Not independently verified. |
| Other upscaler/FG mods, Vortex, other mod managers | Untested | Pending | No named/versioned coexistence or manager test exists. |

Probe evidence date: 2026-10-10. The local environment was Xbox menu 5.01/package 5.0.4.0, Direct3D 12, RTX 5070 Ti, NVIDIA 617.42, Windows 11 Pro build 26200 and 2560 × 1440. Window mode, selected-adapter runtime state and active HAGS are not independently established by this matrix. The original NVIDIA profile was retained.

Standard DLSS Frame Generation and Multi Frame Generation are separate modes. The local Dynamic FG observation must not be generalized to RTX 40-series standard FG, other RTX 50-series configurations, maximum generation counts or other drivers. Configured HAGS is not proof of active HAGS. FPS ratios, a menu toggle, loaded plugins or compilation cannot verify generated frames.

Statuses are Verified, Untested, Reported, Pending and Unsupported. Reported means an external observation awaiting reproduction. Unsupported identifies a documented restriction; a guard refusal is not a reproduced gameplay failure. The initial production guard authorizes only the compiled Xbox identity even where another store is marked Untested.

Every Verified row needs a method, date and game identity. A Verified native release also needs the tested ZIP SHA256 and build ID; probe/fixture evidence cannot set native release status Verified. New native/gameplay records must state their exact scope and artifact rather than inherit this probe's outcome.
