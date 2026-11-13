# Validation record

## Environment

Hardware testing on November 12, 2026 used an iPad12,1 running iPadOS 27.0.1 and
a Windows 11 x64 PC. Microsoft `usb.inf` and `winusb.inf` replaced the device's
composite-parent and USB multiplexing-interface Apple bindings. Apple device
upper/lower filters were removed from those bindings. Apple Mobile Device
Service was absent. An Apple Ethernet driver remains bound to the separate
network interface, and Apple packages remain in the development machine's
driver store. The diagnostic environment check therefore reports false, and
clean-machine acceptance remains pending. The mount uses the Microsoft USB
multiplexing interface. Memory Integrity was enabled. Secure Boot status could not be queried
from the unelevated development process.

UsbDk 1.0.22 installed successfully and required a PC restart before enumeration
worked. Its libusb configuration setter is a no-op, and the selected first
configuration exposed no multiplexing interface on this iPad. The working path
uses Microsoft's composite driver and WinUSB. Windows selected configuration 4
after setting `OriginalConfigurationValue` to descriptor index 3 on this machine.

## Verified behavior

- Device discovery, first-time trust, pairing, device information, and shared-app discovery.
- Eight MiB uploaded and downloaded through AFC with matching SHA-256 hashes.
- A mounted aggregate drive containing Media and Apps.
- Media copy-in/copy-out verified by hash, open-file access, rename, truncation, and deletion.
- Owlfiles shared documents through House Arrest, Unicode filename copy, matching hashes, and memory-mapped reading.
- Read-only remounting rejected file creation.
- Upstream WinFsp 2.1 external tests passed on a unique media test directory. The selected tests were `create_share_test`, `rdwr_noncached_test`, `rdwr_cached_test`, and `flush_test`.
- Controller startup, automatic mounting, status queries, remembered drive selection, remounting, and shutdown.
- Application MSI installation completed successfully on the development PC. The installed Program Files binaries mounted the iPad and passed `scripts/test-device.ps1` for Media and Owlfiles, including another eight MiB hash-verified AFC transfer.
- Debug and Release builds succeeded locally. All eight CTest tests passed. The application MSI, combined setup executable, portable ZIP, and corresponding-source collection were generated.
- Fake-backend filesystem tests cover independent handles, sharing violations, short transfers, offsets beyond 4 GiB, open-file rename, cross-source rename rejection, and read-only access.
- Pairing-store, protocol framing, malformed packet, filename mapping, and MSVC endianness regressions have automated checks.

The original vcpkg libimobiledevice port left `__BYTE_ORDER` undefined under
MSVC. Its 64-bit little-endian conversions consequently byte-swapped AFC packet
fields. A raw AFC probe isolated that failure. The local dependency overlay
corrects the byte order and an automated compile-time test guards it.

## Release acceptance still required

- Clean Windows installation with Apple device packages absent.
- An iPhone and additional iPad models, USB-C and Lightning connections.
- Locked devices, trust revocation, sleep/resume, surprise removal, and simultaneous physical devices.
- Secure Boot verification and driver rollback on those device combinations.
- Installer repair, upgrade, uninstall, and interrupted-install validation on a clean VM.
- Large physical transfers above 4 GiB and broader application save workflows.

Atomic replacement by rename is intentionally rejected. Creation/access/change
time updates are compatibility hints. Application compatibility should be
evaluated against those published semantics.
