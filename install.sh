#!/bin/bash
# Installation script for Scream PipeWire Sender Module

set -e

echo "========================================"
echo "Scream PipeWire Sender Module Installer"
echo "========================================"
echo

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "Error: This script must be run as root (use sudo)"
    exit 1
fi

# Check if PipeWire is installed
if ! pkg-config --exists libpipewire-0.3; then
    echo "Error: PipeWire development files not found"
    echo "Please install PipeWire first:"
    echo "  Ubuntu/Debian: sudo apt-get install libpipewire-0.3-dev pipewire"
    echo "  Fedora/RHEL:   sudo dnf install pipewire-devel"
    echo "  Arch Linux:    sudo pacman -S pipewire"
    exit 1
fi

# Create build directory
echo "Creating build directory..."
mkdir -p build
cd build

# Configure with CMake
echo "Configuring with CMake..."
cmake ..

# Build
echo "Building module..."
make

# Install
echo "Installing module..."
make install

# Get module directory
MODULE_DIR=$(pkg-config --variable=moduledir libpipewire-0.3)
if [ -z "$MODULE_DIR" ]; then
    MODULE_DIR="/usr/lib/pipewire-0.3"
fi

echo
echo "========================================"
echo "Installation complete!"
echo "========================================"
echo "Module installed to: $MODULE_DIR"
echo
echo "To use the module, configure automatic loading:"
echo
echo "1. Create config directory:"
echo "   mkdir -p ~/.config/pipewire/pipewire.conf.d/"
echo
echo "2. Copy example configuration:"
echo "   cp scream-sender.conf.example ~/.config/pipewire/pipewire.conf.d/scream-sender.conf"
echo
echo "3. Restart PipeWire to load the module:"
echo "   systemctl --user restart pipewire"
echo
echo "4. Verify the sink was created:"
echo "   pactl list sinks short | grep Scream"
echo
echo "For more information, see README.md"
echo
