# Linux User Guide

[中文](README.md) | [English](README.en.md)

Send your phone's Bluetooth audio through AirPlayQt to an AirPlay receiver or a AirPlay stereo pair.

## 1. Install dependencies

These commands target an up-to-date Arch Linux x86_64 installation. Use PipeWire 1.4+ and WirePlumber 0.5+. The GUI requires an active desktop user session; see section 7 for the headless CLI. Connect the phone to the computer over Bluetooth, and place the computer and AirPlay receivers on a local network that allows them to communicate.

```sh
sudo pacman -S --needed qt6-base openssl libplist pipewire pipewire-audio wireplumber bluez bluez-utils avahi libcap
sudo systemctl enable --now bluetooth.service avahi-daemon.service
systemctl --user start pipewire.service wireplumber.service
```

Check service status. Receiving AAC also requires PipeWire's AAC plugin; packaging may differ on other distributions.

```sh
systemctl status bluetooth.service avahi-daemon.service
systemctl --user status pipewire.service wireplumber.service
pacman -Ql pipewire-audio | grep libspa-codec-bluez5-aac.so
```

## 2. Set application permissions

These steps assume AirPlayQt is installed at `/usr/local/bin/AirPlayQt`. If it is installed elsewhere, substitute its actual path in the commands.

```sh
sudo setcap cap_net_bind_service=ep /usr/local/bin/AirPlayQt
getcap /usr/local/bin/AirPlayQt
```

The last command should show `cap_net_bind_service=ep`, which allows binding UDP ports 319/320 for AirPlay playback. Reapply `setcap` whenever you reinstall or replace the executable.

## 3. Pair your phone

Enable Bluetooth and discoverability in KDE's Bluetooth settings. Select the computer in the phone's Bluetooth settings, confirm the matching codes, and mark the phone as trusted on the computer.

Alternatively, run `bluetoothctl` in a terminal and enter these commands one at a time. Keep the terminal open until pairing finishes:

```text
power on
agent KeyboardDisplay
default-agent
pairable on
discoverable on
```

If an agent is already registered, continue with `default-agent`. When the phone initiates pairing, answer `yes` to `Confirm passkey` after checking that the codes match. If prompted with `Enter passkey`, enter the six digits shown on the phone.

After pairing, enter the following in the same terminal. Replace `PHONE_MAC` with the phone's Bluetooth address, such as `AA:BB:CC:DD:EE:FF`; use `devices` to list addresses.

```text
trust PHONE_MAC
connect PHONE_MAC
info PHONE_MAC
quit
```

Confirm that the device reports `Paired: yes`, `Trusted: yes`, and `Connected: yes`. Use AirPlayQt's input list to select the source; manage pairing and connection through the system's Bluetooth settings.

## 4. Make Bluetooth audio available for capture

Skip this section if your system already configures all phones as capture inputs, such as the customized archiso image.

Copy the installed configuration example and edit the copied file. If the destination already exists, edit it directly and preserve its existing rules.

```sh
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cp -n /usr/local/share/doc/AirPlayQt/51-airplayqt-bluetooth.conf.example ~/.config/wireplumber/wireplumber.conf.d/51-airplayqt-bluetooth.conf
nano ~/.config/wireplumber/wireplumber.conf.d/51-airplayqt-bluetooth.conf
```

Choose one matching pattern and keep `bluez5.media-source-role = "input"` in the file:

|Scope| `node.name` |
| --- | --- |
|Only the selected phone; replace address colons with underscores| `"~bluez_input.AA_BB_CC_DD_EE_FF.*"` |
|All phones| `"~bluez_input.*"` |

Stop audio playback, apply the configuration, and reconnect the phone. This briefly interrupts desktop audio.

```sh
systemctl --user restart wireplumber
```

This makes phone audio available to capture applications without also playing it through the computer's speakers. Use `wpctl status` to list inputs; some devices appear only after phone playback begins.

## 5. Play through AirPlay

Launch the installed application as your normal desktop user, without `sudo`:

```sh
/usr/local/bin/AirPlayQt
```

1. Play music on the phone and select the computer, such as `archlive`, as its audio output.
2. Select the phone in AirPlayQt's input list and choose FL/FR for the left and right input channels.
3. Select the network interface connected to the AirPlay receivers' LAN, scan, and select the receivers. For stereo playback, first create the pair using an application supported by the receivers, then select both members in AirPlayQt and check their left/right assignments.
4. Click Start, adjust the volume, and use Mute as needed.
5. Click Stop before changing input devices. To end playback, click Stop or close the application.

You can start sending before the phone begins playback. Pausing or disconnecting the phone keeps the session sending silence; audio resumes when the same phone returns. Leave the prebuffer at its default of 40 ms initially.

