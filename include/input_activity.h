#pragma once

#include <atomic>

// Timestamps (nowSeconds()) of the last physical input event. Remote-control
// injection uses these to avoid stealing keyboard/pointer focus while the
// local owner is actively using the machine.
namespace input_activity {
inline std::atomic<double> lastLocalKeyboard{ 0.0 };
inline std::atomic<double> lastLocalPointer{ 0.0 };
}
