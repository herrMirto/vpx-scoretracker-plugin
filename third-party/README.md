# Plugin SDK

`platforms/stage-dependencies.sh` stages public VPX plugin headers and nlohmann JSON
into ignored `include/`. Pass a local VPX checkout to `build-standalone.sh` to use
that installation's SDK. `include/revisions.txt` records the actual VPX commit.
No standalone PinMAME headers, libraries or cryptographic sources are required.
