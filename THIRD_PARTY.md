# Third-party source and licenses

Project-owned source is GPL-3.0-or-later. Individual upstream files retain their
original copyright notices and licenses.

The USB multiplexing implementation in `third_party/usbmuxd` is derived from
https://github.com/libimobiledevice/usbmuxd at commit
`3ded00c9985a5108cfc7591a309f9a23d57a8cba`.
It is distributed under GPL version 2 or version 3, with LGPL-2.1-or-later
utility declarations as indicated by the source headers. This application uses
the GPL version 3 option. Windows changes widen socket handles, adapt socket
errors and polling, authenticate loopback peers, support MSVC wire layouts,
validate the active USB configuration, and avoid automatic USB mode switching.
Windows process management, logging, timers, and configuration storage are
implemented in `src/transport`.

The libimobiledevice overlay under `ports` derives from Microsoft's vcpkg
baseline `40f3c709db80acf154ac4b17a1f83c564ebd022e`. Its original build recipes and
patches are MIT licensed, with that notice in `ports/LICENSE.txt`. The additional
MSVC byte-order patch prevents byte-swapping of AFC's 64-bit fields.

The local iFuse checkout at commit
`f7eac74184ff240c8ba9cf9b413eed75968a462a` was consulted for protocol usage.
The application client implementation was written for this project.

The exact library sources and build patches are selected by the builtin vcpkg
baseline in `vcpkg.json`. The initial build uses the following versions.

| Dependency | Version | License |
| --- | --- | --- |
| libusb | 1.0.30 | LGPL-2.1-or-later |
| libplist | 2.7.0 | LGPL-2.1-or-later |
| libimobiledevice-glue | 2023-05-13 | LGPL-2.1-or-later |
| libusbmuxd | 2023-06-21, port revision 1 | LGPL-2.1-or-later |
| libimobiledevice | 2023-07-05, port revision 1 | LGPL-2.1-or-later |
| OpenSSL | 3.6.3 | Apache-2.0 |
| nlohmann/json | 3.12.0, port revision 2 | MIT |

Installed binary packages include the dependency copyright files. Before public
distribution, include corresponding source for the vendored code, modified
dependencies, and bundled GPL/LGPL components, including the baseline's build
instructions and patches. Retain upstream notices in source archives.

UsbDk 1.0.22 is an optional separately installed driver from
https://github.com/daynix/UsbDk/releases/tag/v1.00-22. Its code is Apache-2.0.
The x64 MSI SHA256 is
`91f6f695e1e13c656024e6d3b55620bf08d8835ef05ee0496935ba6bb62466a5`.
It is excluded from the application ZIP. The first-configuration restriction
found during hardware testing is recorded in the transport validation notes.
