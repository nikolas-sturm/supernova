#!/bin/sh

# User Service
systemctl --user stop app-dev.lizardbyte.app.Sol
rm "$HOME/.config/systemd/user/app-dev.lizardbyte.app.Sol.service"
systemctl --user daemon-reload
echo "Sol User Service has been removed."

# Remove rules
flatpak-spawn --host pkexec sh -c "rm /etc/modules-load.d/60-sol.conf"
flatpak-spawn --host pkexec sh -c "rm /etc/udev/rules.d/60-sol.rules"
flatpak-spawn --host pkexec udevadm control --reload-rules
echo "Input rules removed."
