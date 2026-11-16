# Vendored third-party code

Single-header libraries, downloaded with Windows' built-in `curl.exe` from official GitHub releases.
Nothing else is vendored. Versions below were read from the macros inside the downloaded headers.

| Library | Version | File | Source URL | SHA-256 |
|---|---|---|---|---|
| doctest | 2.5.3 | `doctest.h` | https://raw.githubusercontent.com/doctest/doctest/v2.5.3/doctest/doctest.h | `CFD518A3EF90F67E1F3BA514DF23FB3627437DE1A2FEEBA78CF5062A40021421` |
| nlohmann/json | 3.12.0 | `json.hpp` | https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp | `AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63` |

Downloaded: 2026-10-28.

Re-verify with PowerShell: `Get-FileHash third_party\doctest.h -Algorithm SHA256`.
