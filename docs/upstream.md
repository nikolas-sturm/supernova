# Upstream Maintenance

## Preserved History

The applications entered this monorepo through **unsquashed subtree imports**:

| Source | Imported revision | Monorepo prefix |
| --- | --- | --- |
| Progenitor (now Sol) | `74273db` | `apps/sol/` |
| Terra | `3333223` | `apps/terra/` |

Original commits and ancestry remain available, and the original source
repositories remain preserved. Do not squash or rewrite imported history merely
to simplify the monorepo. Third-party recursive submodules are distinct from
these first-party subtree imports; their recorded gitlinks and `.gitmodules`
paths remain the dependency source of truth.

The host's original import prefix was `apps/progenitor/`; its current prefix is
`apps/sol/`. Submodule section names preserve the original `progenitor/` identity,
while their paths follow the current prefix. This is a working-tree relocation,
not a rewrite of the original import commit or source checkout.

## Selective Updates

Bring in reviewed fixes selectively rather than merging an upstream root tree
over the monorepo. Fetching a source repository does not itself make its paths
match `apps/sol/` or `apps/terra/`.

1. Identify the source repository, full commit SHA, parent/base, license, and
   prerequisites. Inspect the complete diff, including renames, binary files,
   submodules, and any unrelated changes.
2. Map source-root paths to the appropriate monorepo prefix. For already
   prefix-adjusted commits with matching paths, a reviewed cherry-pick can be
   appropriate; retain provenance with `-x` when committing is authorized.
3. For source-root commits, prepare a patch from the selected parent-to-commit
   diff and apply it under the app prefix, or use a deliberately reviewed
   path-prefix-aware cherry-pick strategy. Check path mapping first; a plain
   cherry-pick is not automatically safe for relocated trees. Merge commits need
   an explicitly chosen parent and usually a manual port.
4. Resolve against current first-party code. Exclude unrelated upstream packaging,
   tooling, platform, or dependency changes unless deliberately selected. Inspect
   the resulting diff for unexpected root files or changes outside the app.
5. Record the original repository URL, full SHA, original subject, dependencies,
   and adaptations in the resulting commit message or maintenance record. For a
   manual port, state that it is a port, not an unchanged cherry-pick.
6. Run the relevant frontend, native, and streaming regression checks when
   authorized and dependencies are available. Record blockers and unrun checks
   honestly; then commit only when explicitly requested.

There is no automatic `git merge` recipe here: common ancestry alone does not
establish correct path mapping or semantic compatibility. Do not merge a whole
upstream branch simply because the initial imports were unsquashed. Never
force-push or rewrite the preserved source repositories to accommodate a port.

Suggested provenance fields for a manually adapted change:

```text
Upstream-Repository: <source URL>
Upstream-Commit: <full original SHA>
Upstream-Subject: <original subject>
Ported-To: apps/<app>/
Adaptations: <path mapping and semantic differences>
Validation: <checks performed, results, and blockers>
```

## Moonlight Boundary

Terra tracks `apps/terra/refs/moonlight-qt` as a pinned submodule. That revision
selects its nested `moonlight-common-c` transport revision and related upstream
dependencies. Review a specific Qt revision and its recursive pin changes before
advancing the outer gitlink; do not independently advance the nested common
transport or blindly track an upstream branch tip.

Common transport fixes can become available through the reviewed Moonlight Qt
pin update because Terra builds that nested C transport. **Qt application fixes
do not automatically reach Terra**: Qt UI, session orchestration, rendering,
platform integration, and application behavior must be assessed and ported into
Terra's Neutralino/C++ implementation where relevant. A pin update is not proof
that equivalent application behavior exists or works.

Sol has a separate `apps/sol/third-party/moonlight-common-c` pin.
Do not assume the host and client dependency pointers move together. Preserve
upstream standards, checksums, licenses, and unsupported-platform source while
reviewing any pin update. None of that expands Supernova's Windows/Linux support
or CI matrix.
