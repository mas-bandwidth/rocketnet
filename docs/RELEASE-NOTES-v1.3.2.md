# RocketNet v1.3.2

Changes since the `v1.3.1` tag. This is a point release. The packet layout and protocol version are the same as
v1.3.1, the public API and ABI are the same, and there are no new counters (the contract still holds 315).
v1.3.2 endpoints work with v1.3.1 endpoints. No code behaviour changes.

## Licence

- rocketnet is dual-licensed: `AGPL-3.0-or-later OR LicenseRef-MBSL-1.0`. You may use it under the GNU Affero
  General Public License v3.0 or any later version, or under the commercial Más Bandwidth Source License 1.0
  (`MBSL.md`). The README's "License and credits" section summarises both.
- `LICENSE` is unchanged: the verbatim AGPL-3.0 text.
- Each source file's notice names both licences.
- The XDP program in `sdk/xdp/` keeps its own licence, the GPL. Third-party code (`sodium/` and the vendored test
  data and libraries) keeps its own licences.
- The installed package carries `MBSL.md` beside `LICENSE` in `share/rocketnet/`.
- `docs/CREDITS.md`: rocketnet was created by Glenn Fiedler and Rowan Claude.

Upgrade: replace v1.3.1 with v1.3.2. You do not need to change any source.
