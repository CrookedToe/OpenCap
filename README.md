# OpenCap

OpenCap is an MIT-licensed Windows compatibility project for UDEXREAL UDCAP VR gloves. It includes a desktop control app, an OpenVR driver, and a SteamVR dashboard window. It does not require the UDCAP desktop app, driver, or updater. The target is feature parity with UDCAP 0.1.8.7. See the [compatibility matrix](docs/parity.md) for implementation and verification status.

## Build and test

Install Visual Studio 2022 with Desktop development with C++, a Windows SDK, CMake 3.24 or later, and Git. Configuration downloads the pinned OpenVR SDK and Community Hand Driver Core. The community core resolves its own build dependencies.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix out/OpenCap
```

Use `-DOPENCAP_RECEIVER=OFF` for a build without hardware transport, or pass `-DOPENVR_SDK_ROOT=C:\path\to\openvr` to build against an existing SDK checkout. Start `OpenCap.exe --demo` to exercise the desktop and shared commands with simulated data. Settings are stored under `%LOCALAPPDATA%\OpenCap`, with separate settings for demo mode.

## Switch SteamVR to OpenCap

Extract the Windows ZIP and double-click `Setup-OpenCap.cmd`. It closes SteamVR and the glove apps, installs the OpenCap desktop app, SteamVR driver, and dashboard for your Windows account, then starts SteamVR. The OpenCap dashboard starts the desktop app automatically. Setup saves and disables the existing UDCAP driver and overlay registrations. Your UDCAP files, glove pairing, and calibration stay in place.

On a fresh Windows installation, connect the receiver before running setup. If Windows reports a missing driver, setup uses Windows Update to install the matching USB driver and asks for administrator approval for that step. **Start and stop with SteamVR** controls the desktop app after setup.

To remove OpenCap and restore your previous UDCAP SteamVR setup, double-click `Restore-UDCAP.cmd` from the same extracted package. It restores the UDCAP registrations and startup settings, removes the installed OpenCap files and Start menu shortcut, and starts SteamVR again. OpenCap calibration and preferences are kept for the next test.

Run setup from a newly extracted package to upgrade while keeping the saved UDCAP configuration. Run `Setup-OpenCap.ps1` directly if you want to leave SteamVR closed afterward. `Install-OpenCap.ps1` manages the application files for standalone installations and supports `-Action Rollback`.

## Try with SteamVR

OpenCap imports compatible settings from an installed UDCAP driver on the first normal launch. It layers your saved UDCAP desktop settings and the active UDCAP SteamVR settings over the installed defaults. **Import UDCAP settings** refreshes those values. Keep your existing glove pairing and power on each glove so OpenCap can apply its controller preferences.

The import covers joystick center, range and dead zone, trigger, grip and trackpad settings, finger response curves, per-hand vibration, tracker offsets and priority, skeleton-only mode, and VMC and OSC destinations. Hand-pose calibration uses OpenCap's separate workflow. OpenCap stores the imported data in its own settings and glove preferences.

1. Run `Setup-OpenCap.cmd` to switch the active SteamVR integration.
2. Power on both gloves and click **Calibrate both gloves**. Each pose has a three-second countdown. For the second pose, straighten your fingers and bring them together for zero splay. For the third, keep the fingers straight and spread them comfortably for full splay. Hold each pose until OpenCap captures it and advances. For joystick calibration, select a hand, use **Set joystick center** with the stick released, then rotate the stick and select **Finish joystick range**.
3. Assign a tracker to each hand in SteamVR or enter its serial or device index in OpenCap. For a tracker mounted on the back of the hand, the alignment places the SteamVR grip pose relative to the tracker. Position uses metres along the tracker’s local axes and rotation uses degrees. The wrist is part of the hand skeleton.

Use **Controller** to see live input and set per-hand button assignments and touch and click thresholds. Use **Hand response** to adjust a calibrated glove’s finger ranges and response curves, **Joint correction** for per-joint rotation, and **Open hand viewer** for an orbitable model and live joint data. Enable **Hold stick to toggle joystick** to use the five-second shortcut while wearing the gloves. F1 starts guided calibration in the desktop window. Enable the F1 checkbox to use it while another program is focused.

The desktop shows the detected controller revision. Controller 1.0 has separate tests for the two vibration motors. Controller 2.0 has low and high vibration tests. VMC streams finger rotations and controller inputs to the configured destination.

The tracker presets apply starting offsets for VIVE Tracker, Tundra Tracker, Quest controller, or VIVE Ultimate Tracker. They preserve your tracker assignments. With SteamVR and the OpenCap driver running, the desktop **Tracker ID** menu lists connected controllers and trackers by serial. Automatic assignment locks to the selected tracker for the SteamVR session and reacquires the same serial after a dropout. Use the desktop’s **Tracking** page to change the assignment or alignment.

OpenCap saves the three hand poses for each glove. **Export calibration** writes the raw captures, movement, and grade for each sensor channel to CSV. The hand viewer uses the UDCAP 0.1.8.7 English grades Poor, Average, Good, and Excellent, with movement thresholds specific to each channel. Canceling or failing a new capture restores the last good calibration. **Inspect firmware image** reads the version and checks the SHA-256 hash of a UDCAP MCUboot file. To verify the RSA-2048 PSS or ECDSA P-256 signature, select the matching trusted signing public key in PEM or DER format when prompted.

**Firmware updates** transfers a supported firmware `.bin` file directly to the hardware. USB supports the original 0.1.8.7 left glove, right glove, and single-hand receiver images. Wireless supports the two glove images and verifies the connected glove’s serial and hand. Supply the image separately. OpenCap checks the complete file hash before writing. Combined-receiver and controller images and recovery without a verifiable device identity remain open. [Firmware tracking issue](https://github.com/CrookedToe/OpenCap/issues/4)

The SteamVR dashboard shows connection, battery, radio signal, tracker lock, and calibration status for both gloves. Select **Calibrate gloves** and follow the shared countdown in the headset. Use **Open desktop** for pairing, joystick calibration, tracker alignment, vibration tests, and other settings.

The desktop groups glove and joystick calibration under **Gloves**, pairing and vibration under **Devices**, and tracker assignment and alignment under **Tracking**. **Streaming** holds the VMC and OSC destinations, and **Settings** holds startup, shortcuts, firmware, and application updates. OpenCap shows a new radio channel after the receiver reports it back.

When SteamVR starts the installed OpenCap dashboard, OpenCap checks GitHub and opens a Windows install prompt if a newer version is available. In **Settings**, enable **Include beta releases** to also receive published prereleases. **Check for updates** uses the same channel. Accepting the prompt downloads and verifies the package before closing OpenCap and SteamVR, then restarts SteamVR after the upgrade. The previous package is available for rollback. Portable copies can use a fresh ZIP from [Releases](https://github.com/CrookedToe/OpenCap/releases).

The desktop process owns the receiver connections and publishes the live glove state. The SteamVR driver consumes state and returns vibration requests. The dashboard uses the same commands as the desktop. All three run in the same Windows session. A desktop you started separately can keep running after SteamVR closes. For manual or portable installations, `Set-SteamVRRegistration.ps1` supports separate driver and dashboard registration.

## Dependencies

- [OpenVR SDK](https://github.com/ValveSoftware/openvr/tree/0924064316de3effbcd1acf1e309182a2deb1c05) supplies the SteamVR interfaces and reference hand geometry.
- [Community Hand Driver Core](https://github.com/OldestNova/UdCap-Community-HandDriver-Core/tree/14987ad6751761fde0f587f4687cb6aaa8e46414) supplies receiver communication and calibration.
- [Parity issue](https://github.com/CrookedToe/OpenCap/issues/1) tracks hardware and runtime acceptance.
