# PS4 Bluetooth Audio (experimental firmware support)

Homebrew for a **jailbroken PS4** running GoldHEN. Sends game audio from the console's built-in Bluetooth radio straight to Bluetooth Classic headphones—no dongle or PC in the audio path. Output only (no microphone).

> **Experimental.** Discovers kernel symbols and patch offsets dynamically at runtime instead of using hardcoded firmware tables (verifies unique kernel instruction signatures before patching and aborts cleanly on mismatches). Patches live in RAM only and disappear on reboot. **Tested upstream on firmware v12.50, v13.52, and v14.02.** Firmware 9.00 is not validated by this source package.

## Install

Requirements: jailbroken PS4, **GoldHEN** with **FTP (port 2121)** and **BinLoader (port 9090)** enabled.

### Option A: Home-screen app (PKG)

`BTAudio4-1.5.1.pkg` installs **BTAudio4** (`BTAD00001`) and bundles the payload.

1. Copy `BTAudio4-1.5.1.pkg` to `/data/pkg/BTAudio4-1.5.1.pkg` via FTP (or install from USB).
2. On the PS4, open **GoldHEN → Debug Settings → Package Installer** and install the PKG. (Delete any older version from the home screen first.)
3. Ensure **BinLoader (port 9090)** is enabled, then launch **Bluetooth Audio** from the home screen.

### Option B: Payload only

Send the flat payload directly from your PC (do not open a bare connection to port 9090, as BinLoader only accepts one payload per jailbreak):

```powershell
python tools/deploy_variant.py build/BTAudio4-1.5.0.bin --host <PS4_IP> --connect
```

Optional GoldHEN AutoRun install:

```powershell
python tools/install_bta.py --host <PS4_IP> --payload build/BTAudio4-1.5.0.bin
python tools/install_bta.py --host <PS4_IP> --disable-autorun   # undo
```

## Usage

Put your headphones in pairing mode, then pair and connect using either:
- **Home-screen app:** `Triangle` scan/disconnect, `D-pad` select, `X` connect/start service, `Square` toggle TV sound, `Circle` close app (service stays running).
- **Web UI:** `http://<PS4_IP>:8097/` from any browser on your local network (no authentication—use on a trusted network only).

**Tips:**
- If a pair connects with no sound, try reconnecting or toggling **Joint Stereo** off per headphone in the web UI.
- In this 1.5.1 source patch, HDMI audio remains active while Bluetooth streams to preserve the PS4 capture mix. Mute the monitor itself for earbud-only listening.
- If a pair won't appear in scans, remove it in **Settings → Devices → Bluetooth Devices** first, then put it back in pairing mode.
- Runtime state and logs live in `/data/bta_audio/` (check `/data/bta_audio/app.log` if the service fails to start).

## Files

| Path | Description |
|---|---|
| `build/BTAudio4-1.5.1.pkg` | Built home-screen PKG (bundles `BTA-Audio.bin`) |
| `build/BTAudio4-1.5.1.bin` | Built flat payload |
| `src/payload/` | Payload source (launcher, A2DP worker, web UI, dynamic kernel resolver, device cache, BT accept patch) |
| `src/native/` | OpenOrbis home-screen app source and asset generator |
| `tools/` | Build, FTP install, and payload deploy scripts |

## Build from source

Requires `..\toolchain` beside the repo (`toolchain/llvm/bin` and `toolchain/OpenOrbis/PS4Toolchain`), Python (with Pillow + matplotlib), and .NET for `PkgTool.Core`:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_app.ps1      # builds build/BTAudio4-1.5.0.bin
powershell -ExecutionPolicy Bypass -File tools/build_native.ps1   # builds build/BTAudio4-1.5.0.pkg
```

## Credits & Licenses

- Font assets: DejaVu (`src/native/LICENSE_DEJAVU.txt`).
- AutoRun entry format: documented in [orbisRPC](https://github.com/SirHumza/orbisRPC/tree/main/installer).
- Requires [GoldHEN](https://github.com/GoldHEN/GoldHEN) (not included).
- The PKG bundles OpenOrbis sample modules (`libc.prx`, `libSceFios2.prx`, `right.sprx`) from [OpenOrbis-PS4-Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
- Project code is released under the [MIT License](LICENSE).
