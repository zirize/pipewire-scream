# Quick Start Guide

## Prerequisites

Install PipeWire development files:

```bash
# Ubuntu/Debian
sudo apt-get install libpipewire-0.3-dev pipewire cmake build-essential

# Fedora/RHEL
sudo dnf install pipewire-devel cmake gcc

# Arch Linux
sudo pacman -S pipewire cmake gcc
```

## Build and Install

```bash
sudo ./install.sh
```

Or manually:

```bash
mkdir build && cd build
cmake ..
make
sudo make install
```

## Quick Test

### 1. Start a receiver on another machine

On a Linux machine (or the same machine for testing):

```bash
# Download from https://github.com/duncanthrax/scream
./scream -o pulse
```

### 2. Configure and load the PipeWire module

```bash
# Create config directory
mkdir -p ~/.config/pipewire/pipewire.conf.d/

# Copy example config
cp scream-sender.conf.example ~/.config/pipewire/pipewire.conf.d/scream-sender.conf

# Restart PipeWire to load the module
systemctl --user restart pipewire
```

### 3. Check that the sink was created

```bash
pactl list sinks short | grep Scream
```

Or use `pavucontrol` (PulseAudio Volume Control) to see the "Scream" sink.

### 4. Play audio to the Scream sink

```bash
# Set as default sink
pactl set-default-sink Scream

# Play a test file
paplay /usr/share/sounds/alsa/Front_Center.wav

# Or use speaker-test
speaker-test -D pulse -c 2 -t sine
```

### 5. Disable the module when done

```bash
# Remove or rename the config file
mv ~/.config/pipewire/pipewire.conf.d/scream-sender.conf ~/.config/pipewire/pipewire.conf.d/scream-sender.conf.disabled

# Restart PipeWire
systemctl --user restart pipewire
```

## Troubleshooting

### Module fails to load

Check PipeWire logs:
```bash
journalctl --user -u pipewire -f
```

### No audio received

1. Check that receiver is running and listening
2. Verify network connectivity:
   ```bash
   sudo tcpdump -i any udp port 4010
   ```
3. Check firewall settings on receiver
4. Verify multicast routing:
   ```bash
   ip route show
   # Should show a route for 224.0.0.0/4
   ```

### Poor audio quality

- Try lower sample rate (44100 Hz)
- Use wired network instead of WiFi
- Check network latency with `ping`

### Permission issues

If the module fails to create socket, check that your user has network permissions.

## Configuration

For permanent configuration, copy the example config:

```bash
mkdir -p ~/.config/pipewire/pipewire.conf.d/
cp scream-sender.conf.example ~/.config/pipewire/pipewire.conf.d/scream-sender.conf
# Edit the file as needed
nano ~/.config/pipewire/pipewire.conf.d/scream-sender.conf
systemctl --user restart pipewire
```

## Network Setup

### Multicast (default)

Ensure multicast is enabled on your network interface:

```bash
# Check current routes
ip route show

# Add multicast route if needed (eth0 example)
sudo ip route add 224.0.0.0/4 dev eth0
```

### Unicast

To send to a specific receiver, edit `~/.config/pipewire/pipewire.conf.d/scream-sender.conf`:

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

Then restart PipeWire: `systemctl --user restart pipewire`

## Advanced Usage

### Multiple sinks with different configurations

Create multiple config files:

```bash
# ~/.config/pipewire/pipewire.conf.d/scream-high-quality.conf
context.modules = [
    {
        name = libpipewire-module-scream
        args = {
            sink.name = "Scream-HQ"
            rate = 96000
            format = "S24LE"
        }
    }
]

# ~/.config/pipewire/pipewire.conf.d/scream-living-room.conf
context.modules = [
    {
        name = libpipewire-module-scream
        args = {
            sink.name = "Living-Room"
            ip = "192.168.1.50"
        }
    }
]
```

### Debugging

Enable verbose logging in PipeWire config:

```bash
# In ~/.config/pipewire/pipewire.conf
context.properties = {
    log.level = 2  # 0=error, 1=warn, 2=info, 3=debug, 4=trace
}
```

Then check logs:
```bash
journalctl --user -u pipewire -f | grep scream
```
