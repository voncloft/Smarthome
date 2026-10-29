# SmartHome / Lights

Qt desktop controller for Govee lights and BroadLink infrared remotes.
Govee lighting requires your own API key.

## BroadLink Mini setup

1. Install the Scratch package `python-broadlink` and its dependencies.
2. Connect the Mini to 2.4 GHz Wi-Fi using the BroadLink phone app. The PC can use Ethernet on the same network. Disable **Lock device** in the phone app if local control is blocked.
3. Open **Infrared**, click **Check support**, then **Discover hubs**. **Add by IP address** is available if discovery cannot find it.
4. Select the hub and name its room. For the Acer H6510BD, click **Add Acer projector** to load published codes without the original remote; then click **Test Menu** with the projector already on. For other devices, click **Add remote** and learn their buttons as follows.
5. Select **Menu**, click **Learn button**, and point the original remote at the Mini. Press Menu once while learning is active.
6. Click **Test Menu** with the projector already on to test reception without a power cycle. This button never falls back to Power.

Additional hubs and remotes can be saved for other rooms. Commands go only to the selected remote's hub; rediscovery updates changed addresses while preserving names and buttons. A hub identity check prevents sending to a different device that acquired an old address. Infrared cannot confirm the TV/projector state, and multiple devices within range may respond to identical codes.

Saved hub addresses, room names, and learned codes are stored in `infrared.json` alongside the existing Govee settings. They are separate from `Lights.json` and lighting routines. Learning and sending run asynchronously, and startup does not discover hubs or transmit commands automatically.

## Build and check

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
# Optional system installation (or make install from the build directory):
sudo cmake --install build
```

Tests use simulated hubs and temporary settings, without contacting lights or infrared devices. Python 3 and `broadlink` (including its `cryptography` dependency) are needed for real hub operations. Qt Test is needed only when building tests.

## Changes

### 0.1.3 — 2026-10-29

- Show infrared commands, status, and errors in the original Diagnostics list,
  mixed with light events in the same recent-results history (newest first).
- Remove the separate infrared log box; retain the latest 500 combined events
  and preserve the list when tabs or light verification refresh.

### 0.1.2 — 2026-10-29

- Added a scrolling infrared activity log under Diagnostics, alongside light verification.
- Log sends, learning, discovery, support checks, progress, errors, cancellation, and timeouts with timestamps and target details.
- Keep the latest 500 messages for the current session, with Clear log and automatic scrolling that pauses while reading older entries.
- Preserve the log across tab refreshes. A sent command does not confirm appliance state.

### 0.1.1 — 2026-10-29

- Added local BroadLink Mini discovery, room naming, remote-button learning, and sending.
- Added a dedicated Menu test and per-hub targeting for multiple rooms.
- Added persistent infrared settings, asynchronous operation status, and automated tests.
- Added a CMake install target for the app, launcher, and icon.

Hardware transmission and reception still need to be checked with the Mini.

The Acer preset comes from the [exact H6510BD model record](https://github.com/pickysysadmin/logitech-harmony-ir-archive/blob/main/devices/Acer/H6510BD.json) and its [published Harmony code set](https://github.com/pickysysadmin/logitech-harmony-ir-archive/blob/main/codesets/5b/5b0b82bf412b05cc.json). Original Pronto timings and source identity are retained in `assets/acer-h6510bd.json`. Menu is `0x10C821DE` in that archive's notation; Power is a toggle. These are published candidates, not physically verified codes.
