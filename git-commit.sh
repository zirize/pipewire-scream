#!/bin/bash
# Git commit script for PipeWire Scream Sender

set -e

cd "$(dirname "$0")/../.."

echo "=== PipeWire Scream Sender - Git Commit Script ==="
echo ""

# Check git status
echo "Current git status:"
git status --short

echo ""
echo "Files to be added:"
echo "  - Senders/pipewire/"
echo ""

read -p "Continue with commit? (y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]
then
    echo "Aborted."
    exit 1
fi

# Add new files
git add Senders/pipewire/

# Show what will be committed
echo ""
echo "=== Files staged for commit ==="
git status

echo ""
read -p "Proceed with commit? (y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]
then
    echo "Aborted."
    exit 1
fi

# Commit
git commit -m "Add PipeWire sender module for Linux

Implements a native PipeWire module that creates a virtual audio sink
and transmits audio over UDP using the Scream protocol.

Features:
- Virtual audio sink in PipeWire
- Multicast and unicast support
- Compatible with existing Scream receivers
- Tested stable on PipeWire 1.0.5
- Low-latency audio transmission

Includes:
- Complete module source code (module-scream-sender.c)
- CMake build system
- Comprehensive documentation (README.md)
- Configuration examples
- Changelog (CHANGELOG.md)

Tested on:
- PipeWire 1.0.5
- Long-duration music playback
- Unicast UDP transmission
- 48kHz 16-bit stereo audio

This provides Linux users with equivalent functionality to the Windows
Scream driver without requiring external tools or PulseAudio."

echo ""
echo "=== Commit successful ==="
echo ""
echo "Next steps:"
echo "1. Review commit: git show HEAD"
echo "2. Push to fork: git push origin <branch-name>"
echo "3. Or create PR on GitHub"
echo ""
echo "See RELEASE_STRATEGY.md for detailed public release options."
