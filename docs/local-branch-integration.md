# Local Blur, Keyer and workspace integration

Integrated into the existing Welder 0.2.6 / PFX Build 001 application:

| Branch | Source commit |
| --- | --- |
| blur-controls-update | b50970842eecce1ced7989e331bb939579fcc731 |
| keyer-updates | 49b2e8f9edb3bc8a0ab2ef17ad5fc8b7e55dc7ac |
| workspace-animation-updates | a93c04ab0132ae2966a28e18440b8b77892e45f8 |

The local integration preserves the existing Write, Grade, ColorCorrect, compact timeline and package launcher. Shared Properties conflicts were resolved by retaining both sets of node controls. Verdana reference text now requests its font through the incoming lazy font loader.

## Validation

- Windows editor, CLI and engine tests built successfully.
- Engine suite: 45,333 checks, zero failures, including Keyer, selected Merge matte and missing source-mask regressions.
- Blur suite: 18,509 checks passed.
- Installed fonts: 260 families / 170 unique faces validated.
- Nuke importer: 69 tests passed, one skipped.
- Commit-range secrets scan and whitespace checks passed.
- Editor screenshots checked Blur, Keyer, Write, Backdrop, Curve Editor, Dope Sheet, Viewer and Node Graph.
- A scripted Curve Editor drag moved a key from frame 25/value 40 to frame 27/value approximately 35.09, then saved it correctly.

## GitHub status

At inspection, the three source branch heads had no GitHub checks, commit statuses or Actions runs. Blur had open PR #7; the other two branches had no PR. Keyer and workspace source commit emails do not satisfy the repository commit-email policy. Local integration uses compliant squash commits with source references, without rewriting contributor branches.

This validation describes the combined local Windows build. It does not establish passing cross-platform GitHub CI for the original branch heads. No GitHub PR was merged; PR #6 remains untouched.

## Latest GitHub main refresh (2026-11-28)

Integrated `origin/main` at `73f05aa82c5103e6908e7e2ad1b3030e73daffe7` into the existing local branch, preserving Preferences, native text clarity, Keyer, workspace and node-interface customizations. This local merge does not publish changes or merge a GitHub PR.

The refresh adds legacy Blur mask compatibility (a connected mask without an explicit channel retains implicit alpha) and per-render permission for Write Python callbacks, including the editor checkbox and CLI `--allow-write-python` option.

Validation: Windows editor and CLI built; 45,334 engine checks passed; 18,510 Blur checks passed; 22 Preferences checks passed; 3,529 native font-render checks passed; importer suite passed 69 tests with one skip. A CLI callback was blocked before output without opt-in and successfully rendered with explicit opt-in. The updated render dialog was visually checked. Both installed executables matched their build SHA256 hashes, and the installed editor smoke test passed open/render, playback, editing, painting and autosave recovery. The existing application name, launcher and 0.2.6 / PFX Build 001 version remain unchanged.
