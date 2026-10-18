# Deploying CraftPP on Raspberry Pi (Linux aarch64)

CraftPP is engineered to run at 20 TPS on low-power ARM64 hardware (Raspberry Pi 4 / 5) with minimal memory footprint (~50 MiB RSS) and ~0% idle CPU.

## Prerequisites

- Raspberry Pi OS 64-bit (Debian Bookworm or newer).
- 1 GB+ RAM recommended (works comfortably on 2 GB / 4 GB models).
- Fast microSD card (A2 rated) or USB 3.0 NVMe SSD for fast Anvil chunk I/O.

## Installation

### 1. Download or Build the Binary

You can download prebuilt release binaries from GitHub Releases (`craftpp-linux-aarch64.tar.gz`) or build on device:

```bash
sudo apt update && sudo apt install -y cmake clang git zlib1g-dev libssl-dev
git clone https://github.com/kkorenn/CraftPP.git
cd CraftPP
./build.sh
```

### 2. Configure Server Directory

```bash
sudo useradd -r -m -d /opt/craftpp -s /usr/sbin/nologin craftpp
sudo cp build/craftpp /opt/craftpp/
sudo cp -r data /opt/craftpp/
sudo chown -R craftpp:craftpp /opt/craftpp
```

Accept the Minecraft EULA by creating `/opt/craftpp/eula.txt`:

```text
eula=true
```

## Systemd Service

Create `/etc/systemd/system/craftpp.service`:

```ini
[Unit]
Description=CraftPP Minecraft 26.3 Server
After=network.target

[Service]
Type=simple
User=craftpp
Group=craftpp
WorkingDirectory=/opt/craftpp
ExecStart=/opt/craftpp/craftpp /opt/craftpp/server.properties
Restart=on-failure
RestartSec=10
LimitNOFILE=65536
Nice=-5

[Install]
WantedBy=multi-user.target
```

Enable and start the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now craftpp
```

View live logs:

```bash
journalctl -u craftpp -f
```

## Performance & Storage Tuning for SD Cards

1. **zram Swap**: Enable zram swap for responsive memory paging:
   ```bash
   sudo apt install -y zram-tools
   ```
2. **Mount with `noatime`**: Reduce flash wear on the SD card:
   Edit `/etc/fstab` and ensure the root filesystem uses the `noatime` option.
3. **Autosave Frequency**: Set `auto-save-interval=6000` (5 minutes) in `server.properties` to batch disk flushes.
