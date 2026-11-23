# UDCAP 0.1.8.7 compatibility

The Windows reference installation identifies its desktop executable and assembly as 0.1.8.7. The SteamVR package is registered as `udcap`. The installation includes two overlay executables and a separate Unity hand viewer. Test the behavior of each workflow against the corresponding first-party component and record the hardware and software versions used.

## Current build

| Area | Implemented | Acceptance still needed |
| --- | --- | --- |
| Receiver and radio | Windows COM discovery and wake reconnect, community core probe and telemetry, pairing, desktop channel controls verified by receiver readback, radio signal in both interfaces, vibration | Confirm receiver identities and exercise both gloves, combined and wired devices, reconnect, and radio interference on accessible hardware |
| Calibration | Automatic three-pose capture with spoken prompts and a three-second countdown per pose, UDCAP per-channel flexion/splay grades and CSV export, cancel and recovery, saved joystick center and range | Compare displayed grades and glove response on live captures and check persistence after restarts |
| Desktop | Live hand and data viewers, controller input meters, digital remapping, touch and click threshold editors, five-second joystick shortcut, editable per-glove response curves, joint corrections, four UDCAP tracker presets, and CSV sample export | Compare live settings and response against UDCAP, including all controller inputs and toggle behavior |
| Controllers | Compact and extended joystick frames, controller revision display, separate Controller 1.0 motor tests and Controller 2.0 intensity tests, revision-aware haptics and joystick calibration commands | Exercise both revisions on hardware, including V2 calibration responses, remaining gestures, and actuator behavior |
| Streaming | VMC finger transforms and controller blends, neutral state on connection loss, UDCAP VRChat OSC parameters | Compare motion and input in a VMC or VRChat client using live gloves |
| SteamVR driver | OpenVR controller input, independent joints, complete 31-bone transforms, tracker selection by serial, pose-source telemetry, controller priority and haptic routing | Verify both hands in SteamVR and in games, including alignment and loss of tracking |
| SteamVR dashboard | At-a-glance status for both gloves, shared guided calibration and countdown, cancellation and outcome feedback, quick access to the desktop, and optional SteamVR-managed desktop startup | In-headset readability and input, and desktop startup and shutdown on a running SteamVR installation |
| Firmware and distribution | Firmware version display, MCUboot hash and optional signature verification, native USB and wireless transfers for supported 0.1.8.7 images, USB driver setup through Windows Update, reversible per-user SteamVR setup, verified release upgrades with rollback | Combined receiver and controller firmware, recovery for a device without a verifiable identity, live USB and wireless transfers, clean Windows PnP acceptance, and upgrade from a published release |
| Settings migration | Layer installed desktop and driver defaults, saved desktop preferences and UDCAP SteamVR settings | Compare the imported joystick range, finger response and tracker alignment on live hardware |

## Acceptance matrix

| Area | Reference behavior to match | Acceptance check |
| --- | --- | --- |
| Receiver | Detect left, right, combined, and supported wired receivers; recover after unplug and sleep | Connect and reconnect each supported receiver and glove combination without restarting the app |
| Telemetry | Report connection, hand, battery, frame rate, RSSI, channel, and firmware version | Compare at the same battery and radio conditions, including stale telemetry |
| Radio | Pair individual or both gloves and select or inspect channels | Pair on real hardware, verify changed channels, then restore original pairing and channels |
| Firmware | Inspect, update, and recover receiver and glove firmware | Establish update protocol and backup/recovery procedure, then test with dedicated hardware |
| Calibration | Three hand poses, joystick center and range, quick calibration, persistence, and prompts | Compare output through the full finger and joystick range, including restart and recalibration |
| Hand settings | Finger response, per-joint correction, calibration quality and data export | Compare edited values, motion, and saved results |
| Controller | Analog and digital controls, SteamVR binding paths, gesture shortcuts, enable toggles, haptics | Exercise both controller revisions and verify bindings in a real SteamVR application |
| Spatial tracking | Resolve trackers or controllers, apply per-hand transforms and priority, handle tracking loss | Compare physical poses across Lighthouse and inside-out setups, including lost tracking and reassignment |
| Hand viewer | Live hand and data views, camera controls, detachable window and data export | Compare live movement and exported fields |
| Streaming | OSC and VMC configuration and output | Verify message addresses, units, coordinate systems, update rate and receiver compatibility |
| SteamVR driver | Register, expose two hand devices, update skeleton, inputs and haptics | Run SteamVR with the replacement and compare both hands and bindings in a game |
| SteamVR dashboard | Two-glove status, quick hand calibration, desktop launch, startup behavior | Open and exercise each action in-headset, then restart SteamVR |
| Distribution | Install/update and enable/disable the SteamVR components | Confirm a fresh Windows install, upgrade, removal, and restoration of UDCAP |

## Windows verification

- The receiver-enabled build passes the automated desktop, OpenVR factory, calibration, controller packet, settings, skeleton, firmware image, and VMC mapping checks. An injected send function also verified the VMC packet encoding and the reset on disconnect. UDP delivery to a real client still needs a test outside the isolated environment.
- OpenCap installation, reinstall, rollback after a locked SteamVR configuration file, and UDCAP restoration passed with isolated SteamVR profiles. Valve's registration utility saw the expected driver after each switch. Dashboard autostart was verified in configuration and still needs an in-headset check.
- Simulated upgrade and rollback passed with Windows PowerShell 5.1 and PowerShell 7. The three firmware images installed with UDCAP 0.1.8.7 passed complete-file and MCUboot hash checks. USB and wireless transfers were exercised with simulated bootloader acknowledgments, including fragmented, stale, and rejected replies.
- The native OpenCap desktop recognized both receivers and streamed from both Controller 2.0 gloves. The wearer confirmed the high vibration pulse on each glove. Radio changes, Controller 1.0 hardware, and in-game haptics remain open.
- The wearer confirmed the Controller 2.0 System button fix in SteamVR after reporting that the other controller buttons worked. A simulated press and release also verifies that the System button works while A is held.
- The desktop pages and shared two-glove calibration were checked with simulated gloves. In-headset verification is pending for dashboard texture stability and the back-of-hand tracker pose.

## Calibration roadmap

Replace the timed pose workflow with live calibration after the current capture and persistence behavior is verified on both hands.

## Evidence

- [UDEXREAL download and version](https://udexreal.com/pages/setup)
- [UDEXREAL product manual](https://udcap-vr-gloves-document.gitbook.io/udcap)
- [SteamVR/OpenVR driver API](https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md)
- [Community Hand Driver Core](https://github.com/OldestNova/UdCap-Community-HandDriver-Core/tree/14987ad6751761fde0f587f4687cb6aaa8e46414) supplies MIT-licensed transport and calibration code. Its own documented scope excludes OTA.
- [GitHub parity tracker](https://github.com/CrookedToe/OpenCap/issues/1)
