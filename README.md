<p align="center">
  <h1 align="center">⏱️ ishap</h1>
  <p align="center"><b>Tiny Reusable Fixed-Timestep Runner</b></p>

  <p align="center">
    <a href="https://github.com/mickryley/ishap"><img alt="Version" src="https://img.shields.io/badge/version-v0.5.0-lightblue.svg"></a>
    <a href="https://github.com/mickryley/ishap/commits/main"><img alt="Last Commit" src="https://img.shields.io/github/last-commit/mickryley/ishap.svg"></a>
    <a href="https://github.com/mickryley/ishap/actions/workflows/testing"><img alt="Build Status" src="https://github.com/mickryley/ishap/actions/workflows/testing.yml/badge.svg"></a>
    <a href="https://en.cppreference.com/w/cpp/compiler_support"><img alt="C++20+" src="https://img.shields.io/badge/C%2B%2B-20%2B-orange.svg"></a>
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
> **New in v0.5.0:** Diagnostic `Stats` counters (dropped steps, clamped deltas), `BasicFixedTimestepRunner<StepFn, Clock>` template for zero-overhead step functions and injectable clocks, `can_step()` query, `NaN`/`Inf`-safe `set_time_scale`, and a robust first-tick sentinel.

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
| 📊 **Diagnostics** | Cumulative `Stats` counters for dropped steps and clamped deltas |

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

### 🎨 Render Interpolation (alpha)

`tick()` and `push_time()` both return an **alpha** value in `[0, 1)` — the fraction of the next fixed step that has elapsed. Use it to interpolate between the previous and current simulation state so rendering is smooth regardless of frame rate:

```cpp
// Keep a copy of state before each fixed step
PhysicsState prev = current;

const double alpha = runner.tick();
// runner called your fixed-update function N times, updating `current`

render_position = lerp(prev.position, current.position, alpha);
```

---

### 🔁 Deterministic / Replay Mode

`push_time()` accepts an explicit delta with **no wall-clock dependency**. Use it whenever reproducibility matters — tests, replays, network lock-step, or CI:

```cpp
// Simulation is bit-exact across machines
for (auto& frame : recorded_frames) {
    const double alpha = runner.push_time(frame.elapsed);
    snapshot(alpha);
}
```

---

### 📊 Diagnostic Stats

`stats()` returns a snapshot of cumulative counters that survive `reset()`:

```cpp
auto s = runner.stats();
std::cout << "Ticks: "          << s.total_ticks          << "\n"
          << "Steps: "          << s.total_steps          << "\n"
          << "Dropped: "        << s.dropped_step_events  << "\n"
          << "Delta clamped: "  << s.clamped_delta_events << "\n";

runner.reset_stats(); // zero all counters independently of runner state
```

A non-zero `dropped_step_events` means your simulation is falling behind — consider raising `safety_max_substeps` or reducing fixed-update work.

---

### ⚡ Advanced: `BasicFixedTimestepRunner<StepFn, Clock>`

The underlying class exposes two template parameters:

```cpp
template<
    typename StepFn = std::function<void(std::chrono::nanoseconds)>,
    typename Clock  = std::chrono::steady_clock>
class BasicFixedTimestepRunner;

using FixedTimestepRunner = BasicFixedTimestepRunner<>; // both defaults
```

**Concrete `StepFn`** — for very high update rates (8 kHz+) where `std::function` dispatch is measurable, pass a concrete callable so the compiler can inline it entirely:

```cpp
auto my_update = [&](std::chrono::nanoseconds dt) { /* ... */ };

// Deduced as BasicFixedTimestepRunner<decltype(my_update)> — zero overhead
ishap::timestep::BasicFixedTimestepRunner runner{my_update, config};
```

**Custom `Clock`** — inject a fake clock for deterministic `tick()`-based tests without switching to `push_time()`:

```cpp
struct FakeClock {
    using duration   = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<FakeClock>;
    static time_point now() { return current_fake_time; }
    static inline time_point current_fake_time{};
};

ishap::timestep::BasicFixedTimestepRunner<std::function<void(std::chrono::nanoseconds)>, FakeClock> runner{fn, config};
FakeClock::current_fake_time += 16ms;
runner.tick(); // uses FakeClock::now()
```

The default `FixedTimestepRunner` alias is fine for all other cases.

---

### 🔍 `can_step()`

Returns `true` when enough time has accumulated to fire at least one fixed step. Useful for polling-style loops or to check whether the next `push_time()` call will trigger any simulation work:

```cpp
// Peek without running — useful for conditional logic or profiling
if (runner.can_step()) {
    prepare_simulation_state();
}
const double alpha = runner.tick();
```

Note: because `tick()` and `push_time()` drain the accumulator as they run, `can_step()` will typically return `false` immediately after a normal call. It returns `true` in post-call state only when the substep cap was hit and time is still pending.

---

### 🔒 Thread Safety

`FixedTimestepRunner` is **not thread-safe**. All calls to `tick()`, `push_time()`, `set_hz()`, and other mutating methods must occur on the same thread. If you need to drive updates from one thread and read state from another, add your own mutex or use a single-producer/single-consumer queue.

---

### 🧪 Running Tests (Windows)

A batch script is included that finds Visual Studio automatically, configures a Ninja build, and runs CTest:

```bat
run_tests.bat            # Debug build, stats enabled
run_tests.bat --release  # Release build
run_tests.bat --no-stats # Build with ISHAP_DISABLE_STATS=ON
run_tests.bat --clean    # Wipe build dir first, then build and test
```

Requires Visual Studio with the **C++ Desktop Development** workload installed.

---

### 🧱 CMake Integration

#### Option 1 — FetchContent
```cmake
include(FetchContent)
FetchContent_Declare(
  ishap
  GIT_REPOSITORY https://github.com/mickryley/ishap.git
  GIT_TAG        v0.5.0
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
- C++20 or later (`cxx_std_20` minimum)
- Header-only — no compiled sources required

---

### 📦 Directory Layout
```
ishap/
├─ include/
│  └─ ishap/
│     └─ ishap.hpp
├─ tests/
│  ├─ CMakeLists.txt
│  └─ test_ishap.cpp
├─ CMakeLists.txt
├─ run_tests.bat
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