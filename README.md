# Apple Windows Mount

Mount iPhone and iPad media and shared app documents as Windows drive letters.
The application uses Microsoft USB drivers, an open-source usbmuxd transport,
libimobiledevice, and WinFsp. Project-owned code is GPL-3.0-or-later.

The current implementation targets Windows 11 x64. Hardware validation has been
performed with an iPad12,1 running iPadOS 27.0.1. Additional device models,
Lightning connections, and clean-machine installation remain release acceptance
work. See [validation notes](docs/validation.md).

## Run

Build the setup bundle using the commands below, or use the executables under
`build/windows-x64/bin/Release` after installing WinFsp 2.1.

1. Launch `AppleWindowsMount-Setup.exe` to install WinFsp and the application.
2. Connect and unlock the device. USB setup requires administrator assistance.
   For a device connected after installation, choose **Set up USB devices** from
   the tray menu or run `awm setup`.
3. Reconnect the device after driver setup. Windows may require a restart.
4. Launch **Apple Windows Mount** from the Start menu and accept **Trust This
   Computer** on the device.

The tray application automatically mounts trusted devices. The initial drive
preference is `I:`. It selects an available letter if the preferred letter is
occupied and remembers the result for that device.

The volume exposes `Media` and `Apps`. Application directories use bundle
identifiers. Each application receives a separate House Arrest session for its
shared Documents directory. The tray menu provides Explorer access, mount and
unmount controls, read-only selection, app refresh, pairing removal, diagnostic
export, and optional startup with Windows.

## Command line

```powershell
awm start
awm status --json
awm devices --json
awm pair
awm mount --drive I:
awm mount --read-only
awm mount --read-write
awm apps --json
awm refresh
awm unmount
awm shutdown
awm doctor --json
```

Use `--udid DEVICE_ID` to select a device when several are connected. `info`,
`apps`, `ls`, and `transfer-probe` also work with a separately running
`awm-usbmuxd` transport.

```powershell
awm info --json
awm ls --path /DCIM --json
awm transfer-probe --bytes 8388608 --json
```

The transfer probe creates a unique test directory, uploads random data,
downloads it, compares SHA-256 hashes, and removes its files. A cleanup failure
reports the remaining device path. `--port` selects an alternative loopback port
for isolated transport testing.

For development, `awm-mount --udid DEVICE_ID --drive I: --media-only` mounts the
media root directly. The mount worker supports `--read-only` and exits on
Ctrl+C or device removal.

## Build and test

Install Visual Studio's C++ desktop tools, CMake 3.25 or later, and WinFsp 2.1
with its Developer feature. Set `VCPKG_ROOT` to a vcpkg checkout. The manifest
pins the registry baseline and applies a local MSVC endianness correction to
libimobiledevice.

For Visual Studio 2022,

```powershell
$env:VCPKG_ROOT = 'C:\path\to\vcpkg'
cmake --preset windows-vs2022
cmake --build --preset vs2022-debug
ctest --preset windows-vs2022
cmake --build --preset vs2022-release
```

For Visual Studio 2026, use configure, build, and test preset `windows-x64`.
Use build preset `release` for release binaries. The native filesystem tests
mount an in-memory fake backend on an available drive letter and exercise the
Win32 entry points. They require the WinFsp driver and run without an iOS device.

With a trusted device connected, run the hardware smoke suite. It verifies an
eight MiB AFC round trip and mounted Unicode file creation, hashing, rename,
truncation, and cleanup. Supply a file-sharing app's bundle identifier to test
its Documents directory too. This selects read/write mounting and leaves it active.

```powershell
./scripts/test-device.ps1 -Udid YOUR_UDID -AppBundleId YOUR_APP_BUNDLE_ID
```

The suite writes a diagnostic report to `build/hardware-validation.json`.
The remaining device and installer acceptance matrix is in
[docs/validation.md](docs/validation.md).

Create the application MSI and combined setup executable using a .NET SDK and
the pinned WiX SDK restored by the installer projects.

```powershell
./scripts/package.ps1
```

For a Visual Studio 2022 build, pass
`-BuildDirectory build/windows-vs2022`. The script stages release binaries and
runtime dependencies, validates the upstream WinFsp installer, and builds both
installer projects. The setup executable embeds the upstream installer
unchanged. Build artifacts are unsigned development packages.

## Filesystem behavior

| Capability | Behavior |
| --- | --- |
| File I/O | Offset reads/writes, append, truncation, large offsets, and synchronous AFC acknowledgments |
| Windows access | WinFsp sharing modes, file locks, and a current-user security descriptor |
| Rename | Open-handle paths follow renames. Cross-source moves return the cross-device error |
| Replace by rename | Returns an unsupported status when a destination already exists |
| Read-only mode | Enforced by volume flags and mutation callbacks |
| Timestamps | AFC persists modification-time updates. Creation/access/change-time requests are compatibility hints |
| Attributes | Directory and archive attributes. Additional Windows attributes return an unsupported status |
| Flush | Completes acknowledged writes and reports service errors. Device flash persistence follows AFC's guarantees |
| Names | Unicode names are preserved. Unsafe and case-colliding names receive escaped aliases |
| Long aliases | SHA-256 aliases resolve through the directory's original-name mapping |
| Links and streams | Symbolic links, reparse-point operations, alternate streams, and extended attributes are excluded |
| Caching | One-second directory/metadata lifetimes and bounded directory caches |

Media and shared app documents follow the access granted by iOS services.
Private app storage, iCloud document providers, the complete Files-app namespace,
and privileged jailbreak services are outside this implementation.

On disconnect, the worker unmounts and open handles expire with that process.
Reconnection creates fresh sessions. A request stalled for 30 seconds terminates
its mount generation. Service failures invalidate the affected session, while
app-service failures stay isolated from other sources. Writes receive explicit
failure on interruption and require a new caller operation after reconnect.

WinFsp's cleanup callback has no return status. A device-side deletion failure
at cleanup is logged and stops the mount to expose the failed state.

## State and driver recovery

Preferences, logs, exported diagnostics, and ACL-protected pairing records are
under `%LOCALAPPDATA%\AppleWindowsMount`. Pairing keys are excluded from logs and
diagnostic export. Treat the Pairing directory as device access credentials.

Driver setup selects Microsoft's inbox composite and WinUSB drivers, chooses a
descriptor configuration containing the USB multiplexing interface, registers
the interface GUID, and records the previous settings before changing them.
Installer driver journals are under
`%ProgramData%\AppleWindowsMount\DriverBackups`. The helper supports restoration
from an individual journal or a directory of journals. Installation rollback and
uninstall invoke restoration. Reconnect disconnected devices before restoring
their bindings.

```powershell
# Run from an elevated terminal.
awm-driver restore --journal C:\path\to\device-backup.json
```

UsbDk remains an experimental transport option. The tested UsbDk/libusb path
selected the first configuration and exposed no USB multiplexing endpoints on
the test iPad. The default transport validates the active configuration before
opening its endpoints. WinFsp is shared with other applications and remains
installed after removing Apple Windows Mount.

Dependency versions, license notices, and source-distribution requirements are
recorded in [THIRD_PARTY.md](THIRD_PARTY.md).

`scripts/package-sources.ps1` collects the application source, dependency source
archives, pinned vcpkg recipes, and WinFsp source under `build/distribution/sources`.
Distribute this source collection alongside binary releases.