### Phone volume keys

While streaming, Bluetooth volume requests from the selected phone control AirPlay volume, including both members of a stereo pair. Absolute volume `0` mutes (`-144 dB`); values `1–127` map to `-30 + 30 × value / 127 dB`. If only up/down keys are available, each press or repeat changes volume by `1 dB`; release does not add a step. Relative keys wait 100 ms so absolute volume takes priority in the same window. The application does not add PCM gain or send AirPlay volume changes back to the phone.

Initial connection and reconnection establish a baseline without overriding AirPlay volume. Stopping, changing inputs, or disconnecting discards pending keys. The phone must expose absolute volume or AVRCP keys through BlueZ; changes made only to the phone's audio samples cannot be recovered as key requests.

Absolute volume does not require input-device access. Up/down keys require read access to the corresponding AVRCP input node. If the log reports `Cannot read AVRCP volume keys`, this optional rule grants the active local desktop user access to Bluetooth AVRCP nodes:

```sh
sudo install -m 0644 /usr/local/share/doc/AirPlayQt/70-airplayqt-avrcp.rules.example /etc/udev/rules.d/70-airplayqt-avrcp.rules
sudo udevadm control --reload-rules
```

Disconnect and reconnect the phone afterward. The application does not change system permissions itself and needs neither root nor membership in the `input` group that exposes all keyboards. Unreadable or ambiguous AVRCP devices are logged without stopping audio. Devices with identical names on the same adapter may be indistinguishable; ordinary computer keyboards are excluded. Keys are not grabbed exclusively, so the desktop may also respond to the same media keys.

## 6. Troubleshooting

### iPhone cannot find the computer

Enable discoverability on the computer and keep the iPhone on Settings → Bluetooth. Run `discoverable on` again if the discovery window expires.

If it remains invisible, temporarily set the adapter class to loudspeaker. Check the adapter index with `sudo btmgmt info`; `0` below corresponds to `hci0`. Then toggle discoverability off and on and search again.

```sh
sudo btmgmt --index 0 class 4 20
```

This is a temporary setting and may reset after reboot. Skip it on the archiso image that already persists the loudspeaker class.

### Phone disconnects immediately

Check pairing and trust status, then reconnect. If the journal shows `a2dp.c:auth_cb() Access denied`, also check KDE for a pending authorization prompt.

```sh
bluetoothctl info PHONE_MAC
bluetoothctl trust PHONE_MAC
bluetoothctl connect PHONE_MAC
journalctl -b -u bluetooth.service --no-pager -n 80
```

### Connected but silent

- Switch the phone's output back to itself, then select the computer again and resume playback.
- Check inputs with `wpctl status`; verify the rule in section 4 and select the phone in AirPlayQt rather than a headset microphone.
- Check AirPlayQt's log: `codec=aac` indicates AAC; SBC is also supported. The obtained client rate should be 44100 Hz. The source rate may differ, and `unknown` means that metadata is unavailable.
- For unsupported rate control or a client-format mismatch, check that PipeWire is 1.4+ and review custom format/resampling settings.

### AirPlay receivers missing or playback cannot start

Check that Avahi is running, the correct interface is selected, and the LAN permits multicast traffic. Check for guest-network or client-isolation settings. Avahi is also required when entering receiver addresses manually.

```sh
systemctl status avahi-daemon.service
getcap /usr/local/bin/AirPlayQt
sudo ss -lunp 'sport = :319 or sport = :320'
```

If binding UDP 319/320 fails, check the executable's capability and whether another process occupies the ports. After switching interfaces or changing network addresses, reselect the interface and start sending again.

### Settings location

AirPlayQt saves settings to `$XDG_CONFIG_HOME/AirPlayQt.json`, or `~/.config/AirPlayQt.json` when that variable is unset. Close the application before editing the file manually.

## 7. Standalone CLI (foreground operation)

`AirPlayQtCli` does not read or write GUI settings and requires an explicit input device ID and receiver IPv4 addresses.

### Build and install separately

