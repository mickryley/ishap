// ishap.hpp — tiny, reusable fixed–timestep runner
// SPDX-License-Identifier: MIT
//
// Rationale
// - Clean, unit-safe API using std::chrono
// - Works with either a real clock (tick()) or external dt feed (push_time())
// - Deterministic stepping (when fed explicit dt)
// - Time scaling, max-delta clamp, substep cap, alpha for interpolation
// - Header-only; no exceptions; no allocations beyond std::function target
//
// Usage
//   ishap::timestep::FixedTimestepRunner runner{
//       [](std::chrono::nanoseconds dt){ /* fixed update */ },
//       ishap::timestep::standards::k_config_ntsc_double_5994
//   };
//   for (;;) {
//       // Game/Film/Robotics loop …
//       const double a = runner.tick(); // or runner.push_time(frame_dt);
//       // render(interpolate(a));
//   }
//
#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace ishap::timestep {
    namespace standards {
        inline constexpr double k_hz_cinema = 24.0;
        inline constexpr double k_hz_pal = 25.0;
        inline constexpr double k_hz_ntsc_film = 24.0 * 1000.0 / 1001.0;    // ~23.976
        inline constexpr double k_hz_ntsc_video = 30.0 * 1000.0 / 1001.0;   // ~29.97
        inline constexpr double k_hz_hfr_cinema = 48.0;
        inline constexpr double k_hz_pal_double = 50.0;
        inline constexpr double k_hz_ntsc_double = 60.0 * 1000.0 / 1001.0;  // ~59.94
        inline constexpr double k_hz_ntsc_120 = 120.0 * 1000.0 / 1001.0;    // ~119.88
        inline constexpr double k_hz_12 = 12.0;
        inline constexpr double k_hz_15 = 15.0;
        inline constexpr double k_hz_20 = 20.0;
        inline constexpr double k_hz_30 = 30.0;
        inline constexpr double k_hz_60 = 60.0;
        inline constexpr double k_hz_72 = 72.0;
        inline constexpr double k_hz_75 = 75.0;
        inline constexpr double k_hz_80 = 80.0;
        inline constexpr double k_hz_90 = 90.0;
        inline constexpr double k_hz_120 = 120.0;
        inline constexpr double k_hz_144 = 144.0;
        inline constexpr double k_hz_240 = 240.0;
        inline constexpr double k_hz_360 = 360.0;
        inline constexpr double k_hz_250 = 250.0;
        inline constexpr double k_hz_500 = 500.0;
        inline constexpr double k_hz_1000 = 1000.0;
        inline constexpr double k_hz_2000 = 2000.0;
        inline constexpr double k_hz_4000 = 4000.0;
        inline constexpr double k_hz_8000 = 8000.0;
    }

    inline constexpr std::chrono::nanoseconds   k_default_max_delta{ 250'000'000 }; // 250ms
    inline constexpr size_t                     k_default_max_substeps = 8;
    inline constexpr size_t                     k_default_max_accumulator_overflow = 3;
    inline constexpr double                     k_default_time_scale = 1.0;

    /// @brief Maximum length for step sequence array
    inline constexpr size_t k_max_step_sequence_length = 32;

    /// @brief Epsilon for comparing step sequence timing
    inline constexpr double k_step_sequence_epsilon = 1e-6;

#ifndef ISHAP_DISABLE_STATS
    /**
    * @brief Cumulative diagnostic counters.Access via stats(), reset via reset_stats().
    * Define ISHAP_DISABLE_STATS before including this header (or via CMake) to compile
    * out all tracking and remove the Stats member entirely.
    */
    struct Stats {
        size_t total_steps{0};          ///< Fixed steps executed across all ticks
        size_t total_ticks{0};          ///< Total tick() / push_time() calls
        size_t dropped_step_events{0};  ///< Times the substep cap was reached (steps were skipped)
        size_t clamped_delta_events{0}; ///< Times the max-delta clamp was applied
    };
