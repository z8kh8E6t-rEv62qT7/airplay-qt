# Linux User Guide

[中文](README.md) | [English](README.en.md)

Send your phone's Bluetooth audio through AirPlayQt to an AirPlay receiver or a HomePod stereo pair.

## 1. Install dependencies

These commands target an up-to-date Arch Linux x86_64 installation. Use PipeWire 1.4+, WirePlumber 0.5+, and an active desktop user session. Connect the phone to the computer over Bluetooth, and place the computer and HomePods on a local network that allows them to communicate.

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

The last command should show `cap_net_bind_service=ep`, which allows binding UDP ports 319/320 for HomePod playback. Reapply `setcap` whenever you reinstall or replace the executable.

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

After pairing, enter the following in the same terminal. Replace `PHONE_MAC` with the phone's Bluetooth address, such as `08:C7:B5:48:7D:0C`; use `devices` to list addresses.

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
|Only the selected phone; replace address colons with underscores| `"~bluez_input.08_C7_B5_48_7D_0C.*"` |
|All phones| `"~bluez_input.*"` |

Stop audio playback, apply the configuration, and reconnect the phone. This briefly interrupts desktop audio.

```sh
systemctl --user restart wireplumber
```

This makes phone audio available to capture applications without also playing it through the computer's speakers. Use `wpctl status` to list inputs; some devices appear only after phone playback begins.

## 5. Play through HomePod

Launch the installed application as your normal desktop user, without `sudo`:

```sh
/usr/local/bin/AirPlayQt
```

1. Play music on the phone and select the computer, such as `archlive`, as its audio output.
2. Select the phone in AirPlayQt's input list and choose FL/FR for the left and right input channels.
3. Select the network interface connected to the HomePods' LAN, scan, and select the receivers. For stereo playback, first create the pair in Apple's Home app, then select both members in AirPlayQt and check their left/right assignments.
4. Click Start, adjust the volume, and use Mute as needed.
5. Click Stop before changing input devices. To end playback, click Stop or close the application.

You can start sending before the phone begins playback. Pausing or disconnecting the phone keeps the session sending silence; audio resumes when the same phone returns. Leave the prebuffer at its default of 40 ms initially.

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

### HomePods missing or playback cannot start

Check that Avahi is running, the correct interface is selected, and the LAN permits multicast traffic. Check for guest-network or client-isolation settings. Avahi is also required when entering receiver addresses manually.

```sh
systemctl status avahi-daemon.service
getcap /usr/local/bin/AirPlayQt
sudo ss -lunp 'sport = :319 or sport = :320'
```

If binding UDP 319/320 fails, check the executable's capability and whether another process occupies the ports. After switching interfaces or changing network addresses, reselect the interface and start sending again.

### Settings location

AirPlayQt saves settings to `$XDG_CONFIG_HOME/AirPlayQt.json`, or `~/.config/AirPlayQt.json` when that variable is unset. Close the application before editing the file manually.
