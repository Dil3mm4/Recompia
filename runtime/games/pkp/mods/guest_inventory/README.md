# Guest inventory (Modtopia package)

A standalone, opt-in Link Play inventory package for game build 2.0.1. Visitors carry their offline bag, send changes to the host for authoritative item consumption, and restore their bag after returning home. It uses Modtopia's reliable in-session transport: both LAN and relay use the current Link Play session rather than a separate UDP bridge.

## Compatibility and installation

Requires a Recompia build exposing **Modtopia API 0.8**. The native mod exports `modtopia_api_version = 0x00000008`; an older loader rejects it. The code also checks `MtHost.size` and all session callbacks before registering hooks. Existing 0.7 mods remain ABI-compatible because API 0.8 only appends fields to the function tables.

The manifest targets **main build 76a489f3bd3642fa5e18bd83d32c98ba** (Pokopia 2.0.1). Modtopia validates all ten instruction words and requires the game to be relinked with those hook points; it does not require translating shaders again.

Install the `.pkmod` under the game's `mods/` folder, approve its native code in Recompia's Mods page, and enable the mod **on both Link Play peers**. This enabled state is the sole toggle, so Recompia includes `guest_inventory@0.1.0` in the existing multiplayer gameplay-mod compatibility fingerprint. Disable it in the Mods page to restore normal Link Play behavior at the next launch. The mod is not intended for seamless co-op mode.

## Build and package

```sh
cmake -S runtime/games/pkp/mods/guest_inventory -B /tmp/guest-inventory-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/guest-inventory-build
python3 runtime/games/pkp/mods/guest_inventory/package.py --macos-arm64 /tmp/guest-inventory-build/mod.dylib --output guest_inventory-0.1.0.pkmod
```

To build Windows, use the same CMake source directory with MSVC and a Windows-x64 target. Package with `--windows-x64 path/to/mod.dll`; pass both platform switches to combine them in one archive. The ZIP contains `mod.toml` and the appropriate `bin/<platform>/mod.<extension>` paths, as required by Modtopia. No game binaries, ROMs, shaders, saves, or user data are included.

## Compatibility verification

The host uses Modtopia's append-only major/minor ABI policy. Verify that `MT_API_VERSION_MINOR == 8`, all four new host and backend callbacks are appended after the 0.7 tail, and a 0.8 package cannot start on an older 0.7 host. Verify that the manifest scans with exactly ten hook points for game version 2.0.1.

Multiplayer acceptance tests still needed on a game build linked with the new hook plan: visitor starts with existing offline bag; travels to host; uses and picks up items; host confirms transactions; visitor returns home without losing items; disconnect/rejoin; multiple visitors; LAN and relayed sessions; both peers with mod enabled vs one peer disabled. Old releases cannot load the package.
