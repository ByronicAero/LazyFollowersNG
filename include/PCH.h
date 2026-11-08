#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/basic_file_sink.h>

// Must come after CommonLib: it uses RE::InputEvent and pulls in <windows.h>.
#include "SKSEMenuFramework.h"

namespace logger = SKSE::log;
using namespace std::literals;
