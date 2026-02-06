# Technical Documentation

## Architecture

The Scream PipeWire Sender Module consists of three main components:

### 1. PipeWire Integration Layer
- Creates a virtual audio sink using PipeWire's stream API
- Handles audio format negotiation (sample rate, channels, bit depth)
- Processes audio buffers in real-time via callback mechanism
- Manages lifecycle (initialization, state changes, cleanup)

### 2. Scream Protocol Handler
- Encodes audio format information into 5-byte Scream header
- Splits audio data into UDP packets (max 1152 bytes payload)
- Handles sample rate encoding (48kHz base vs 44.1kHz base)
- Maps channel configurations to WAVEFORMATEXTENSIBLE masks

### 3. Network Layer
- UDP socket management (multicast or unicast)
- Multicast group joining and TTL configuration
- Network interface selection
- Non-blocking packet transmission

## Audio Processing Flow

```
PipeWire Graph → Virtual Sink → Process Callback → Packetize → UDP Send
                     ↓
              Format Negotiation
                     ↓
              Stream Connected
                     ↓
              Audio Buffers
```

1. **Initialization**: Module creates sink and connects to PipeWire core
2. **Format Negotiation**: Sink advertises supported formats
3. **Stream Connection**: PipeWire connects audio sources to sink
4. **Audio Processing**: Process callback receives audio buffers
5. **Packetization**: Buffers split into Scream protocol packets
6. **Transmission**: Packets sent via UDP to network

## Scream Protocol Details

### Header Format (5 bytes)

```
Byte 0: Sample Rate Encoding
  Bit 7:   Base rate (0=48kHz, 1=44.1kHz)
  Bits 0-6: Multiplier (1-127)
  
  Examples:
    48000 Hz  = 0x01 (base=48k, mult=1)
    96000 Hz  = 0x02 (base=48k, mult=2)
    44100 Hz  = 0x81 (base=44.1k, mult=1)
    88200 Hz  = 0x82 (base=44.1k, mult=2)

Byte 1: Sample Size (bits)
  16, 24, or 32

Byte 2: Channel Count
  1-255

Bytes 3-4: Channel Mask (little-endian)
  WAVEFORMATEXTENSIBLE speaker positions
  
  Common masks:
    Mono:     0x0004 (SPEAKER_FRONT_CENTER)
    Stereo:   0x0003 (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
    5.1:      0x003F
    7.1:      0x00FF
```

### Payload Format

- Raw PCM data in little-endian format
- Interleaved channels (L, R, L, R, ... for stereo)
- Maximum 1152 bytes per packet
- Sample alignment: packets contain whole samples only

### Packet Size Calculation

```c
bytes_per_sample = (sample_size / 8) * channels
max_samples_per_packet = SCREAM_MAX_PAYLOAD / bytes_per_sample
packet_payload = max_samples_per_packet * bytes_per_sample
```

Examples:
- 16-bit stereo: (16/8) * 2 = 4 bytes/sample, 288 samples/packet = 1152 bytes
- 24-bit 5.1:    (24/8) * 6 = 18 bytes/sample, 64 samples/packet = 1152 bytes

## Network Configuration

### Multicast Mode (Default)

- Group: 239.255.77.77 (administratively scoped)
- Port: 4010 (UDP)
- TTL: 1 (link-local, can be increased)
- Requires IGMP support on network

Advantages:
- Multiple receivers can listen simultaneously
- Automatic discovery
- Efficient for multiple receivers

Disadvantages:
- Not all networks support multicast
- Router configuration may be needed
- Potential for packet loss

### Unicast Mode

- Direct IP addressing
- Point-to-point transmission
- No multicast routing required

Advantages:
- Works on all networks
- More reliable delivery
- Easier firewall configuration

Disadvantages:
- One receiver per sender
- Manual IP configuration required

## Performance Considerations

### Latency

Total latency = PipeWire_buffer + Network_latency + Receiver_buffer

Typical values:
- PipeWire buffer: 10-20ms (configurable)
- Network latency: 1-10ms (LAN), 10-100ms (WiFi)
- Receiver buffer: 50-200ms (configurable)
- **Total: 61-230ms typically**

### Bandwidth

Bitrate = sample_rate * (sample_size / 8) * channels * 8

Examples:
- 44.1kHz, 16-bit, stereo: 1,411 kbit/s
- 48kHz, 16-bit, stereo: 1,536 kbit/s
- 96kHz, 24-bit, stereo: 4,608 kbit/s
- 48kHz, 24-bit, 5.1: 6,912 kbit/s

Add ~10% overhead for UDP/IP headers and packet framing.

### CPU Usage

- Minimal CPU usage (<1% on modern systems)
- Real-time priority not required
- Most work done in kernel (UDP stack)

## Error Handling

### Network Errors

- Failed sends are logged but don't block processing
- Prevents buffer buildup on network congestion
- No retransmission (UDP best-effort delivery)

### PipeWire Errors

- State changes monitored and logged
- Stream errors trigger cleanup
- Module can be reloaded without restart

### Buffer Management

- Uses PipeWire's buffer pool
- No additional buffering (low latency)
- Automatic flow control via PipeWire

## Supported Audio Formats

| Format | Sample Size | Endianness |
|--------|-------------|------------|
| S16LE  | 16-bit      | Little     |
| S24LE  | 24-bit      | Little     |
| S32LE  | 32-bit      | Little     |

Sample rates: 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 Hz

Channels: 1-255 (typically 1, 2, 6, or 8)

## Module Parameters

All parameters are optional with sensible defaults:

```c
struct module_args {
    char *sink_name;          // "Scream"
    char *sink_description;   // "Scream Network Sink"
    char *ip;                 // "239.255.77.77"
    int port;                 // 4010
    char *interface;          // NULL (auto)
    int rate;                 // 48000
    int channels;             // 2
    char *format;             // "S16LE"
};
```

## Debugging

### Enable Debug Logging

```bash
# Set PipeWire log level
PIPEWIRE_DEBUG=3 pipewire &

# Or in config
context.properties = {
    log.level = 3
}
```

### Capture Network Traffic

```bash
# Capture Scream packets
sudo tcpdump -i any -w scream.pcap udp port 4010

# Analyze with Wireshark
wireshark scream.pcap
```

### Test Audio Path

```bash
# Generate test tone
pw-cat -p --channels=2 --rate=48000 --target=Scream < /dev/zero

# Or use speaker-test
speaker-test -D pipewire:Scream -c 2
```

## Comparison with Alternatives

| Feature | Scream | PulseAudio RTP | Snapcast |
|---------|--------|----------------|----------|
| Latency | Low (60-100ms) | Medium (100-200ms) | Medium (100-300ms) |
| Setup | Simple | Medium | Complex |
| Multi-receiver | Yes | Yes | Yes |
| Compression | No | Optional | Yes |
| Sync | No | No | Yes |
| Windows sender | Yes | No | Yes |

## Future Enhancements

Potential improvements:

1. **Silence Suppression**: Don't send packets during silence
2. **Adaptive Bitrate**: Adjust quality based on network conditions
3. **Compression**: Optional Opus/AAC encoding
4. **Encryption**: TLS/DTLS for secure transmission
5. **Discovery**: mDNS/Zeroconf for automatic receiver detection
6. **Statistics**: Network metrics and quality monitoring

## References

- [PipeWire Documentation](https://docs.pipewire.org/)
- [Scream Protocol](https://github.com/duncanthrax/scream)
- [WAVEFORMATEXTENSIBLE](https://docs.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible)
- [RFC 1112 - IGMP](https://tools.ietf.org/html/rfc1112)
