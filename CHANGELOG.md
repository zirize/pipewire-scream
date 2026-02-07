# Changelog

All notable changes to the PipeWire Scream Sender module will be documented in this file.

## [1.0.1] - 2026-02-07

### Fixed
- Memory management improvements in module_destroy (proper NULL checks)
- Integer overflow protection in audio frame size calculations
- Enhanced UDP transmission error handling with failure tracking
- Format validation now reports specific errors for invalid values
- Standardized parameter parsing with proper error messages

### Added
- Configurable multicast TTL parameter (`multicast.ttl`)
- Maximum consecutive send failure tracking (prevents log spam)
- Input validation constants (MIN/MAX ranges for all parameters)
- Helper function for consistent parameter parsing

### Improved
- Better thread safety in buffer processing
- More robust error handling throughout the codebase
- Clearer error messages for configuration issues
- Code quality: eliminated magic numbers, reduced duplication

## [1.0.0] - 2026-02-06

### Added
- Initial working implementation of PipeWire Scream sender module
- Virtual audio sink creation with PipeWire integration
- UDP multicast and unicast audio transmission
- Scream protocol implementation (compatible with existing receivers)
- Configurable sample rates, bit depths, and channel configurations
- CMake build system
- Comprehensive README with usage examples
- Module configuration via PipeWire config files

### Features
- Support for PipeWire 1.0.5+ 
- Low-latency audio transmission over UDP
- Automatic network packet framing (1152 bytes payload)
- Virtual and network node flags for proper PipeWire integration
- Configurable destination IP and port
- Default multicast to 239.255.77.77:4010
- Tested with long-duration playback (stable)

### Technical Details
- Uses `pw_stream` API with proper virtual/network flags
- Implements PipeWire stream events (state_changed, param_changed, process)
- Scream protocol header encoding (sample rate, bit depth, channels, channel mask)
- Network socket initialization with multicast support
- Compatible with existing Unix Scream receivers

### Tested Configurations
- PipeWire: 1.0.5
- Sample rates: 44.1kHz, 48kHz, 96kHz
- Formats: S16LE (16-bit)
- Channels: Stereo (2ch)
- Network: Unicast UDP transmission
- Duration: Long-form music playback (tested stable)

### Known Issues
- None critical
- Short WAV files may exhibit slight noise (source file dependent)

### Credits
- Based on PipeWire module-roc-sink architecture
- Scream protocol by Tom Paton and contributors
