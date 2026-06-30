Files in this directory are named for the **exact** output of `awk -F= '/^ID=/ {print $2}' /etc/os-release` for their respective distribution.

The Linux CI dependency-install action sources the matching distribution file before configuring the project with CMake presets.
