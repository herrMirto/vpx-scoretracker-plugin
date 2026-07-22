// Local and standalone builds are unsigned by default. Release CMake builds
// override this header from the GitHub Actions secret.
#pragma once

#define SCORETRACKER_SIGNING_ENABLED 0
#define SCORETRACKER_SIGNING_SEED_HEX ""
