# Preview-4 qualification record

This record holds only what has been run and recorded. Where a result is not
yet in hand, the section says so and names what fills it. Manual results are
not GitHub CI runs. A result carried from another commit names that commit.
The gates still owed when the tag is cut are listed under "Not qualified".

## Candidate head

- Base: `23724925`, `main` after #627, the newest commit under this branch
  before the release commit.
- Landed before the tag, recorded in the notes as landed: #629 (the public
  contract audit) and #631 (the macOS fuzzing record).
- Release head: **TO BE FILLED** when the release pull request merges: the
  exact `main` commit the tag will name, reconciled with this record
  ([RELEASING.md](RELEASING.md), "Cutting the release", steps 5 and 7).
- Changes since the preview-3 tag are in
  [the release notes](RELEASE-NOTES-v1.0.0-preview-4.md). The standard and the
  wire encodings did not change.

## CI on the release head

The qualification for this release is the CI run on the tag commit
`afa3cf36`. This table was never filled; the run id is cited in the release
notes. Placeholder fields below are left as they were.

| Item | Value |
| --- | --- |
| Release head commit | TO BE FILLED |
| CI run id | TO BE FILLED |
| Other workflow run ids | TO BE FILLED |
| Jobs passed / failed / cancelled | TO BE FILLED |
| Legs waived, with the reason | TO BE FILLED |

## Run records added since preview-3

| Platform | Run | Head | Result | Record |
| --- | --- | --- | --- | --- |
| macOS arm64 | libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer, the ten macOS fuzz targets, 900 s each, 231,163,029 executions in total | `a82dbf37` | every process exited 0; 0 crashes, 0 findings | [runs/macos-fuzz.md](runs/macos-fuzz.md) (#631) |

The records from preview-3 (the LAN proof, the soak records and the
fifty-times transport cells in [runs/](runs/)) are unchanged and were made at
earlier heads; see [the preview-3 record](QUALIFICATION-v1.0.0-preview-3.md)
for their heads. They are not claimed for the preview-4 head.

## Documentation and version gates

At the release branch head, run by hand:

- `python3 tools/check_release_doc.py`
- `python3 tools/test_release_version.py`
- The `version-consistency` step of
  [release-check.yml](../.github/workflows/release-check.yml): shipping
  `ROCKETNET_VERSION_FULL=v1.0.0-preview-4  MAJOR.MINOR.PATCH=1.0.0`,
  prerelease `preview-4`; development `dev` with `255.255.255`.
- `make roadmap-check` (historical check; the roadmap and renderer were retired on 2026-10-29)

Results are recorded in the release pull request.

## Not qualified

Owed at tag time, or not part of this preview. None of these is reported as a
pass.

- **Native unit and functional suites** on Linux, macOS and Windows at the
  release head: not recorded here. The preview-3 suites ran at earlier heads.
- **Release head CI**: see the placeholder above.
- **Review of the public header** `sdk/rocketnet.h` by the release owner,
  before the tag. The loopback hub and the new statuses are public API changes.
- **Tag commit reconciliation**: the merged `main` commit compared with this
  record, and the version, wire-change and tag checks run on it.
- **Space vendor gate** and **space soak gate**: the integrating application
  re-vendored at the release commit and both jobs passing on it
  (RELEASING.md step 6). Not run.
- **Windows server under LAN load**, **AF_XDP zero copy and line rate**, the
  **Linux AF_XDP soak loss** re-measurement, and the **complete security
  review**: as in the preview-3 record.

The [release notes](RELEASE-NOTES-v1.0.0-preview-4.md) give the known limits
for users of this preview.
