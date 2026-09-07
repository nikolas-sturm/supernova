#!/bin/sh

# User Service
mkdir -p ~/.config/systemd/user
cp "/app/share/sol/systemd/user/app-dev.lizardbyte.app.Sol.service" "$HOME/.config/systemd/user/app-dev.lizardbyte.app.Sol.service"
echo "Sol User Service has been installed."
echo "Use [systemctl --user enable app-dev.lizardbyte.app.Sol] once to autostart Sol on login."

# Load uhid for descriptor-driven gamepad emulation
UHID=$(cat /app/share/sol/modules-load.d/60-sol.conf)
echo "Enabling gamepad emulation."
flatpak-spawn --host pkexec sh -c "echo '$UHID' > /etc/modules-load.d/60-sol.conf"
flatpak-spawn --host pkexec modprobe uhid

# Udev rule
UDEV=$(cat /app/share/sol/udev/rules.d/60-sol.rules)
echo "Configuring virtual input permissions."
flatpak-spawn --host pkexec sh -c "echo '$UDEV' > /etc/udev/rules.d/60-sol.rules"
flatpak-spawn --host pkexec udevadm control --reload-rules
flatpak-spawn --host pkexec udevadm trigger --property-match=DEVNAME=/dev/uinput
flatpak-spawn --host pkexec udevadm trigger --property-match=DEVNAME=/dev/uhid
flatpak-spawn --host pkexec udevadm trigger --subsystem-match=hidraw
flatpak-spawn --host pkexec udevadm trigger --subsystem-match=input
echo "Virtual input permissions have been updated."
