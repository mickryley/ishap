<p align="center">
  <h1 align="center">⏱️ ishap</h1>
  <p align="center"><b>Tiny Reusable Fixed-Timestep Runner</b></p>

  <p align="center">
    <a href="https://github.com/mickryley/ishap"><img alt="Version" src="https://img.shields.io/badge/version-v0.3.0-lightblue.svg"></a>
    <a href="https://github.com/mickryley/ishap/commits/main"><img alt="Last Commit" src="https://img.shields.io/github/last-commit/mickryley/ishap.svg"></a>
    <a href="https://en.cppreference.com/w/cpp/compiler_support"><img alt="C++17+" src="https://img.shields.io/badge/C%2B%2B-17%2B-orange.svg"></a>
    <img alt="Header-only" src="https://img.shields.io/badge/Header--only-yes-success.svg">
    <a href="https://cmake.org"><img alt="CMake 3.15+" src="https://img.shields.io/badge/CMake-3.15%2B-informational.svg"></a>
    <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/License-MIT-blue.svg"></a>
    <img alt="Lines of Code" src="https://img.shields.io/badge/LOC-%3C500-lightgrey.svg">
  </p>
</p>

---

> **ishap** is a lightweight, header-only fixed timestep runner for deterministic update loops.  
> It offers a clean, unit-safe API built on `std::chrono`, featuring precise clamping, scaling, and substep management.  
> Designed as a dependable, zero-dependency utility for engines, simulations, and real-time systems.  
> ☕ Made in an afternoon to keep everything running like clockwork.
> **New in v0.3.0:** High-precision step sequencing to handle non-integer nanosecond frequencies (like 60Hz) without drift, now with **zero heap allocations** using `std::array`.

---

### ✨ Features

| Feature | Description |
|:--|:--|
| 🕒 **Fixed timestep stepping** | via `tick()` or `push_time()` |
| 🎯 **Deterministic updates** | when fed explicit `dt` |
| 💎 **Sub-ns Precision** | Automated step-sequencing to eliminate timing drift |
| ⚙️ **Unit-safe API** | built on `std::chrono` |
| 🧩 **Time scaling** | **Max delta clamp**, **substep cap**, and **accumulator management** |
| 🚫 **Exception-safe** | internal try/catch preserves `noexcept` |
| 💡 **Error hook support** | handle exceptions without breaking flow |
| 🧱 **Header-only** | zero dependencies, drop-in ready |
| 🧱 **Zero-Alloc** | No `std::vector` or heap usage beyond `std::function` |

---


### 🧭 Example Usage

```cpp
#include <ishap/ishap.hpp>
#include <iostream>
using namespace std::chrono_literals;

int main() {
    ishap::timestep::FixedTimestepRunner runner{
        [](std::chrono::nanoseconds dt) {
            // Fixed update logic
            std::cout << "Fixed step: " << dt.count() << "ns\n";
        },
        {.step = 16ms, .safety_max_substeps = 8}
    };

    // Game loop
    while (true) {
        const double alpha = runner.tick(); // or runner.push_time(frame_dt)
        // render(interpolate(alpha));
    }
}
```

---

### ⚙️ Configuration Overview

| Field | Type | Default | Description |
|:--|:--|:--|:--|
| `step` | `std::chrono::nanoseconds` | ~16.67 ms | Fixed update timestep |
| `target_hz` | `double` | `60.0` | Target frequency for serialization |
| `time_scale` | `double` | `1.0` | Speed multiplier (`0.0` = paused) |
| `safety_max_delta` | `std::chrono::nanoseconds` | 250 ms | Clamp for large frame gaps |
| `safety_max_substeps` | `size_t` | 8 | Maximum fixed steps per tick |
| `safety_max_accumulator_overflow` | `size_t` | 3 | Accumulator clamp multiplier |

---

### 🧱 CMake Integration

#### Option 1 — FetchContent
```cmake
include(FetchContent)
FetchContent_Declare(
  ishap
  GIT_REPOSITORY https://github.com/mickryley/ishap.git
  GIT_TAG        v0.3.0
)
FetchContent_MakeAvailable(ishap)

target_link_libraries(your_target PRIVATE ishap::ishap)
```

#### Option 2 — Installed Package
```cmake
find_package(ishap CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ishap::ishap)
```

---

🧩 The Drift Problem & Step Sequencing
Standard `std::chrono::nanoseconds` are integers. However, 1/60s is exactly 16,666,666.666... nanoseconds. Over time, simply using 16ms or 16,666,667ns causes simulation drift.

ishap solves this by generating a step_sequence. For 60Hz, it will automatically cycle through a sequence of steps (e.g., alternating between 16,666,667ns and 16,666,666ns) to ensure that over the long term, the average frequency is exactly 60.0 Hz.

---

### 🧰 Requirements
- CMake **3.15+**
- C++17 or later (`cxx_std_17` minimum; tested with C++20 / 23)
- Header-only — no compiled sources required

---

### 📦 Directory Layout
```
ishap/
├─ include/
│  └─ ishap/
│     └─ ishap.hpp
├─ CMakeLists.txt
├─ LICENSE
└─ README.md
```

---

### 🧩 Example Error Hook
```cpp
runner.set_error_function([] {
    std::cerr << "Step error caught — simulation continued safely.\n";
});
```

---

### 🧾 License
**MIT License** — see [`LICENSE`](LICENSE).  
SPDX Identifier: `MIT`

---

Focused on precision and reuse.  
A compact, self-contained timestep utility built for deterministic real-time systems.