# Third-party dependencies

Everything here is pinned by version and SHA-256 in [`deps.json`](deps.json). `scripts/fetch-deps.ps1` and `scripts/fetch-deps.sh` download into `third_party/`, and refuse to continue on a hash mismatch. Fetched content is not committed.

| Dependency | Version | Licence | Used for | SHA-256 |
|---|---|---|---|---|
| Steam Audio C API SDK | 4.8.1 | Apache-2.0. Bundles Embree (Apache-2.0), Intel IPP (Intel Simplified Software Licence), FFTS, PFFFT and MySOFA; see `THIRDPARTY.md` in the SDK | acoustic simulation + spatial DSP | `4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449` |
| doctest | 2.4.12 | MIT | native unit tests (not shipped) | `94029a7d32da24a56249658147dbd2b33ff0b9ed665295cbbaf19aafff5b0ced` |

Planned (added in the phase that first uses them):

| Dependency | Phase | Licence |
|---|---|---|
| miniaudio | 1 | public domain / MIT-0 |
| libogg + libvorbis | 1 | BSD-3-Clause |

## Redistribution

The packaged mod ships the `phonon` binary for each RID plus `THIRD_PARTY_NOTICES.md`, generated from the SDK's `THIRDPARTY.md` and this file.
