# Welder development preferences

- Keep this project at Welder **0.2.6 / PFX Build 001** for all requested changes.
- Do not increase the application version, package version, or build number unless the user explicitly requests it.
- Keep all shared source changes in this repository. The existing launcher is `Welder.bat` and its local application package is `dist\Welder`.
- For application changes, rebuild and verify the Windows editor and refresh that local package so the launcher runs the updated software. Detect machine resources at runtime; do not commit personal absolute paths or local build tools.
