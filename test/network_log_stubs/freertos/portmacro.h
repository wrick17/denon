#pragma once
#include <mutex>
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
extern thread_local unsigned testCriticalDepth;
#define portENTER_CRITICAL_SAFE(mux) do { (mux)->lock(); ++testCriticalDepth; } while (0)
#define portEXIT_CRITICAL_SAFE(mux) do { --testCriticalDepth; (mux)->unlock(); } while (0)
