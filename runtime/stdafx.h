// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <string>
#include <vector>

#include <sys/mman.h>
#ifdef __APPLE__
#include <TargetConditionals.h>  // TARGET_OS_IOS: __APPLE__ alone means macOS or iOS
#endif

#include <ppc_config.h>
#include <ppc_context.h>
#include <xbox.h>
#include <cpu/ppc_context.h>
#include <kernel/memory.h>
#include <kernel/status.h>
#include <mutex.h>
#include <kernel/heap.h>
#include <xxHashMap.h>

// Logging used by code ported from Unleashed Recompiled.
#define LOG_UTILITY(msg) fprintf(stderr, "[kernel] %s: %s\n", __func__, msg)
#define LOGF_UTILITY(fmt, ...) fprintf(stderr, "[kernel] %s: %s\n", __func__, std::format(fmt, __VA_ARGS__).c_str())
