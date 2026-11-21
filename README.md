# OpenCap

OpenCap is an MIT-licensed Windows compatibility project for UDEXREAL UDCAP VR gloves. It includes a desktop control app, an OpenVR driver, and a SteamVR dashboard window. The target is feature parity with UDCAP 0.1.8.7. See the [compatibility matrix](docs/parity.md) for implementation and verification status.

## Build and test

Install Visual Studio 2022 with Desktop development with C++, a Windows SDK, CMake 3.24 or later, and Git. Configuration downloads the pinned OpenVR SDK and Community Hand Driver Core. The community core resolves its own build dependencies.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix out/OpenCap
```

Use `-DOPENCAP_RECEIVER=OFF` for a build without hardware transport, or pass `-DOPENVR_SDK_ROOT=C:\path\to\openvr` to build against an existing SDK checkout. Start `OpenCap.exe --demo` to exercise the desktop and shared commands with simulated data. Settings are stored under `%LOCALAPPDATA%\OpenCap`, with separate settings for demo mode.

## Install and upgrade

Run `Install-OpenCap.ps1` from an extracted Windows package to install it for the current Windows user and add a Start menu shortcut. Close the desktop and dashboard and stop SteamVR before replacing an installed package. Run the script from a newly extracted package to upgrade. `-Action Rollback` restores the previous package and `-Action Remove` removes the installed application. The SteamVR registration commands below apply to the chosen installation directory.

## Try with SteamVR

OpenCap imports compatible settings from an installed UDCAP driver on the first normal launch. It layers your saved UDCAP desktop settings and the active UDCAP SteamVR settings over the installed defaults. **Import UDCAP settings** refreshes those values. Keep your existing glove pairing and power on each glove so OpenCap can apply its controller preferences.

The import covers joystick center, range and dead zone, trigger, grip and trackpad settings, finger response curves, per-hand vibration, tracker offsets and priority, skeleton-only mode, and VMC and OSC destinations. Hand-pose calibration uses OpenCap's separate workflow. OpenCap stores the imported data in its own settings and glove preferences.

1. Exit the UDCAP desktop app to release its receiver ports.
2. Start `OpenCap.exe`. Power on the gloves, then run the three-pose calibration for each hand. Use **Set joystick center** with the stick released, then rotate the stick and select **Finish joystick range**.
3. With SteamVR stopped, run `out/OpenCap/Set-SteamVRRegistration.ps1 -InstallPath out/OpenCap`. In SteamVR's add-on settings, disable the UDCAP add-on before using OpenCap to avoid duplicate controllers.
4. Start SteamVR. Use **Open in SteamVR** to make the dashboard available. To register it for future sessions, run the registration script with `-Dashboard` while SteamVR is running.
5. Assign a tracker to each hand in SteamVR or enter a tracker serial or device index in OpenCap. Position offsets are in metres and rotation offsets are in degrees.

Use **Controller** to set per-hand button assignments and touch and click thresholds, **Joint correction** for finger adjustments, and **Open hand viewer** for an orbitable model and live joint data. Enable **Hold stick to toggle joystick** to use the five-second shortcut while wearing the gloves. F1 starts guided calibration in the desktop window. Enable the F1 checkbox to use it while another program is focused.

The tracker presets apply starting offsets for VIVE Tracker, Tundra Tracker, Quest controller, or VIVE Ultimate Tracker. They preserve your tracker assignments. With SteamVR and the OpenCap driver running, the Tracker ID menu lists connected controllers and trackers by serial. The desktop and dashboard show the active pose source.

OpenCap saves the three hand poses for each glove. The hand viewer's data mode shows the measured span for each of the 12 sensor channels and highlights spans below 10 counts. Canceling or failing a new capture restores the last good calibration. **Inspect firmware image** reads the version and checks the payload hash of a UDCAP MCUboot file. Firmware transfers and recovery are tracked in [issue #4](https://github.com/CrookedToe/OpenCap/issues/4).

To remove the driver, run the registration script with `-Remove`. Add `-Dashboard` while SteamVR is running to remove dashboard registration too. Restore the UDCAP add-on in SteamVR if switching back.

The desktop process owns the receiver connections and publishes the live glove state. The SteamVR driver consumes state and returns vibration requests. The dashboard uses the same commands as the desktop. All three run in the same Windows session. Register the dashboard and enable **Start and stop with SteamVR** in OpenCap to have it launch a desktop session. A desktop you started separately can keep running after SteamVR closes.

## Dependencies

- [OpenVR SDK](https://github.com/ValveSoftware/openvr/tree/0924064316de3effbcd1acf1e309182a2deb1c05) supplies the SteamVR interfaces and reference hand geometry.
- [Community Hand Driver Core](https://github.com/OldestNova/UdCap-Community-HandDriver-Core/tree/14987ad6751761fde0f587f4687cb6aaa8e46414) supplies receiver communication and calibration.
- [Parity issue](https://github.com/CrookedToe/OpenCap/issues/1) tracks hardware and runtime acceptance.
