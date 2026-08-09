# Batocera

The Batocera package installs the existing VPX ScoreTracker plugin and provides two read-only
viewers: the web dashboard for another device on the local network, and a controller-friendly
Pygame viewer launched from EmulationStation's Ports collection.

The package targets Batocera 43.1 x86-64 with a current, plugin-enabled VPX build. Batocera
43.1's original VPX package predates the plugin API, so it is not compatible. The installer verifies
that the replacement VPX installation exposes `/usr/bin/vpinball/plugins` before making changes.
It uses Batocera's persistent user-service mechanism and stores its files under
`/userdata/system/scoretracker`.

## Install

Copy `scoretracker-batocera-VERSION-x86_64.tar.gz` to `/userdata/system/` on the Batocera machine,
then connect over SSH:

```sh
cd /userdata/system
tar -xzf scoretracker-batocera-*-x86_64.tar.gz
cd /userdata/system/scoretracker-install
./install-batocera.sh
```

This single installer installs the VPX plugin and maps, the web dashboard service, and the Ports
viewer. No separate Pybrowser package or installation step is required.

The dashboard listens on port `8080` by default. To select another port during installation, use:

```sh
./install-batocera.sh --listen-port 8090
```

The installer refuses a port that is already in use. The selection is stored in
`/userdata/system/scoretracker/scoretracker.conf` and is preserved by future updates.

The installer prints an address such as `http://192.168.1.50:8080`. Open it from a phone, tablet, or
computer. The dashboard is deliberately read-only and should only be exposed to a trusted local
network.

After installation, refresh the EmulationStation game list and open **Ports > VPX ScoreTracker** to use
the native viewer. Use the directional pad to select a table, A to open its history, B to go back or
exit, and the left shoulder button to refresh. Keyboard equivalents are the arrow keys, Enter,
Escape, and R. The installer adds the VPX ScoreTracker icon and 2026 release year to the existing
Ports gamelist while preserving play count and last-played metadata. Restart EmulationStation if
the new metadata is not visible immediately.

The native viewer reuses local Batocera wheel, marquee, or logo artwork when available. It resolves
media from the VPX `gamelist.xml` and standard Batocera media folders without downloading artwork.
Tables without matching local media retain the text-only layout.

## Score-saved notification

The plugin shows a brief “Score saved” pop-up over the playfield by default. To hide the pop-up
without disabling score tracking, edit
`/userdata/system/configs/vpinball/VPinballX.ini` and set:

```ini
[Plugin.ScoreTracker]
Enable = 1
Notifications = 0
```

Restart VPX after changing the setting.

## Updates

The Ports viewer checks for a newer compatible Batocera package when it opens. If one is available,
use the update control shown in its footer and confirm the installation. The updater downloads the
versioned package from this repository, verifies GitHub's SHA-256 digest, and runs the unified
installer. It preserves the configured dashboard port.

The same updater can be run over SSH:

```sh
/userdata/system/scoretracker/update-scoretracker.sh --check
/userdata/system/scoretracker/update-scoretracker.sh --install
```

Updater output is appended to `/userdata/system/scoretracker/scoretracker-update.log`.

## Service management

```sh
batocera-services stop ScoreTracker
batocera-services start ScoreTracker
tail -f /userdata/system/scoretracker/scoretracker-server.log
```

The VPX executable and bundled plugins are installed in Batocera's non-persistent system image. The
ScoreTracker service therefore copies the persistent plugin payload into
`/usr/bin/vpinball/plugins/scoretracker` each time the service starts. It does not use
`batocera-save-overlay`.
