<!-- SPDX-License-Identifier: MPL-2.0 -->

# Windows MSI packaging (deferred)

The first public beta is **Linux exclusive**. Signed Windows MSI identity, runtime DLL harvest,
Authenticode, and clean-machine install/upgrade/uninstall are deferred until after that Linux beta.
Windows packaging and GPU compatibility need more calendar time than the Linux release.

This directory still contains a WiX 5 skeleton for later Windows work. Do not treat a successful
local MSI build as a first-beta artifact. The packaging script first runs
`cmake --install`, records every staged file and SHA-256 in
`installed-files.json`, and lets WiX's `Files` element harvest that exact tree.
It never reaches into a compiler output directory directly.

Prerequisites are a completed x86-64 Release build, CMake on `PATH`, PowerShell
7, and WiX 5 or newer on `PATH`. Use new or empty staging and artifact
directories:

```powershell
./packaging/windows/package-msi.ps1 `
  -BuildDirectory ./build/release `
  -StagingDirectory ./build/msi-stage `
  -OutputDirectory ./build/msi-artifacts `
  -Version 0.1.0
```

The script deliberately refuses to delete or overwrite a non-empty staging
tree. It also refuses to proceed when the installed executable is absent.

## Release blockers

- The CMake install rules must deploy all pinned Qt, FFmpeg, Protobuf, OpenSSL,
  libebur128, and other runtime DLLs plus required notices. The current install
  rule only guarantees the application executable.
- The product identity, manufacturer identity, upgrade code ownership, Start
  menu integration, file associations, install/upgrade tests, and uninstall
  behavior require release review.
- Authenticode signing and timestamping are **not configured**. The script emits
  an unsigned MSI and says so. Signing credentials must never be stored here.
- The generated MSI must pass dependency auditing, Windows Installer validation,
  malware scanning, and clean-machine install/upgrade/uninstall tests before it
  can be distributed.

