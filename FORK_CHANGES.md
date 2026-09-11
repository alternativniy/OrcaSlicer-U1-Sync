# Fork Changes

Everything in this repo that is **not** upstream OrcaSlicer/OrcaSlicer — i.e. everything to re-check
or re-apply when rebasing/merging onto a newer upstream version. This is not a changelog for users;
it's a maintenance checklist for us.

## How this list was built

Each of our topic branches carries both upstream release-branch history and our own commits,
interleaved. To find *only* ours on a given branch:

```bash
git log $(git merge-base origin/main <branch>)..<branch> --no-merges --format="%h|%an|%s"
```

Everything authored by `SoftFever` / other upstream maintainers (direct pushes to their release
branch, mostly version bumps and CI housekeeping, no PR number) is upstream's own — not ours.
Everything authored by `r.romantsov` is ours. Re-run the command above after a rebase, on every
branch in the map below, and diff against the list in this file to catch anything new or dropped.

## Branch map

Where each change actually lives. `release/v2.4-u1` is the integration branch — everything merged
there is "live"; anything only on a topic branch below is **not yet in `release/v2.4-u1`**.

| Branch | Base | Merged into `release/v2.4-u1`? | Carries |
|---|---|---|---|
| `fix/no-sparse-layers-toolchange` | `release/v2.4` (old) | Yes | Change #1, fix half |
| `fix/no-sparse-layers-toolchange-main` | `main` | No (upstream PR copy, see watchlist) | Change #1, fix half, rebased onto `main` |
| `feature/no-sparse-layers-protection` | `release/v2.4` (old) | Yes | Change #1, detection half |
| `feature/no-sparse-layers-protection-main` | `main` | No (upstream PR copy, see watchlist) | Change #1, detection half, rebased onto `main` |
| `fix/snapmaker-u1-sync` | `main` | Yes (same commits) | Change #2 |
| `feature/retraction-toolchange` | `main` | Yes (same commits) | Changes #3, #4 |
| `feature/snaporca-migrate-features` | `main` | **No** | Change #8 (M220 B/R) — only branch that has it right now |

The `-main` branches exist because the no-sparse-layers work started life on `release/v2.4` and had
to be rebased onto `main` to open upstream PRs (drifted too far to apply cleanly otherwise); they're
what's actually open against `OrcaSlicer/OrcaSlicer`, see the watchlist. `release/v2.4-u1` has its
own copies of the same fix/detection commits, applied directly to the old base — that's why the
`-main` variants show "No" here despite the change being live on `release/v2.4-u1`.

**Change #8 is the one thing in this file not yet on `release/v2.4-u1`.** Cherry-pick
`ce17f6a78a` from `feature/snaporca-migrate-features` before the next rebase, or it'll get lost.

## Changes

### 1. "No sparse layers" toolchange collision fix + detection

