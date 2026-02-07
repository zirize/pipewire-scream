# Scream PipeWire Sender Module

A PipeWire module that creates a virtual audio sink and transmits audio over the network using the Scream protocol.

**Part of the [Scream](https://github.com/duncanthrax/scream) ecosystem** - This is a Linux PipeWire implementation of the Scream sender. For receivers and the original Windows driver, see the [main Scream project](https://github.com/duncanthrax/scream).

## ⚠️ AI-Generated Code Notice

**This project was automatically generated using GitHub Copilot CLI with minimal human intervention.**

- The source code, build system, and documentation were created through AI assistance
- While functional and tested, users should exercise caution and review the code before use in production environments
- Contributions, code reviews, and bug reports are especially welcome to improve reliability
- No warranty is provided - use at your own risk (see LICENSE for details)

If you encounter issues, please report them on the [GitHub Issues](https://github.com/zirize/pipewire-scream/issues) page.

## Overview

This module provides Linux/PipeWire equivalent functionality to the Windows Scream driver. Audio played to the virtual sink is transmitted as raw PCM over UDP (multicast or unicast) to Scream receivers on the network.

**Status**: ✅ **Tested and Working** (PipeWire 1.0.5)

## Features

- Virtual audio sink in PipeWire
- Multicast (default: 239.255.77.77:4010) and unicast support
- Configurable sample rates, bit depths, and channel configurations
- Low latency audio transmission
- Compatible with existing Scream receivers
- Stable for long-duration playback

## Dependencies

- PipeWire >= 0.3.0
- CMake >= 3.10
- GCC or Clang with C11 support

### Ubuntu/Debian
```bash
sudo apt-get install libpipewire-0.3-dev pipewire cmake build-essential
```

### Fedora/RHEL
```bash
sudo dnf install pipewire-devel cmake gcc
```

### Arch Linux
```bash
sudo pacman -S pipewire cmake gcc
```

## Build

```bash
mkdir build && cd build
cmake ..
make
sudo make install
```

The module will be installed to the PipeWire modules directory (typically `/usr/lib/x86_64-linux-gnu/pipewire-0.3/` or `/usr/lib/pipewire-0.3/`).

## Usage

### Automatic loading on PipeWire startup (Recommended)

1. Create config directory:
```bash
mkdir -p ~/.config/pipewire/pipewire.conf.d/
```

2. Copy and edit the configuration file:
```bash
cp scream-sender.conf.example ~/.config/pipewire/pipewire.conf.d/scream-sender.conf
nano ~/.config/pipewire/pipewire.conf.d/scream-sender.conf
```

3. Restart PipeWire:
```bash
systemctl --user restart pipewire
```

### Configuration Examples

**Multicast (default)**:
```
context.modules = [
    { name = libpipewire-module-scream }
]
```

**Unicast to specific receiver**:
```
context.modules = [
    {
        name = libpipewire-module-scream
        args = {
            ip = "192.168.1.100"
        }
    }
]
```

**High quality audio**:
```
context.modules = [
    {
        name = libpipewire-module-scream
        args = {
            rate = 96000
            format = "S24LE"
        }
    }
]
```

## Module Arguments

| Argument | Type | Default | Description |
|----------|------|---------|-------------|
| `sink.name` | string | `Scream` | Name of the virtual sink |
| `sink.description` | string | `Scream Network Sink` | Description shown in audio settings |
| `ip` | string | `239.255.77.77` | Destination IP (multicast or unicast) |
| `port` | int | `4010` | Destination UDP port (1-65535) |
| `interface` | string | (auto) | Network interface to use |
| `multicast.ttl` | int | `1` | Multicast TTL (1-255) |
| `rate` | int | `48000` | Default sample rate (8000-384000) |
| `channels` | int | `2` | Default channel count (1-255) |
| `format` | string | `S16LE` | Audio format (S16LE, S24LE, S32LE) |

## Receivers

To receive the audio stream, use any Scream receiver:

### Linux
Use the Scream Unix receiver from the main Scream repository:
```bash
# See: https://github.com/duncanthrax/scream
# Build and run
./scream -o pulse
```

### Windows
Use ScreamReader (included in the Windows Scream package).

## Network Configuration

Receivers must open UDP port 4010 (or your custom port) in their firewall.

For multicast, ensure IGMP is working on your network and the multicast route is set:
```bash
# Check multicast route
ip route show

# Add multicast route if needed
sudo ip route add 224.0.0.0/4 dev eth0
```

## Troubleshooting

### Module fails to load
- Check PipeWire logs: `journalctl --user -u pipewire -f`
- Verify module path: `find /usr/lib* -name "*scream*"`

### No audio transmitted
- Check firewall settings
- Verify network interface is correct
- Test with: `sudo tcpdump -i any udp port 4010`

### Audio quality issues
- Try different sample rates (44100, 48000)
- Reduce channel count if bandwidth is limited
- Check network latency with `ping`

## Performance Notes

- Default configuration (48kHz, 16-bit, stereo) uses ~1.5 Mbit/s
- Higher sample rates and bit depths increase bandwidth proportionally
- Network jitter can cause audio dropouts - use wired connections when possible
- Tested stable with long-duration music playback

## Testing

### Verify Module is Loaded
```bash
pactl list sinks short | grep Scream
# or
pw-dump | jq '.[] | select(.info.props["node.name"] == "Scream")'
```

### Test Audio Playback
```bash
# Play a test file
paplay --device=Scream /path/to/audio.wav

# Stream music
mpv --audio-device=pipewire/Scream music.mp3
```

### Monitor Logs
```bash
# Real-time PipeWire logs
journalctl --user -u pipewire -f

# Check for Scream module messages
journalctl --user -u pipewire | grep -i scream
```

## Known Issues

- Short WAV files may have slight noise (depends on source file quality)
- PipeWire < 1.0 may require different API calls (untested)

## Contributing

Contributions are welcome! Please test thoroughly and document any changes.

## License

MS-PL (Microsoft Public License) - same as the main Scream project.
