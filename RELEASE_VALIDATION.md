# WheelWizard VR integration validation

Validated locally on Windows x64 on 2026-09-09. These are release candidates; public release
acceptance is not complete.

## Candidates

| Product | Version | Local artifact | SHA-256 |
| --- | --- | --- | --- |
| WiiCompiled OpenXR VR setup | 0.2.32 | `Launcher/dist-vr/WiiCompiled-Setup.exe` | `64fdd492f6ce71fd223b49f493e0809d9a7f9ed45ebaa3605f626eb77282dbe4` |
| WheelWizard VR | 2.5.5 | `Launcher/dist-vr/WheelWizardVRWindows.exe` | `b319b86d46bea6ebfa52965c65706976d6004abc01914ff82a2b73c71de84def` |

The launcher artifact is also in the launcher checkout at `artifacts/WheelWizardVRWindows.exe`.
`Launcher/dist-vr/SHA256SUMS.txt` contains both checksums. Only these executables and their checksums
are intended for distribution. The setup contains the toolkit and source payload, not a compiled
game. All extracted assets, normal installation copies, compiled games, and test NANDs remain in
ignored local validation directories.

The existing local setup in `Launcher/dist` is the user's 0.2.31 rebuild. Its SHA-256 matches the
setup asset published under the VR fork's `0.2` tag. The backend fork also has tag `0.1`; the
launcher fork has no published release. These were checked through the GitHub API before finalizing
the candidates. The next source patch versions are 0.2.32 and 2.5.5. New release tags use `vX.Y.Z`
and must agree with the single version source in each product's `Directory.Build.props`.

## Passed

- WheelWizard suite: **255 tests**, including backend selection/migration, configuration isolation,
  all four backend/game launch command paths, same-version wrong-product rejection, separate
  release routing, exact self-update asset selection, shared NAND configuration, Mii edit blocking,
  and uninstall preservation of both backends' configuration, NAND, and ghosts.
- Retro Rewind transaction tests: staged replacement, cancellation, preservation of saves/ghosts
  and XML save directories, current distribution ZIP layout, archive path validation, interrupted
  publication recovery, and a real Windows locked-file failure after one directory was published.
- Translator suite: **576 tests**.
- Packaged setup self-tests, including product ownership, configuration, portable move healing,
  install transaction rollback, interrupted-install recovery, Retro-WFC snapshot/retry behavior,
  compile-input fingerprints, and product report contracts.
- Full synthetic PowerPC translation and native OpenXR runtime link. The synthetic fixture now
  includes the three callbacks used by the VR camera; no game translation is used for this test.
- Pinned-fact audit and setup payload boundary audit. Native dependency audit inspected **524
  binaries and 5,277 imports**. The pinned OpenXR SDK and static loader were built; licenses remain
  included. A configured bundled SDK takes precedence over a developer machine's OpenXR package.
- Executable identity/version and embedded setup payload identity/version agree. Both Windows
  release workflows use existing GitHub Actions versions and have no Linux/macOS release jobs.
- Final packaged setup installed both VR products from the owned clean PAL image into an isolated
  portable installation. Updating that installation with the final package succeeded, and both
  products reported `current` afterward.
- Real product checks after modifying isolated Retro Rewind content: an asset-only change kept
  both products current; a Code.pul change reported `code-pul-changed` for Retro Rewind and left
  Base current; restoring the content returned both products to current.
- Normal Base, normal Retro Rewind, VR Base, and VR Retro Rewind each stayed running during a
  20-second startup observation through their installed setup host and closed with setup exit 0.
  Normal tests used an isolated copy of the existing normal installation. Test configurations
  pointed to the same isolated NAND and isolated Retro Rewind content.
- Both VR runs used a child-process-only missing OpenXR runtime override. Logs confirmed
  `vr.enabled=true`, `vr.required=false`, D3D12, and desktop fallback. System OpenXR registration
  was not changed.
- The final single-file launcher started using an isolated portable configuration, displayed
  `WheelWizard VR` as its window title, and closed with exit 0. Its startup log contained no
  application-start or unhandled-exception errors. Publishing succeeded with existing Windows
  registry platform-analyzer warnings.
- Source synchronization checked destination hashes before replacing files and retained backups.
  Both repositories pass `git diff --check`. Source changes are local and have not been committed
  or pushed as part of this validation.

## Still required for release acceptance

- Interactive Home/settings verification and complete install/launch flows for all four selections.
  Command-level tests and isolated startup observations do not establish menu or race correctness.
- Headset menus and races for Base and Retro Rewind, physical headset absence, runtime loss during
  a session, and visual confirmation of the nonblocking fallback notification.
- Real player progress/Miis/ghosts exercised across gameplay, updates, reinstall, and uninstall.
  Automated preservation tests passed; live player data was deliberately excluded from smoke tests.
- Fresh installation on a clean Windows machine without developer toolchains. The tested package
  used its bundled tools on this workstation; a pristine-machine run has not been performed.
- Live network-interruption/download tests and interactive overlapping-launcher scenarios.
  File leases, content cancellation, interrupted publication, and setup rollback have automated
  coverage, but the complete manual matrix remains outstanding.

After acceptance, publish the VR backend first, then the launcher. The launcher release workflow
downloads the published backend and checks its VR identity, capability, and tag/version agreement
before publishing. Until that backend release exists, a fresh launcher install cannot resolve this
local 0.2.32 candidate from GitHub automatically.

Detailed local logs and isolated installations are under `.scratch/` (excluded from distribution).