Dependencies include Qt 6.5+, OpenSSL, libplist 2.7+ and the Linux audio components in section 1.

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf qt6-base openssl libplist pipewire pipewire-audio wireplumber bluez bluez-utils avahi libcap
cmake -S . -B build/linux-cli -G Ninja -DCMAKE_BUILD_TYPE=Release -DAIRPLAY_BUILD_STANDALONE=OFF -DAIRPLAY_BUILD_CLI=ON -DAIRPLAY_BUILD_VST3=OFF -DBUILD_TESTING=OFF
cmake --build build/linux-cli --target AirPlayQtCli
sudo cmake --install build/linux-cli --prefix /usr/local
sudo setcap cap_net_bind_service=ep /usr/local/bin/AirPlayQtCli
getcap /usr/local/bin/AirPlayQtCli
```

Reapply the capability whenever the executable is replaced. Manual receiver addresses still require Avahi to publish the session control service and permission to bind UDP 319/320.

### Headless audio environment

Follow sections 1, 3 and 4 to enable system Bluetooth and Avahi, pair and trust the phone, and configure `bluez5.media-source-role = "input"`.

```sh
systemctl --user start pipewire.service wireplumber.service
```

For an SSH session without an active local seat, the user can add the following to `~/.config/wireplumber/wireplumber.conf.d/52-airplayqt-headless.conf`. Merge it with any existing file instead of overwriting it. Use this for a dedicated audio user and avoid competing Bluetooth audio sessions under other users.

```text
wireplumber.profiles = {
  main = {
    monitor.bluez.seat-monitoring = disabled
  }
}
```

This is WirePlumber's documented [headless Bluetooth configuration](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/bluetooth.html#logind-integration). Stop playback, run `systemctl --user restart wireplumber`, then reconnect the phone. The application does not create system configuration, start services, pair devices or install an autostart service.

The optional AVRCP rule in section 5 uses `uaccess` for active local users; it does not guarantee access for an SSH user. If relative volume keys are needed without a desktop, an administrator must grant the runtime user read access specifically to the matching Bluetooth AVRCP input node, not to all keyboard devices. Absolute volume continues through BlueZ D-Bus. Unreadable keys produce a log message without interrupting audio.

### Query and send

```sh
/usr/local/bin/AirPlayQtCli --help
/usr/local/bin/AirPlayQtCli --version
/usr/local/bin/AirPlayQtCli --list-devices
/usr/local/bin/AirPlayQtCli --list-interfaces
```

`--list-devices` prints IDs, names and connection states. Startup and device listing inspect the initial system query only. A missing selected device, failed query or query taking more than 5 seconds causes an exit; the CLI does not wait for devices to appear later. If device changes invalidate the initial BlueZ snapshot, it also exits; run the command again. A paired but disconnected phone that remains in the catalogue can be selected, preserving the GUI's silence-while-waiting behavior.

Replace the sample device ID and IP addresses with actual values. Bluetooth IDs contain both adapter and phone addresses; copy the complete ID from the list. One receiver:

```sh
/usr/local/bin/AirPlayQtCli --device 'bluez:00:11:22:33:44:55/AA:BB:CC:DD:EE:FF' --receiver 192.168.8.9
```

A stereo pair already configured through the receivers' own system:

```sh
/usr/local/bin/AirPlayQtCli --device 'bluez:00:11:22:33:44:55/AA:BB:CC:DD:EE:FF' --receiver 192.168.8.9:7000 --receiver 192.168.8.10:7000
```

Optional interface binding and reversed input channels:

```sh
/usr/local/bin/AirPlayQtCli --device 'bluez:00:11:22:33:44:55/AA:BB:CC:DD:EE:FF' --receiver 192.168.8.9 --interface eth0 --local-ip 192.168.8.20 --left 2 --right 1
```

| Option | Behavior |
| --- | --- |
| `--device ID` | Required for sending; exact stable ID match, with no automatic device substitution |
| `--receiver IPv4[:port]` | Required for sending; one or two distinct endpoints, default port 7000; no receiver scanning |
| `--interface NAME --local-ip IPv4` | Supply together; omitted means system routing; a failed explicit binding never switches to another interface |
| `--left N --right N` | Default 1/2 (FL/FR); only 1/2 or 2/1 accepted |
| `--list-devices`, `--list-interfaces` | Use each alone; print results and exit |
| `--help`, `--version` | Use each alone; no audio initialization |

Only `--receiver` may be repeated. Unknown options, positional arguments and combining queries with sending options are rejected. Timing uses existing defaults, without loading advanced GUI settings. Lists and help go to stdout; English status, error and retry messages with UTC timestamps go to stderr. Audio statistics are not continuously printed.

The first device check, capture initialization or AirPlay startup failure exits immediately. After the first streaming state, including silent streaming, a session failure, disconnection or takeover by another sender causes cleanup followed by a 2-second delay and unlimited retries of the entire receiver group using the original options. Phone pauses, disconnections and temporarily unavailable audio nodes keep the GUI's silence and capture recovery behavior; they do not count as AirPlay session failures. Recreated sessions also retain the existing session volume initialization behavior.

Ctrl+C or SIGTERM cancels retries, stops capture and cleans up the network session before exiting. Exit codes: successful query `0`, initialization/runtime failure `1`, invalid arguments `2`, SIGINT `130`, SIGTERM `143`.
