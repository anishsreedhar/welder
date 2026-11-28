# PR #6 regression fixes

Welder remains **0.2.6 / PFX Build 001**. This update includes the latest `main` while preserving per-render Write Python permission and the implicit alpha mask migration for saved Blur nodes.

| Report | Corrected behavior | Regression coverage |
| --- | --- | --- |
| Save As deletes its own destination when it matches the current autosave | Complete outstanding autosave writes before saving. Cleanup checks filesystem identity and canonical paths, preserves the destination, and runs only after a successful write. | Save the current comp to its autosave filename in the real editor smoke test; verify contents survive. Shared save-transaction tests cover an exact path, a lexical alias, removal of other recovery files, and preservation of recovery after a failed save. |
| Older Keyer mattes change with the new response | Saved Keyers without a response marker retain smooth ramps, original luminance weights and the upper cutoff. New nodes and Nuke imports explicitly select the Nuke response. The marker persists through reopening and duplication. | Red input 2.0 gives legacy alpha 0 and new alpha 1; input 0.25 gives legacy 0.15625 and new 0.25, both before and after reopening. Duplicate an old node and check its matte. Import tests verify the new response marker. |
| A frame-server worker loses the editor's path mappings | Write a snapshot of active mappings beside the temporary worker comp and pass that file to every worker through `--path-mappings`. Read it before loading the comp. Mappings are invocation context, not executable permissions or settings embedded in an artist's comp. | Start an actual CLI worker with a mapped foreign plate/output path and verify its rendered RGBA pixels match the editor-side render. Clear parent mapping globals before starting it. |
| Cross-platform separators or trailing slashes break mapped paths | Compare normalized component prefixes, normalize suffix separators and join the destination with exactly one separator. Prefer the longest mapping. Windows drive/UNC prefixes match without case sensitivity; POSIX prefixes retain it. | Windows-to-Linux and Linux-to-Windows mappings, mixed separators, source/destination trailing slash combinations, root mappings, exact prefixes and nonmatching partial folder names. |

`--path-mappings mappings.json` accepts a JSON array of `[source_prefix, destination_prefix]` pairs. The editor creates this automatically for background renders; other CLI callers can supply their own invocation file. This does not enable Write Python callbacks: those still require `--allow-write-python` for the individual render.

The PR remains open for review; these changes do not merge it into `main`.