#endif

	/// @brief Configuration settings for the timestep runner
    struct Config {
        /// @brief Target fixed update step duration (default: ~16.67ms for 60Hz)
        std::chrono::nanoseconds 	step        						= std::chrono::nanoseconds(16'666'666);
        /// @brief Used for serialization to preserve "60.0" instead of "60.0000024".
        double						target_hz							= standards::k_hz_60;
        /**
        * @brief Optional sequence of steps to cycle through for high-precision timing.
        * Used when 1.0/Hz does not divide cleanly into integers of nanoseconds (e.g., 1/60Hz = 16.666...ms).
        * If empty, 'step' is used for all updates.
        */
        std::array<std::chrono::nanoseconds, k_max_step_sequence_length> step_sequence = {};
        /// @brief Length of the step sequence in use (0 if not used)
        size_t                      step_sequence_length                = 0;
        /// @brief Time scale factor (default: 1.0 = normal time)
        double       				time_scale   						= k_default_time_scale;
        /// @brief Safety max delta to prevent spiral of death (default: 250ms)
        std::chrono::nanoseconds 	safety_max_delta   					= k_default_max_delta;
        /// @brief Safety max substeps to prevent spiral of death (default: 8)
        size_t          			safety_max_substeps 				= k_default_max_substeps;
        /// @brief Safety max accumulator overflow multiplier (default: 3)
        size_t 						safety_max_accumulator_overflow 	= k_default_max_accumulator_overflow;

        /// @brief Generates a Config with a pre-calculated step sequence for a given Hz
        [[nodiscard]] static constexpr Config from_hz(double hz) noexcept {
            Config config{};
            config.target_hz = hz;
            if (hz <= 0.0) return config;

            config.step = std::chrono::nanoseconds(static_cast<int64_t>(1'000'000'000.0 / hz));

            double period_ns = 1'000'000'000.0 / hz;
            double ideal_accum = 0.0;
            int64_t discrete_accum = 0;

            for (size_t i = 0; i < k_max_step_sequence_length; ++i) {
                ideal_accum += period_ns;

                int64_t target_discrete = static_cast<int64_t>(ideal_accum + 0.5);
                int64_t step_ns = target_discrete - discrete_accum;

                config.step_sequence[i] = std::chrono::nanoseconds(step_ns);
                discrete_accum += step_ns;
                config.step_sequence_length++;

                double diff = ideal_accum - static_cast<double>(discrete_accum);
                if (diff < 0.0) diff = -diff;

                if (diff < k_step_sequence_epsilon) {
                    if (config.step_sequence_length <= 1) config.step_sequence_length = 0;
                    break;
                }
            }
            return config;
        }
    };

    namespace standards {
        inline constexpr Config k_config_cinema_24         = Config::from_hz(k_hz_cinema);
        inline constexpr Config k_config_pal_25            = Config::from_hz(k_hz_pal);
        inline constexpr Config k_config_ntsc_film_23976   = Config::from_hz(k_hz_ntsc_film);
        inline constexpr Config k_config_ntsc_video_2997   = Config::from_hz(k_hz_ntsc_video);
        inline constexpr Config k_config_hfr_cinema_48     = Config::from_hz(k_hz_hfr_cinema);
        inline constexpr Config k_config_pal_double_50     = Config::from_hz(k_hz_pal_double);
        inline constexpr Config k_config_ntsc_double_5994  = Config::from_hz(k_hz_ntsc_double);
        inline constexpr Config k_config_ntsc_120_11988    = Config::from_hz(k_hz_ntsc_120);
        inline constexpr Config k_config_12                = Config::from_hz(k_hz_12);
        inline constexpr Config k_config_15                = Config::from_hz(k_hz_15);
        inline constexpr Config k_config_20                = Config::from_hz(k_hz_20);
        inline constexpr Config k_config_30                = Config::from_hz(k_hz_30);
        inline constexpr Config k_config_60                = Config::from_hz(k_hz_60);
        inline constexpr Config k_config_72                = Config::from_hz(k_hz_72);
        inline constexpr Config k_config_75                = Config::from_hz(k_hz_75);
        inline constexpr Config k_config_80                = Config::from_hz(k_hz_80);
        inline constexpr Config k_config_90                = Config::from_hz(k_hz_90);
        inline constexpr Config k_config_120               = Config::from_hz(k_hz_120);
        inline constexpr Config k_config_144               = Config::from_hz(k_hz_144);
        inline constexpr Config k_config_240               = Config::from_hz(k_hz_240);
        inline constexpr Config k_config_360               = Config::from_hz(k_hz_360);
        inline constexpr Config k_config_250               = Config::from_hz(k_hz_250);
        inline constexpr Config k_config_500               = Config::from_hz(k_hz_500);
        inline constexpr Config k_config_1000              = Config::from_hz(k_hz_1000);
        inline constexpr Config k_config_2000              = Config::from_hz(k_hz_2000);
        inline constexpr Config k_config_4000              = Config::from_hz(k_hz_4000);
        inline constexpr Config k_config_8000              = Config::from_hz(k_hz_8000);
    }

    /**
     * @brief Fixed timestep runner for deterministic updates.
     *
     * @tparam OnStepFunction Callable type for the per-step callback.
     * The default (std::function<void(nanoseconds)>) is suitable for most use cases.
	 * @tparam OnErrorFunction Callable type for the error callback when user step code throws.
	 * @tparam Clock Clock type for tick() timing. Defaults to std::chrono::steady_clock.
     * @see FixedTimestepRunner — the default std::function-based alias.
     */
    template<
        typename OnStepFunction = std::function<void(std::chrono::nanoseconds)>,
		typename OnErrorFunction = std::function<void()>,
        typename Clock = std::chrono::steady_clock>
    class BasicFixedTimestepRunner {
    public:
        BasicFixedTimestepRunner() = default;

        /**
        * @brief Constructs a runner with the given update function and configuration.
        * @param fn  The function to call for each fixed update step.
        * @param config Configuration. Defaults to 60 Hz with step sequencing.
        */
        explicit BasicFixedTimestepRunner(OnStepFunction fn, Config config = standards::k_config_60)
            : m_on_update_function(std::move(fn)), m_config(std::move(config)) { reset(true); }

        /**
        * @brief Constructs a runner with no update function (push_time / tick only).
        * @param config Configuration. Defaults to 60 Hz with step sequencing.
        */
        explicit BasicFixedTimestepRunner(std::nullptr_t, Config config = standards::k_config_60)
            : m_config(std::move(config)) { reset(true); }

        /**
        * @brief Resets internal state (accumulator, telemetry, pause). Does NOT reset stats.
        * @param start_now If true, records the current time as the baseline for tick(),
        *                  preventing a large initial delta on the first call.
        * @see reset_stats() to zero cumulative counters independently.
        */
        void reset(bool start_now = true) noexcept {
            m_accumulator       = std::chrono::nanoseconds(0);
            m_last_delta        = std::chrono::nanoseconds(0);
            m_last_steps        = 0;
            m_paused            = false;
            m_step_error_caught = false;
            m_started           = start_now;
            if (start_now) m_last = Clock::now();
        }

        /**
        * @brief Advances using the system steady_clock.
        * @return Alpha (interpolation factor) in [0, 1).
        * @note For deterministic operation (tests, replays) prefer push_time().
        */
        [[nodiscard]] double tick() noexcept { return tick_with_clock(Clock::now()); }

        /**
        * @brief Advances using an externally provided elapsed time.
        * @param elapsed Time since the last call.
        * @return Alpha (interpolation factor) in [0, 1).
        * @note Preferred over tick() for deterministic operation (tests, replays, lock-step networking).
        *       Results are bit-exact across machines because there is no wall-clock dependency.
        */
        [[nodiscard]] double push_time(std::chrono::nanoseconds elapsed) noexcept { return advance(elapsed); }

        /**
        * @brief Sets the target update rate. Regenerates the step sequence automatically.
        * @param hz Target Hz. Must be positive and finite.
        */
        void set_hz(double hz) noexcept {
            if (hz <= 0.0 || !std::isfinite(hz)) return;
            Config t = Config::from_hz(hz);
            m_config.target_hz            = t.target_hz;
            m_config.step                 = t.step;
            m_config.step_sequence        = t.step_sequence;
            m_config.step_sequence_length = t.step_sequence_length;
            m_step_sequence_index         = 0;
        }

        /**
        * @brief Target Hz as originally specified (e.g. 60.0 for 60 Hz).
        * Preserved for serialization — may differ from hz_calculated() due to rounding.
        */
        [[nodiscard]] double hz() const noexcept { return m_config.target_hz; }

        /**
        * @brief Actual Hz derived from the integer nanosecond step duration.
        * May differ slightly from hz() (e.g. 59.9999976 for a 16,666,667 ns step).
        */
        [[nodiscard]] constexpr double hz_calculated() const noexcept
            { return 1.0 / std::chrono::duration<double>(m_config.step).count(); }

        /**
        * @brief Sets the fixed timestep directly, disabling any step sequence.
        * @param s Step duration. Must be positive.
        */
        void set_step(std::chrono::nanoseconds s) noexcept {
            if (s.count() <= 0) return;
            m_config.step                 = s;
            m_config.step_sequence_length = 0;
            m_step_sequence_index         = 0;
            m_config.target_hz            = 1.0 / (static_cast<double>(s.count()) * 1e-9);
        }

        /// @brief Time remaining until the next fixed step fires.
        [[nodiscard]] std::chrono::nanoseconds time_to_next_step() const noexcept
            { return current_step_duration() - m_accumulator; }

        /// @brief The base fixed timestep duration (not accounting for sequence cycling).
        [[nodiscard]] constexpr std::chrono::nanoseconds step() const noexcept
            { return m_config.step; }

        /**
        * @brief Clamps large frame deltas to prevent the spiral of death (default: 250 ms).
        * @param d Maximum allowed delta. Must be positive.
        */
        void set_max_delta(std::chrono::nanoseconds d) noexcept {
            if (d.count() <= 0) return;
            m_config.safety_max_delta = d;
        }

        /// @brief Current max-delta clamp value.
        [[nodiscard]] std::chrono::nanoseconds max_delta() const noexcept
            { return m_config.safety_max_delta; }

        /**
        * @brief Caps fixed steps per tick to prevent the spiral of death (default: 8).
        * @param n Must be >= 1.
        */
        void set_max_substeps(size_t n) noexcept {
            m_config.safety_max_substeps = (n > 0 ? n : size_t{1});
        }

        /// @brief Current max-substeps cap.
        [[nodiscard]] size_t max_substeps() const noexcept { return m_config.safety_max_substeps; }

        /**
        * @brief Sets the time scale factor.
        * @param s > 1.0 fast-forward, < 1.0 slow-motion, 0.0 effective pause.
        *   Negative values are clamped to 0.0. Non-finite values (NaN, Inf) are ignored.
        */
        void set_time_scale(double s) noexcept {
            if (!std::isfinite(s)) return;
            m_config.time_scale = (s < 0.0) ? 0.0 : s;
        }

        /// @brief Current time scale factor.
        [[nodiscard]] double time_scale() const noexcept { return m_config.time_scale; }

        /**
        * @brief Pauses or resumes the runner.
        * While paused, tick() and push_time() return immediately without stepping.
        */
        void   pause(bool p = true) noexcept { m_paused = p; }
        /// @brief True if the runner is currently paused.
        [[nodiscard]] bool   paused() const noexcept { return m_paused; }
        /// @brief Alias for pause(false).
        void   resume() noexcept { m_paused = false; }
        /// @brief Flips the paused state.
        void   toggle_pause() noexcept { m_paused = !m_paused; }

        /// @brief Time buffered toward the next fixed step.
        [[nodiscard]] std::chrono::nanoseconds accumulator() const noexcept { return m_accumulator; }

        /// @brief Raw elapsed time from the last tick/push_time call, before clamping or scaling.
        [[nodiscard]] std::chrono::nanoseconds last_delta() const noexcept { return m_last_delta; }

        /// @brief Number of fixed steps executed during the last tick/push_time call.
        [[nodiscard]] size_t last_steps() const noexcept { return m_last_steps; }

        /// @brief Render-interpolation factor in [0, 1] towards the next fixed step.
        [[nodiscard]] double alpha() const noexcept {
            return static_cast<double>(m_accumulator.count()) /
                   static_cast<double>(current_step_duration().count());
        }

        /// @brief Checks if enough time has accumulated to trigger at least one fixed step.
        [[nodiscard]] bool can_step() const noexcept {
            return m_accumulator >= current_step_duration();
        }

        /// @brief True if an exception was caught in user step code during the last tick.
        [[nodiscard]] bool step_error_caught() const noexcept { return m_step_error_caught; }

#ifndef ISHAP_DISABLE_STATS
        /**
        * @brief Returns a snapshot of cumulative diagnostic counters.
        * Counters accumulate across resets; use reset_stats() to clear them.
        */
        [[nodiscard]] Stats stats() const noexcept { return m_stats; }

        /// @brief Zeroes all cumulative stat counters without affecting runner state.
        void reset_stats() noexcept { m_stats = {}; }
#endif

        /**
        * @brief Replaces the step callback.
        * For concrete-type runners (non-std::function OnStepFunction), the new callable
        * must be the same type as the one used at construction.
        */
        void set_step_function(OnStepFunction fn) { m_on_update_function = std::move(fn); }

        /// @brief True if a step callback is currently installed.
        [[nodiscard]] bool has_step_function() const noexcept {
            if (!m_on_update_function.has_value()) return false;
            // For std::function (and any type with explicit operator bool), also check inner validity.
            if constexpr (std::is_constructible_v<bool, const OnStepFunction&>)
                return static_cast<bool>(*m_on_update_function);
            return true;
        }

        /// @brief Sets the callback invoked when user step code throws an exception.
        void set_error_function(OnErrorFunction fn) { m_on_error_function = std::move(fn); }

        /// @brief True if an error callback is currently installed.
        [[nodiscard]] bool has_error_function() const noexcept {
            if (!m_on_error_function.has_value()) return false;
            if constexpr (std::is_constructible_v<bool, const OnErrorFunction&>)
                return static_cast<bool>(*m_on_error_function);
            return true;
        }

        /// @brief Step duration for the current position in the cycling sequence.
        [[nodiscard]] std::chrono::nanoseconds current_step_duration() const noexcept {
            if (m_config.step_sequence_length > 0)
                return m_config.step_sequence[m_step_sequence_index];
            return m_config.step;
        }

    private:
        using time_point = typename Clock::time_point;
        time_point                      m_last{};

        [[nodiscard]] double tick_with_clock(time_point tick_timepoint) noexcept {
            if (m_paused) {
                m_last_delta = std::chrono::nanoseconds(0);
                m_last_steps = 0;
                m_last = tick_timepoint;
                return alpha();
            }
            if (!m_started) { m_started = true; m_last = tick_timepoint; }
            auto raw = tick_timepoint - m_last;
            m_last = tick_timepoint;
            return advance(std::chrono::duration_cast<std::chrono::nanoseconds>(raw));
        }

        [[nodiscard]] double advance(std::chrono::nanoseconds raw_elapsed) noexcept {
            if (m_paused) { m_last_delta = std::chrono::nanoseconds(0); m_last_steps = 0; return alpha(); }
            m_step_error_caught = false;
            m_last_delta = raw_elapsed;

            // Clamp [Safety]
            std::chrono::nanoseconds dt = raw_elapsed;
            if (dt > m_config.safety_max_delta) {
                dt = m_config.safety_max_delta;
#ifndef ISHAP_DISABLE_STATS
                ++m_stats.clamped_delta_events;
#endif
            }

            // Time scale
            if (m_config.time_scale != 1.0) {
                dt = std::chrono::duration_cast<std::chrono::nanoseconds>(dt * m_config.time_scale);
            }

            m_accumulator += dt;

            // Step loop [with safety cap]
            size_t steps = 0;
            std::chrono::nanoseconds current_step_dt = current_step_duration();
            while (m_accumulator >= current_step_dt && steps < m_config.safety_max_substeps) {
                try {
                    if (has_step_function()) (*m_on_update_function)(current_step_dt);
                } catch (...) {
                    m_step_error_caught = true;
                    if (has_error_function()) (*m_on_error_function)();
                    // Swallow exceptions from user code to maintain noexcept guarantee
                }
                m_accumulator -= current_step_dt;
                ++steps;

                // Advance step sequence index if applicable
                if (m_config.step_sequence_length > 0) {
                    m_step_sequence_index = (m_step_sequence_index + 1) % m_config.step_sequence_length;
                    current_step_dt = current_step_duration();
                }
            }
#ifndef ISHAP_DISABLE_STATS
            if (steps == m_config.safety_max_substeps && m_accumulator >= current_step_dt) {
                ++m_stats.dropped_step_events;
            }
#endif
            m_last_steps = steps;
#ifndef ISHAP_DISABLE_STATS
            m_stats.total_steps += steps;
            ++m_stats.total_ticks;
#endif
            const std::chrono::nanoseconds overflow_cap =
                current_step_dt * m_config.safety_max_accumulator_overflow;
            if (m_accumulator > overflow_cap) { m_accumulator = overflow_cap; }

            return alpha();
        }
    private:
        std::optional<OnStepFunction>   m_on_update_function{};
        Config                          m_config{};
        bool                            m_started{false};
        std::chrono::nanoseconds        m_accumulator{0};
        size_t                          m_step_sequence_index{0};
        bool                            m_paused{false};
        bool                            m_step_error_caught{false};
        std::optional<OnErrorFunction>  m_on_error_function{};
#ifndef ISHAP_DISABLE_STATS
        Stats                           m_stats{};
#endif
        std::chrono::nanoseconds        m_last_delta{0};
        size_t                          m_last_steps{0};
    };
    /// @brief Standard alias using std::function for the step callback.
    using FixedTimestepRunner = BasicFixedTimestepRunner<>;
} // namespace ishap::timestep