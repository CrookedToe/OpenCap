# UDCAP 0.1.8.7 compatibility

The Windows reference installation identifies its desktop executable and assembly as 0.1.8.7. The SteamVR package is registered as `udcap`. The installation includes two overlay executables and a separate Unity hand viewer. Test the behavior of each workflow against the corresponding first-party component and record the hardware and software versions used.

## Current build

| Area | Implemented | Acceptance still needed |
| --- | --- | --- |
| Receiver and radio | Community core integration, periodic discovery, glove telemetry, pairing controls, channel selection, vibration | Physical receiver and glove tests, combined receiver behavior, radio recovery and channel scan |
| Calibration | Three-pose captures, saved joystick center and range, timed voice guidance, and raw sensor spans from the last successful captures | Restore three-pose sensor calibration across restarts, match the first-party quality labels and export format, and compare physical poses |
| Desktop | Live hand and data viewers, digital remapping, touch and click threshold editors, five-second joystick shortcut, joint corrections, four UDCAP tracker presets and CSV sample export | Controller revision 2.0 and additional remapping and vibration behaviors, firmware workflows and localization |
| Streaming | Configurable VMC finger rotations and UDCAP VRChat OSC parameters | Verify VMC coordinates and exercise avatar output with live gloves |
| SteamVR driver | OpenVR controller input, independent joints, complete 31-bone transforms, tracker selection by serial, pose-source telemetry, controller priority and haptic routing | Verify both hands in SteamVR and in games, including alignment and loss of tracking |
| SteamVR dashboard | Device status, guided and manual calibration, joystick toggle, vibration test, alignment, presets and optional SteamVR-managed desktop startup | In-headset controls, desktop startup and shutdown on a running SteamVR installation, and the full first-party dashboard inventory |
| Firmware and distribution | Firmware version display, MCUboot file and payload-hash inspection, per-user install, upgrade and rollback, reversible SteamVR registration | Signature validation against a trusted key, firmware transfer and recovery, automatic application updates, and hardware regression checks |
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
| SteamVR dashboard | Device status, quick controls, offset and calibration actions, startup behavior | Open and exercise every action in-headset, then restart SteamVR |
| Distribution | Install/update and enable/disable the SteamVR components, localize the supported workflows | Confirm a fresh Windows install, upgrade, removal, and restoration of UDCAP |

## Evidence

- [UDEXREAL download and version](https://udexreal.com/pages/setup)
- [UDEXREAL product manual](https://udcap-vr-gloves-document.gitbook.io/udcap)
- [SteamVR/OpenVR driver API](https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md)
- [Community Hand Driver Core](https://github.com/OldestNova/UdCap-Community-HandDriver-Core/tree/14987ad6751761fde0f587f4687cb6aaa8e46414) supplies MIT-licensed transport and calibration code. Its own documented scope excludes OTA.
- [GitHub parity tracker](https://github.com/CrookedToe/OpenCap/issues/1)
