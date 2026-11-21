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

## Try with SteamVR

1. Exit the UDCAP desktop app to release its receiver ports.
2. Start `OpenCap.exe`. Power on the gloves, then run the three-pose calibration for each hand. Use **Set joystick center** with the stick released, then rotate the stick and select **Finish joystick range**.
3. With SteamVR stopped, run `out/OpenCap/Set-SteamVRRegistration.ps1 -InstallPath out/OpenCap`. In SteamVR's add-on settings, disable the UDCAP add-on before using OpenCap to avoid duplicate controllers.
4. Start SteamVR. Use **Open in SteamVR** to make the dashboard available. To register it for future sessions, run the registration script with `-Dashboard` while SteamVR is running.
5. Assign a tracker to each hand in SteamVR or enter a tracker serial or device index in OpenCap. Position offsets are in metres and rotation offsets are in degrees.

Use **Map inputs** for per-hand button assignments, **Joint correction** for finger adjustments, and **Open hand viewer** for an orbitable model and live joint data. F1 starts guided calibration in the desktop window. Enable the F1 checkbox to use it while another program is focused.

To remove the driver, run the registration script with `-Remove`. Add `-Dashboard` while SteamVR is running to remove dashboard registration too. Restore the UDCAP add-on in SteamVR if switching back.

The desktop process owns the receiver connections and publishes the live glove state. The SteamVR driver consumes state and returns vibration requests. The dashboard uses the same commands as the desktop. All three run in the same Windows session.

## Dependencies

- [OpenVR SDK](https://github.com/ValveSoftware/openvr/tree/0924064316de3effbcd1acf1e309182a2deb1c05) supplies the SteamVR interfaces and reference hand geometry.
- [Community Hand Driver Core](https://github.com/OldestNova/UdCap-Community-HandDriver-Core/tree/14987ad6751761fde0f587f4687cb6aaa8e46414) supplies receiver communication and calibration.
- [Parity issue](https://github.com/CrookedToe/OpenCap/issues/1) tracks hardware and runtime acceptance.
