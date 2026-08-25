# Debian Server as a Remote Arduino/ESP32 Flashing Station

Host: `debianhomelab` (Debian 12 bookworm) at `192.168.3.17`, user `lincalibur`.
Set up because local USB passthrough to Arduino IDE was unreliable — the server now compiles and flashes over SSH using `arduino-cli`.

## What's already set up on the server

- SSH key auth from this machine (`~/.ssh/id_ed25519`) — no password needed
- `lincalibur` is in the `dialout` group (USB serial access) and `sudo` (passwordless)
- `arduino-cli` installed at `/usr/local/bin/arduino-cli`
- Board cores installed: `arduino:avr` (Uno/Nano/Mega) and `esp32:esp32` (all ESP32 variants)

## 1. Connect

```bash
ssh lincalibur@192.168.3.17
```

No password prompt — the key was installed in `~/.ssh/authorized_keys` on the server.

## 2. Plug in the ESP32

Plug the ESP32 into a USB port **on the Debian server** (not your local machine). Then check it's detected:

```bash
ssh lincalibur@192.168.3.17 "arduino-cli board list"
```

Look for a line like:
```
/dev/ttyUSB0   serial   Espressif ESP32 ...   esp32:esp32:esp32
```

Most ESP32 dev boards use a CP210x or CH340 USB-serial chip and show up as `/dev/ttyUSB0`. Genuine Arduino Uno/Nano/Mega boards usually show up as `/dev/ttyACM0` — and so does the **ESP32-C3**, which has a native USB-JTAG/CDC controller instead of an external USB-serial chip (identifies itself as "USB JTAG/serial debug unit", Espressif vendor ID `303a`).

If nothing shows up: unplug/replug and re-run the command, or check `dmesg | tail -20` on the server for USB enumeration errors.

## 3. Get your sketch onto the server

Arduino sketches need to live in a folder with the **same name as the `.ino` file** (this is an arduino-cli/Arduino IDE requirement).

Option A — write/edit directly on the server:
```bash
ssh lincalibur@192.168.3.17
mkdir -p ~/sketches/MyProject
nano ~/sketches/MyProject/MyProject.ino
```

Option B — write locally and copy over (recommended if you want to keep sketches in this repo):
```bash
scp -r ./my-sketch-folder lincalibur@192.168.3.17:~/sketches/
```
or keep it synced with `rsync`:
```bash
rsync -av --delete ./esp32-code/ lincalibur@192.168.3.17:~/sketches/MyProject/
```

## 4. Compile

```bash
ssh lincalibur@192.168.3.17 "arduino-cli compile --fqbn esp32:esp32:esp32 ~/sketches/MyProject"
```

Common ESP32 board FQBNs (find more with `arduino-cli board listall | grep -i esp32`):

| Board                        | FQBN                              |
|-------------------------------|------------------------------------|
| Generic ESP32 Dev Module      | `esp32:esp32:esp32`               |
| ESP32-S3 Dev Module            | `esp32:esp32:esp32s3`             |
| ESP32-C3 Dev Module            | `esp32:esp32:esp32c3:CDCOnBoot=cdc` (see [USB CDC gotcha](#esp32-c3-usb-cdc-gotcha) below) |
| ESP32-WROOM-32                | `esp32:esp32:esp32`               |

## 5. Upload (flash)

```bash
ssh lincalibur@192.168.3.17 "arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 ~/sketches/MyProject"
```

Adjust `-p /dev/ttyUSB0` to whatever `arduino-cli board list` showed in step 2.

Compile and upload in one line:
```bash
ssh lincalibur@192.168.3.17 "arduino-cli compile --fqbn esp32:esp32:esp32 ~/sketches/MyProject && arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 ~/sketches/MyProject"
```

### ESP32-C3 USB CDC gotcha

The ESP32-C3 has a native USB-JTAG/CDC controller built in (no separate CP210x/CH340 chip). By default, arduino-cli's `esp32:esp32:esp32c3` FQBN builds with **USB CDC On Boot disabled**, which routes `Serial` output to the physical UART0 pins instead of the USB port. Result: the sketch flashes and runs fine, but `arduino-cli monitor` on `/dev/ttyACM0` shows nothing at all — looks like a dead board when it isn't.

Fix: always compile and upload ESP32-C3 sketches with the `CDCOnBoot=cdc` config option appended to the FQBN:

```bash
arduino-cli compile --fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc' ~/sketches/MyProject
arduino-cli upload -p /dev/ttyACM0 --fqbn 'esp32:esp32:esp32c3:CDCOnBoot=cdc' ~/sketches/MyProject
```

(Quote the FQBN — the colon-separated config suffix needs to survive shell parsing over SSH.)

## 6. Monitor serial output

```bash
ssh lincalibur@192.168.3.17 "arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200"
```
Exit with `Ctrl+C`.

## Installing libraries

```bash
ssh lincalibur@192.168.3.17 "arduino-cli lib search <name>"
ssh lincalibur@192.168.3.17 "arduino-cli lib install \"<Library Name>\""
```

## Arduino (AVR) boards instead of ESP32

Same flow, different FQBN and port:
```bash
arduino-cli compile --fqbn arduino:avr:uno ~/sketches/MyProject
arduino-cli upload -p /dev/ttyACM0 --fqbn arduino:avr:uno ~/sketches/MyProject
```

## One-liner shortcut

Add this to your local `~/.bashrc` / `~/.zshrc` for convenience:
```bash
flash-esp32() {
  local sketch="$1"
  local port="${2:-/dev/ttyUSB0}"
  ssh lincalibur@192.168.3.17 "arduino-cli compile --fqbn esp32:esp32:esp32 ~/sketches/$sketch && arduino-cli upload -p $port --fqbn esp32:esp32:esp32 ~/sketches/$sketch"
}
```
Usage: `flash-esp32 MyProject`

## Troubleshooting

- **Permission denied on serial port**: shouldn't happen (`lincalibur` is in `dialout`), but if it does: `ssh lincalibur@192.168.3.17 "groups"` should list `dialout`. Logging out/in again fixes stale group membership after being added.
- **Board not detected**: check `dmesg | tail` on the server after plugging in. If a driver conflict appears (e.g. `brltty` grabbing the device — not currently installed on this server, but can get installed as a dependency later), remove it: `sudo apt remove brltty`.
- **Wrong FQBN**: run `arduino-cli board listall | grep -i <board name>` to find the exact string.
- **ESP32-C3 flashes fine but no serial output**: you forgot `CDCOnBoot=cdc` — see [USB CDC gotcha](#esp32-c3-usb-cdc-gotcha) above.
- **Sudo/root access**: `lincalibur` has passwordless sudo (`/etc/sudoers.d/lincalibur`) for installing new libraries/tools system-wide if ever needed.

## Optional next step: remote editing with a real editor

Instead of `nano` over SSH, you can use **VS Code Remote-SSH** (`Remote-SSH: Connect to Host...` → `lincalibur@192.168.3.17`) to edit sketches with full syntax highlighting directly on the server, then run the compile/upload commands above in the integrated terminal. Ask if you'd like this set up (VS Code + PlatformIO extension is also an option for a more IDE-like ESP32 workflow).
