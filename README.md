# WitcherFG investigation

The tested Xbox PC Witcher 3 build bypassed Streamline's NVIDIA presentation path. Temporary startup redirection restored FG/MFG on an RTX 5070 Ti, with normal rendering recovered through a ray tracing off/on cycle. The game has been closed and all temporary process changes are gone.

- [Launcher specification](docs/superpowers/specs/2026-10-10-witcherfg-launcher-design.md): proposed design, awaiting review.
- [Implementation plan](docs/superpowers/plans/2026-10-10-witcherfg-launcher.md): proposed tasks and validation gates, awaiting approval before execution.
- [Publisher bug report](docs/bug-reports/2026-10-10-witcher3-xbox-frame-generation.md): confirmed observations, controlled tests, likely source-level cause, and requested publisher action.
- [Evidence ZIP](artifacts/witcher3-xbox-fg-report-2026-10-10.zip): shareable report and selected evidence, with personal paths removed and payload SHA256 checksums.
- `evidence/session-findings.json`: complete local investigation history. Raw evidence remains local and is excluded from Git.

A reusable launcher has not been implemented. The diagnostic scripts are throwaway probes. Compatibility has been demonstrated on one GPU, driver, and Xbox executable build; broader coverage remains to be validated.