Root cause and fix worked out from scratch in this dialogue (no existing upstream PR found for
[OrcaSlicer/OrcaSlicer#11703](https://github.com/OrcaSlicer/OrcaSlicer/issues/11703) at the time,
and none of the collision-check PRs found since — see watchlist — cover this specific mechanism
either, they're about a different check). Two independent branches, both merged into `release/v2.4-u1`:

**`fix/no-sparse-layers-toolchange`** — the actual collision fix. With `wipe_tower_no_sparse_layers`
on, the wipe tower's printed height can sit well below the tallest object between real toolchanges.
Physical Z used to stay at that low tower height through the whole `change_filament_gcode` macro
(and the final purge), so toolchange travel — or a toolchanger's physical head swap — could plow
through already-printed parts.
- `src/libslic3r/GCode.cpp` — `WipeTowerIntegration::append_tcr()` / `append_tcr2()`: restore Z to
  the real topmost layer height before `change_filament_gcode` runs, lower back to the tower after.
  New placeholders `restore_layer_z_before_toolchange` / `deretraction_from_wipe_tower_generator`.
- `src/libslic3r/GCode/WipeTower.cpp`, `WipeTower2.cpp` — emit those placeholders (legacy Type1 and
  default Type2 tower generators both).
- `src/libslic3r/GCode.cpp` — `WipeTowerIntegration::finalize()` / `prime()`: the final purge at
  print end was targeting `m_final_purge.print_z` (== the real per-layer Z, always in sync
  regardless of `wipe_tower_no_sparse_layers`) instead of `m_last_wipe_tower_print_z` (the tower's
  actual accumulated height) — printed the last purge in mid-air. Fixed to use the latter; `prime()`
  now also updates `m_last_wipe_tower_print_z` so a priming-only print (no later toolchange) still
  gets this right.
- Commits: `839fb636e6`, `53a2314d12` (final; superseded an earlier `e3dbe65367` that was reverted
  and redone during a main-merge).
- **Later reverted on `release/v2.4-u1`** (see "Reverted, on purpose" below) — the final-purge half
  reintroduced the exact gantry-row collision risk it was meant to avoid, in a case the detector
  (change below) can't catch. The toolchange-travel fix itself was kept.

**`feature/no-sparse-layers-protection`** — detection + visualization, since nothing caught this
placement risk before. Reuses existing `extruder_clearance_radius` / `extruder_clearance_height_to_rod`
settings; no new printer settings added.
- `src/libslic3r/Print.cpp` — `layered_print_cleareance_valid()`: two new hard-error checks (promoted
  from a softer warning once we could actually detect the risk), both gated on
  `wipe_tower_no_sparse_layers` (without it the tower always tracks the real layer Z, so there's no
  gap to collide into):
  - object within `extruder_clearance_radius` of the tower footprint, even without direct overlap.
  - object sharing the tower's Y-row and taller than `extruder_clearance_height_to_rod` — on
    Cartesian/CoreXY kinematics the X-gantry beam spans the full bed width, so it sweeps that whole
    row regardless of X distance. Known limitation: Y is hardcoded as "the gantry axis", inherited
    from the same assumption already baked into upstream's by-object `sequential_print_clearance_valid()`;
    wrong for delta or non-standard kinematics, and Orca has no per-printer setting to express this.
  - Visualization reuses the existing by-object collision keep-out-zone renderer
    (`set_sequential_print_clearance_polygons`) — no new GUI/GL code.
- `tests/fff_print/test_wipe_tower.cpp`, `tests/fff_print/CMakeLists.txt` — two `Print::validate()`
  tests (positive + negative). Deliberately call `Print::validate()` directly rather than a full
  slice: `has_wipe_tower()` only needs `enable_prime_tower` + 2 configured filament diameters, and
  getting a real per-object filament assignment to survive `Print::apply()`'s filament-usage
  auto-trim turned out to be its own rabbit hole in this fork's filament-grouping code — not worth
  it just to exercise a validate()-time geometry check.
- Commits: `0be65a114b`, `5b32911159`.
- **Known gap, not yet fixed:** the `wipe_tower_no_sparse_layers` gate assumes the tower always
  tracks real layer Z when that option is off. False — if real toolchanges simply stop needed
  partway through a print (e.g. black-then-white-only for the rest), the tower freezes at its last
  real height regardless of the option, and this detector stays silent. The underlying G-code fix
  (change above) is unconditional and still protects the print; only the *warning* has this hole.

Verified against real repro G-code (Snapmaker U1) and a full local `ctest` run (only pre-existing,
unrelated failures). Not verified: an actual physical print.

#### Reverted, on purpose: final-purge relocation

The `finalize()`/`m_last_wipe_tower_print_z` half of commit `53a2314d12` (and the `e3dbe65367` it
superseded) moved the print's last purge onto the wipe tower's actual (possibly lower) height
instead of the real top layer, to stop it printing disconnected in mid-air. Revisited later: doing
that reintroduces the very gantry-row collision the toolchange fix exists to prevent — the toolhead
ends up stationary at a low Z, in the tower's Y-row, which can still hit a tall neighbor — and this
specific case isn't reliably detectable for every plate layout. A purge floating disconnected in
mid-air is purely cosmetic (a small blob, discarded with the print) and is inherently safe from that
collision, since it always happens at the print's actual current maximum height. Reverted back to
mid-air on all three branches that had it:
- `release/v2.4-u1`: `b6ec7366b9` (revert of `53a2314d12`), `f792045bd6` (revert of `e3dbe65367`).
- `fix/no-sparse-layers-toolchange`: same two reverts, different hashes.
- `fix/no-sparse-layers-toolchange-main`: same two reverts, different hashes.

If re-doing this fork's no-sparse-layers work from a newer upstream base, **don't reintroduce the
final-purge relocation** — mid-air is the intended, safer behavior.

### 2. Snapmaker filament profile sync

Synchronizes user filament profiles with the printer; includes a follow-up fix for original-manufacturer
filament sync. Worked out independently in this dialogue — not sourced from an upstream PR.
- `src/slic3r/GUI/Plater.cpp`, `src/slic3r/Utils/IPrinterAgent.hpp`,
  `src/slic3r/Utils/SnapmakerPrinterAgent.cpp/.hpp`.
- Commits: `c8cf1ab792`, `2628f52da5`.
- Also on branch `fix/snapmaker-u1-sync` (same two commits, not just `release/v2.4-u1`).
- **Related upstream PR, different approach, not the source:**
  [OrcaSlicer/OrcaSlicer#12232](https://github.com/OrcaSlicer/OrcaSlicer/pull/12232) "Fix Snapmaker
  AMS filament profile matching (Snapmaker agent only!)" (open, unmerged) tackles the same class of
  bug (wrong preset picked on AMS sync) but with an incompatible implementation (own
  `resolve_tray_info_idx()`/tokenizer, touches `MoonrakerPrinterAgent.cpp/.hpp` too, which we don't).
  If it merges upstream first, expect a real conflict in `SnapmakerPrinterAgent.cpp` — compare both
  before picking one, don't try to keep both.

### 3. Toolchange retraction settings exposed in the GUI

"Retraction Length (Toolchange)" and "Extra Length on Restart (Toolchange)" existed as config options
but were hidden/disabled; now exposed per-filament with proper labels/tooltips.
- `src/libslic3r/Preset.cpp`, `src/libslic3r/PrintConfig.cpp`, `src/slic3r/GUI/Tab.cpp`.
- Commit: `73bf782e84`.
- Also on branch `feature/retraction-toolchange` (same commit, plus #4 below).
- **Source:** adapted from upstream PR
  [OrcaSlicer/OrcaSlicer#13509](https://github.com/OrcaSlicer/OrcaSlicer/pull/13509) "Add toolchange
  retraction overrides to filament setting overrides" by ProtoxiDe22 — open, not yet merged as of
  writing. Same files (`Preset.cpp`, `PrintConfig.cpp`, `Tab.cpp`), same `full_label`/
  `append_retraction_option` fix. **If #13509 merges upstream, drop our version of this and take
  upstream's instead** — don't keep both.

### 4. TPU filament profile tuning

Adjusted TPU settings across many vendor filament profiles (`resources/profiles/*/filament/*tpu*.json`
and a few Snapmaker/Bambu/Chuanying/COEX-specific ones) to actually use the `retract_length_toolchange`
override that #3 exposes — TPU is exactly the "flexible filament, needs different toolchange
retraction" case #13509's own rationale describes.
- Commit: `770f6d442e`.
- Also on branch `feature/retraction-toolchange` (same commit, alongside #3).
- **Our own follow-up, not sourced from a PR** — #13509 only exposes the setting in the GUI, it
  doesn't touch any profile JSON. No upstream PR found doing this specific profile tuning.

### 5. Flatpak CI/CD path fix

`upload-artifact`/nightly-deploy steps used a hardcoded `/__w/OrcaSlicer/OrcaSlicer/...` path instead
of `${{ github.workspace }}`.
- `.github/workflows/build_all.yml`.
- Commit: `f98502faef`. (This got silently dropped once already during an earlier main-merge before
  being re-applied — if it goes missing again after a rebase, that's why; grep the file for
  `github.workspace` near the flatpak upload/deploy steps.)

### 6. Fork identity — independent versioning / update checks

So this fork doesn't compare its version against, or offer updates from, upstream's own release feed.
- `src/libslic3r/AppConfig.cpp` — `VERSION_CHECK_URL` points at this repo's GitHub releases
  (`api.github.com/repos/alternativniy/OrcaSlicer-U1-Sync/releases/latest`) instead of
  `check-version.orcaslicer.com`. `profile_update_url()` deliberately left untouched — profiles still
  come from upstream.
- `src/slic3r/GUI/GUI_App.cpp` — version-compare regex in `check_new_version_sf()` extended to accept
  dot-separated pre-release identifiers with embedded hyphens (needed for our `MAJOR.MINOR.PATCH-u1-
  MAJOR.MINOR.PATCH` scheme below); previously only matched a single flat `-identifier`, so anything
  with a `.` in the suffix silently failed `std::regex_match` and was ignored by the updater.
- `version.inc` — `SoftFever_VERSION` is `2.4.2-u1-1.0.0`: upstream base version, then our own
  independent semver for the fork (`1.0.0`, bump on our own release cadence, unrelated to upstream's).
  Verified against the vendored `semver.c` that numeric-only dot-separated components compare
  numerically (`1.0.9 < 1.0.10`, not lexicographically) — this scheme depends on that.
- Commit: `85c6e7a3e9`.
- Not sourced from any PR — our own infrastructure decision. Re-pick a fork version number after each
  upstream base bump (e.g. next base 2.4.3 → `2.4.3-u1-1.0.0` or continue the `1.x.x` line, whichever
  makes sense at the time).

### 7. README

"About this fork" section summarizing items 1-4 above for anyone landing on the repo.
- Commit: `0bb8923e1a`.

### 8. Snapmaker U1 speed-override (M220 B/R) restored after wipe tower purge

**Not yet merged into `release/v2.4-u1` — only exists on `feature/snaporca-migrate-features`.**
`WipeTowerWriter2::speed_override_backup()`/`speed_override_restore()` (`M220 B`/`M220 R`) only fired
for `gcfMarlinLegacy`/`gcfMarlinFirmware`. U1 runs klipper flavor, so the wipe tower's forced
`M220 S100` override before purging was never restored afterward — any user-set feedrate override got
silently and permanently reset to 100% at the very first toolchange, with no way to tell why.
- `src/libslic3r/GCode/WipeTower2.hpp`, `WipeTower2.cpp` — added `m_printer_model` (threaded through
  both `WipeTower2` and `WipeTowerWriter2` constructors) and `is_snapmaker_u1()`, gated the two M220
  calls on it alongside the existing Marlin check.
  **Did not** touch `WipeTower.cpp`/`WipeTower.hpp` (legacy Type1/BBL path) — M220 B/R is `#if 0`'d
  out there entirely ("BBL machine don't support speed backup"), doesn't apply to U1, left alone.
- Commit: `ce17f6a78a`.
- **Source:** adapted from `Snapmaker/OrcaSlicer` fork source directly (tag `v2.3.5`,
  `src/libslic3r/GCode/WipeTower2.cpp`), which already gates the same two calls on
  `is_snapmaker_u1() { return boost::icontains(m_printer_model, "Snapmaker") && boost::icontains(m_printer_model, "U1"); }`.
  This is **not** an `OrcaSlicer/OrcaSlicer` PR — it's Snapmaker's own downstream fork, a different
  repo entirely (`github.com/Snapmaker/OrcaSlicer`). Their code has no per-file license header
  beyond the project's own AGPLv3, so this is a same-license port, not a foreign-license concern —
  but if re-verifying, re-diff against whatever their current tag is, not v2.3.5 specifically.

## Not fork changes (verified, left alone)

A few things looked like ours at first glance but turned out to be upstream's own, and were
resolved by taking upstream's side during the last main-merge:
- `src/libslic3r/calib.hpp` scalar-vs-array config accessors — upstream's own backport-compat fix
  for their release/v2.4 branch (`6888f9d806`, author SoftFever), obsoleted once merged past the
  point upstream fixed it properly in `main`.
- `src/libslic3r/PresetBundle.cpp` duplicate-line cleanup (`2c029402d7`, author SoftFever).
- All Snapmaker U1 printer/nozzle/process profiles (`resources/profiles/Snapmaker/...`) — upstream
  now carries official U1 support directly (PR-numbered commits), so these track upstream, not us.

## Upstream PR watchlist

PRs on `OrcaSlicer/OrcaSlicer` scouted for relevance to this fork. None of these are merged into our
tree yet — this is a "check back later" list, not a changelog. Re-check status
(`gh pr view <n> --repo OrcaSlicer/OrcaSlicer --json state,mergedAt,mergeable`) before every rebase;
anything merged upstream should be dropped from here and, if we'd carried a local equivalent,
reconciled against it.

**Directly relevant, worth taking independent of merge status** (small, clean, low conflict risk):
- [#15483](https://github.com/OrcaSlicer/OrcaSlicer/pull/15483) — cap ABS/ASA/PPS bed temps at 100°C
  for U1. Simple profile fix.
- [#15349](https://github.com/OrcaSlicer/OrcaSlicer/pull/15349) — U1 exported filename should respect
  filament selection. Simple bug-fix.
- [#15482](https://github.com/OrcaSlicer/OrcaSlicer/pull/15482) — declare `filament_vendor` on
  Snapmaker per-type filament bases. Simple profile fix.
- [#14987](https://github.com/OrcaSlicer/OrcaSlicer/pull/14987) +
  [#15544](https://github.com/OrcaSlicer/OrcaSlicer/pull/15544) (stacked) — sequential-print
  (`print_sequence == "by object"`) collision check fix: object order was by 3mf list index, not
  real geometry (false "too tall" errors); #15544 also narrows the rod-collision check to material
  actually above `height_to_rod`, not the whole object footprint. `CLEAN`/`MERGEABLE` as of last
  check, author measured real U1 clearance by hand. Touches `Print::sequential_print_clearance_valid()`
  — a **different** function from our own `layered_print_cleareance_valid()` (change #1 above,
  layer-print/wipe-tower path); no expected overlap, but same neighborhood of the file, diff
  carefully. Author also has a mirror PR open directly on `Snapmaker/OrcaSlicer#630`.
- [#14400](https://github.com/OrcaSlicer/OrcaSlicer/pull/14400) — per-toolhead nozzle-size picker +
  "Mixed Nozzle Sizes" auto-conversion for multi-toolhead printers. Mostly new files
  (`NozzleAgnostic.{hpp,cpp}`, `NozzlePickerPanel.{hpp,cpp}`), has its own unit tests. `CONFLICTING`
  with current main but stale (no maintainer review in ~2.5 months) rather than contested — likely
  cherry-picks cleanly onto our tree even before it lands upstream.

**Large/active, do not take yet — architecture still moving:**
- [#15145](https://github.com/OrcaSlicer/OrcaSlicer/pull/15145) — U1 filament inventory management,
  AMS-style sync dialog, filament→tool mapping. 102 files, +10146. Stacks on 4 other open PRs
  (#15179, #15180, #15183, #15185). `CONFLICTING`/`DIRTY`, actively being fixed day-to-day as of
  early September (tester-reported regressions, author rebasing). **Directly touches
  `SnapmakerPrinterAgent.cpp/.hpp`** — guaranteed conflict with our change #2 whenever this lands.
  Revisit change #2 against this PR's `FilamentInventory` architecture once it merges or stalls out.
- [#13286](https://github.com/OrcaSlicer/OrcaSlicer/pull/13286) — earlier, smaller filament→extruder
  mapping feature. Author of #15145 says it "probably supersedes" this one. Last commit April 19,
  dead in the water since — treat as abandoned, nothing to take from it.
- [#15196](https://github.com/OrcaSlicer/OrcaSlicer/pull/15196) — filament mapping + calibration
  options for U1; possibly the origin of the `filament_map`/`filament_map_mode` config keys already
  visible in our sliced G-code. Worth a closer read once #15145 (which likely absorbs it) settles.
- [#15145](https://github.com/OrcaSlicer/OrcaSlicer/pull/15145)-adjacent:
  [#15278](https://github.com/OrcaSlicer/OrcaSlicer/pull/15278) and
  [#14760](https://github.com/OrcaSlicer/OrcaSlicer/pull/14760) — more Snapmaker AMS filament-matching
  variants (subtype-aware, priority order). Same conflict risk with change #2 as #12232 above; check
  all of #12232/#15278/#14760/#15145's `SnapmakerPrinterAgent` approach together, once one of them
  actually merges, rather than reconciling against each separately.
- [#12782](https://github.com/OrcaSlicer/OrcaSlicer/pull/12782) — "Consolidated WipeTower generation
  in a single pipeline", merges `WipeTower.cpp`/`WipeTower2.cpp` (Type1/Type2) into one. If this
  lands, change #1 and change #8 (both currently split across the two files) will need re-applying
  against whatever the unified pipeline looks like — re-read this PR's diff first, don't blind
  cherry-pick our commits onto it.
