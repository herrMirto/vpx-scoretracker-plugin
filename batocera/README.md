# Batocera preview

The Batocera package installs the existing VPX ScoreTracker plugin and runs a read-only web
dashboard. Batocera itself does not open a window. Use a browser on another device connected to the
same network and open the address printed by the installer.

The first preview targets Batocera 43.1 x86-64 with a current, plugin-enabled VPX build. Batocera
43.1's original VPX package predates the plugin API, so it is not compatible. The installer verifies
that the replacement VPX installation exposes `/usr/bin/vpinball/plugins` before making changes.
It uses Batocera's persistent user-service mechanism and stores its files under
`/userdata/system/scoretracker`.

## Install

Copy `scoretracker-batocera-x86_64.tar.gz` to `/userdata/system/` on the Batocera machine, then
connect over SSH:

```sh
cd /userdata/system
tar -xzf scoretracker-batocera-x86_64.tar.gz
cd /userdata/system/scoretracker-install
./install-batocera.sh
```

The installer prints an address such as `http://192.168.1.50:8080`. Open it from a phone, tablet, or
computer. The dashboard is deliberately read-only and should only be exposed to a trusted local
network.

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
