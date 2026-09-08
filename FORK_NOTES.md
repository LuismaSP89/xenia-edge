# Fork notes: Forgottenshadow89/xenia-edge

This fork tracks [has207/xenia-edge](https://github.com/has207/xenia-edge) and
carries a small number of changes from Xenia Canary that upstream edge has not
integrated yet. The `edge` branch (default) is always upstream `edge` plus the
fork's commits rebased on top, never merged, so `git log origin/edge..edge`
shows exactly what the fork adds.

## Fork commits

**[Kernel] Select the most compatible and newest title update**

Port of xenia-canary PR [#1201](https://github.com/xenia-canary/xenia-canary/pull/1201)
("Always select most compatible and newest TU", commit `ea18aacb40`). When a
title has several title updates installed, edge used to apply whichever one
was listed first. Now the updates whose title id and media id match the running
executable are kept (a media id mismatch is tolerated only with
`allow_incompatible_title_update = true`) and the highest version wins. This
also stops a TU meant for one "sub" game of a compilation from being applied to
another.

Canary reads the media id and installer version from the package container
header. Edge stores extracted packages with a minimal header, so this port
reads the patch XEX's delta patch descriptor instead, which every title update
carries whether it is a container or an extracted package: its source digest is
the hash of the base XEX signature (the same check edge applies before patching)
and its target version is the update's version. Canary's content-list dialog
change (showing the TU version) has no equivalent in edge and is not ported.

**[Fork] Publish releases from this repository**

`.github/workflows/CI.yml`: the `Create Release` job also runs for this
repository and when the workflow is started manually (pushes alone do not
trigger CI on the fork). Adds this file and `sync-upstream.cmd`.

## Updating to the latest upstream edge

Run `sync-upstream.cmd` from the repository root. It fetches `origin/edge`
(has207), rebases `edge` onto it, force-pushes `edge` to `fork` (this
repository) with `--force-with-lease` and starts the CI workflow with
`gh workflow run`. When the run finishes, the Windows build is in a new release
at https://github.com/Forgottenshadow89/xenia-edge/releases (tagged with the
short commit hash) and as the `xenia_edge_windows` artifact of the run.

If the rebase stops with conflicts, upstream changed code a fork commit
touches (`src/xenia/kernel/kernel_state.{cc,h}` or `.github/workflows/CI.yml`).
Resolve them, `git add`, `git rebase --continue`, then re-run the script. If
upstream integrates one of the changes itself, drop the corresponding fork
commit with an interactive rebase.

## Remotes expected by the script

```
origin  https://github.com/has207/xenia-edge.git
fork    https://github.com/Forgottenshadow89/xenia-edge.git
```
