// license:GPLv3+

#pragma once

#include <filesystem>

#include <string>
using namespace std::string_literals;
using std::string;

#include <vector>
using std::vector;

// Shared logging
#include "plugins/LoggingPlugin.h"
#define LOGD LPI_LOGD
#define LOGI LPI_LOGI
#define LOGW LPI_LOGW
#define LOGE LPI_LOGE

namespace ScoreTracker
{

LPI_USE();

std::filesystem::path GetPluginPath();

}
