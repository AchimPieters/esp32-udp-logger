# esp32-udp-logger

A **zero-config**, **end-user friendly** UDP logger component for **ESP-IDF**.

## What you get

✅ **Only one line in your firmware:**

```c
#include "esp32_udp_logger.h"
```

Nothing else required.

Once the ESP32 gets an IP (Wi‑Fi STA or Ethernet):

- **All `ESP_LOGx()` output is mirrored to UDP** (original output stays active too)
- Logs are sent by default to **UDP broadcast** on port **9999**
- Each device gets a unique hostname: **`esp32-udp-logger-XXXX.local`**
- The device listens for commands on UDP port **9998** (bind-to-me selection)

## Default ports

- TX (logs): **9999**
- RX (commands): **9998**

You can change these via `menuconfig`:
`Component config → esp32-udp-logger`.

---

# Quick start (single device)

## 1) Add the component

### Option A — local component
Copy the folder `esp32-udp-logger/` into your project:

```
your_project/
  components/
    esp32-udp-logger/    <- copy this folder
  main/
  CMakeLists.txt
  ...
```

### Option B — ESP Component Registry
Publish it, then in your project's `idf_component.yml` add:

```yaml
dependencies:
  YOUR_NAMESPACE/esp32-udp-logger: "^1.0.0"
```

## 2) Add one include line to your code

Add this **once** anywhere in your project (for example in `main/app_main.c`):

```c
#include "esp32_udp_logger.h"
```

That’s it.

## 3) Build & flash
```bash
idf.py build flash monitor
```

(You can close the serial monitor after Wi‑Fi is up; UDP logging keeps working.)

## 4) Receive logs on your PC

### macOS / Linux (netcat)
```bash
nc -klu 9999
```

### Windows
**PowerShell** (built-in on many systems):
```powershell
# Simple UDP listener using .NET
$port=9999
$udp = New-Object System.Net.Sockets.UdpClient($port)
$ep  = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any,0)
Write-Host "Listening on UDP $port ..."
while($true){
  $bytes = $udp.Receive([ref]$ep)
  $text  = [System.Text.Encoding]::UTF8.GetString($bytes)
  Write-Host $text
}
```

> Tip: on Windows you can also use Wireshark, or install `ncat` (Nmap) for a netcat-like listener.

---

# Multiple ESP32 devices: select one ("bind-to-me")

If you have multiple ESP32 devices broadcasting logs, you can **select** one device and make it send logs **only to your PC** (unicast).

## 1) Find the device hostname
Each device sets a unique hostname like:

- `esp32-udp-logger-7A3F.local`
- `esp32-udp-logger-19B2.local`

(XXXX are the last bytes of the MAC.)

## 2) Start your receiver on your PC
```bash
nc -klu 9999
```

## 3) Bind one device to your PC
You send a UDP command to port **9998** of the device:

### macOS / Linux
```bash
echo "bind YOUR_PC_IP 9999" | nc -u -w1 esp32-udp-logger-7A3F.local 9998
```

### Windows (PowerShell)
```powershell
$device="esp32-udp-logger-7A3F.local"
$rxPort=9998
$pcIp="YOUR_PC_IP"
$txPort=9999
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Send([Text.Encoding]::UTF8.GetBytes("bind $pcIp $txPort"), ("bind $pcIp $txPort").Length, $device, $rxPort) | Out-Null
$udp.Close()
```

After binding:
- that ESP32 will send logs **unicast** to your PC
- optionally turn off broadcast for that device:

```bash
echo "broadcast off" | nc -u -w1 esp32-udp-logger-7A3F.local 9998
```

## 4) Status / Unbind

Status:
```bash
echo "status" | nc -u -w1 esp32-udp-logger-7A3F.local 9998
```

Unbind (back to broadcast):
```bash
echo "unbind" | nc -u -w1 esp32-udp-logger-7A3F.local 9998
```

---

# Super easy mode: cross-platform CLI (recommended)

This repo includes a Python CLI that works on **macOS / Linux / Windows**.

## Install
```bash
python -m pip install zeroconf
```

## List devices
```bash
python tools/esp32_udp_logger_cli.py list
```

## Interactive picker
```bash
python tools/esp32_udp_logger_cli.py pick
```

## Bind device
```bash
python tools/esp32_udp_logger_cli.py bind esp32-udp-logger-7A3F
```

## Listen logs (built-in)
```bash
python tools/esp32_udp_logger_cli.py listen --port 9999
```

---

# Commands reference (UDP RX port 9998)

Commands are simple ASCII:

- `bind <ipv4> <port>`  → switch to unicast destination
- `unbind`              → switch back to broadcast
- `broadcast on|off`    → enable/disable broadcast sending
- `status`              → get current mode + drop count

Example:
```bash
echo "bind 192.168.1.10 9999" | nc -u -w1 esp32-udp-logger-7A3F.local 9998
```

---

# Troubleshooting

## I see no logs
- Make sure your ESP32 actually has Wi‑Fi/Ethernet and got an IP
- Make sure your PC is on the same subnet (broadcast is subnet-local)
- Try the Python listener:
  ```bash
  python tools/esp32_udp_logger_cli.py listen
  ```

## `list` shows no devices
mDNS can be blocked:
- macOS: usually works out of the box
- Linux: ensure Avahi is running (common on desktop distros)
- Windows: allow Python through firewall on Private networks

## I have Wi‑Fi but broadcast doesn’t arrive
Some networks block broadcast/multicast. In that case:
- bind the device to your PC by IP (unicast):
  ```bash
  python tools/esp32_udp_logger_cli.py bind esp32-udp-logger-7A3F --pc-ip YOUR_PC_IP
  ```
- or update your router/AP settings.

---

# License
MIT
