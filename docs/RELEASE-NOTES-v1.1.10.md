# RocketNet v1.1.10

Changes compared with the `v1.1.9` tag. A point release: the library is
unchanged from v1.1.9 (no source, configuration, export, counter or wire
change), and v1.1.10 endpoints interoperate with every v1.x endpoint.

- Release tooling: a point release, and a landing through `tools/land.py`,
  is gated by the light per-commit jobs. The functional, transport matrix,
  happy path, sanitizer and fuzz smoke jobs still run on every commit and
  their results are printed, but they no longer gate a point release
  (docs/RELEASING.md). Minor, major and pre-releases still require every job.

- Tag: `v1.1.10`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.10).
