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
//       {.step = 16ms, .max_delta = 250ms, .max_substeps = 8, .time_scale = 1.0}
//   };
//   for (;;) {
//       // Game loop …
//       const double a = runner.tick(); // or runner.push_time(frame_dt);
//       // render(interpolate(a));
//   }
//
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>
#include <cmath>
#include <array>

namespace ishap::timestep {

    inline constexpr double                     
        k_hz_60                             = 60.0;
    inline constexpr double                     
        k_hz_120                            = 120.0;
    inline constexpr double
        k_hz_240                            = 240.0;

    inline constexpr std::chrono::nanoseconds   
        k_step_60hz                         { 16'666'667 }; // ~16.67ms
    inline constexpr std::chrono::nanoseconds   
        k_step_120hz                        { 8'333'333 };  // ~8.33ms
    inline constexpr std::chrono::nanoseconds   
        k_step_240hz                        { 4'166'667 };  // ~4.17ms

    inline constexpr std::chrono::nanoseconds   
        k_default_max_delta                 { 250'000'000 }; // 250ms
    inline constexpr size_t                     
        k_default_max_substeps              = 8;
    inline constexpr size_t                     
        k_default_max_accumulator_overflow  = 3;
    inline constexpr double                     
        k_default_time_scale                = 1.0;

    /// @brief Maximum length for step sequence array
    inline constexpr size_t k_max_step_sequence_length = 32;

    /// @brief Epsilon for comparing step sequence timing
    inline constexpr double k_step_sequence_epsilon = 1e-6;

	/// @brief Configuration settings for the timestep runner
struct Config {
	/// @brief Target fixed update step duration (default: ~16.67ms for 60Hz)
    std::chrono::nanoseconds 	step        						= k_step_60hz;
    /// @brief Used for serialization to preserve "60.0" instead of "60.0000024".
	double						target_hz							= k_hz_60;
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
};

/// @brief Fixed timestep runner for deterministic updates
class FixedTimestepRunner {
public:
    using OnStepFunction = std::function<void(std::chrono::nanoseconds)>;
    using OnErrorFunction = std::function<void()>;

    FixedTimestepRunner() = default;

	/**
	* @brief Constructs a FixedTimestepRunner with the given update function and configuration.
	* @param fn The function to call for each fixed update step. It takes a single parameter of type std::chrono::nanoseconds representing the fixed timestep duration.
	* @param config The configuration for the timestep runner. Default values are provided if not specified
	*/
    explicit FixedTimestepRunner(OnStepFunction fn, Config config = {})
        : m_on_update_function(std::move(fn)), m_config(std::move(config)) { reset(true); }

    /**
	* @brief Resets the internal state of the timestep runner.
	* @param start_now If true, sets the last time point to the current time. Default is true.
	* This effectively starts the timing from now, avoiding a large initial delta on the first tick
	*/
    void reset(bool start_now = true) noexcept {
        m_accumulator 			= std::chrono::nanoseconds(0); 
        m_last_delta 			= std::chrono::nanoseconds(0);
        m_last_steps 			= 0;
        m_paused 				= false;
        if (start_now) m_last 	= steady_clock::now();
    }

	/**
	* @brief Advances the timestep runner using the current time from a steady clock.
	* @return The interpolation alpha value in the range [0, 1], representing the
	*         fraction of the next fixed timestep that has been accumulated.
	*/
    [[nodiscard]] double tick() noexcept { return tick_with_clock(steady_clock::now()); }

	/**
	* @brief Advances the timestep runner using an externally provided elapsed time.
	* @param elapsed The elapsed time since the last call, as a std::chrono::nanoseconds duration.
	* @return The interpolation alpha value in the range [0, 1), representing the
	*         fraction of the next fixed timestep that has been accumulated.
	*/
    [[nodiscard]] double push_time(std::chrono::nanoseconds elapsed) noexcept { return advance(elapsed); }

    /**
	* @brief Sets the target fixed update rate in Hertz.
    * @details Automatically generates a vector of cycling timesteps if the Hz results
    * in a non-integer nanosecond period (e.g. 30Hz), ensuring long-term timing accuracy.
	* @param hz The desired update rate in Hertz. Must be positive.
	*/
    void   set_hz(double hz) noexcept            
		{ 
            if (hz <= 0.0 || !std::isfinite(hz)) return;
            m_config.target_hz = hz;
            m_config.step = std::chrono::duration_cast
			<std::chrono::nanoseconds>(std::chrono::duration<double>(1.0 / hz)); 
            
            // Generate step sequence for non-integer nanosecond periods
            m_step_sequence_index = 0;
            m_config.step_sequence.fill(std::chrono::nanoseconds(0));
            m_config.step_sequence_length = 0;

            double period_ns_double = 1e9 / hz;
            double ideal_accum = 0.0;
            int64_t discrete_accum = 0;

            for (size_t i = 0; i < k_max_step_sequence_length; ++i) 
            {
                ideal_accum += period_ns_double;
                int64_t target_discrete = static_cast<int64_t>(std::round(ideal_accum));
                int64_t step_ns = target_discrete - discrete_accum;

                m_config.step_sequence[i] = std::chrono::nanoseconds(step_ns);
                discrete_accum += step_ns;
                m_config.step_sequence_length++;

                if (std::abs(ideal_accum - static_cast<double>(discrete_accum)) < k_step_sequence_epsilon)
                {
                    if (m_config.step_sequence_length <= 1) m_config.step_sequence_length = 0; // No sequence needed
                    break;
                }
            }
        }

    /** 
    * @brief Gets the target fixed update rate in Hertz.
    * This may differ from the calculated rate due to rounding, use hz_calculated() for exact value. (60.0 for 16.67ms step)
    */
	[[nodiscard]] double hz() const noexcept                    
		{ return m_config.target_hz; }

    /** 
    * @brief Gets the current fixed update rate in Hertz.
    * This is calculated based on the actual step duration and may differ from the target rate. (60.0000024 for 16.67ms step)
    */
	[[nodiscard]] constexpr double hz_calculated() const noexcept                    
		{ return 1.0 / std::chrono::duration<double>(m_config.step).count(); }

	/**
	* @brief Sets the fixed timestep duration.
	* @param s The desired fixed timestep duration as a std::chrono::nanoseconds duration.
	*/
    void   set_step(std::chrono::nanoseconds s) noexcept { 
        if (s.count() <= 0) return;
        m_config.step = s; 
        m_config.step_sequence_length = 0;
        m_step_sequence_index = 0;
        m_config.target_hz = 1.0 / (static_cast<double>(s.count()) * 1e-9);
    }

    /// @brief Get the time remaining to the next fixed step in nanoseconds.
    [[nodiscard]] std::chrono::nanoseconds time_to_next_step() const noexcept
        { return current_step_duration() - m_accumulator; }

	/// @brief Get the current fixed timestep duration in nanoseconds.
    [[nodiscard]] constexpr std::chrono::nanoseconds step() const noexcept             
        { return m_config.step; }

	/**
	* @brief Sets the maximum allowed delta time between updates to prevent spiral of death.
	* @param d The maximum delta time as a std::chrono::nanoseconds duration.
	*/
    void   set_max_delta(std::chrono::nanoseconds d) noexcept {
        if (d.count() <= 0) return;
        m_config.safety_max_delta = d; 
        }

	/// @brief Get the current maximum allowed delta time in nanoseconds.
    [[nodiscard]] std::chrono::nanoseconds max_delta() const noexcept
        { return m_config.safety_max_delta; }

	/**
	* @brief Sets the maximum number of fixed update steps to perform per tick to prevent spiral of death.
	* @param n The maximum number of substeps. Must be positive.
	*/
    void    set_max_substeps(size_t n) noexcept { 
        m_config.safety_max_substeps = (n > 0 ? n : size_t{1}); 
    }
	/// @brief Get the current maximum number of fixed update steps per tick.
    [[nodiscard]] size_t    max_substeps() const noexcept     { return m_config.safety_max_substeps; }

	/**
	* @brief Sets the time scale factor for speeding up or slowing down time.
	* @param s The time scale factor. Values > 1.0 speed up time, values < 1.0 slow down time.
	*            A value of 1.0 represents normal time.
	*            A value of 0.0 effectively pauses time (use pause() instead for clarity).
    *            Negative values are clamped to 0.0.
	*/
    void   set_time_scale(double s) noexcept {
        if (s < 0.0) s = 0.0;
         m_config.time_scale = s; 
        }
	/// @brief Get the current time scale factor.
    [[nodiscard]] double time_scale() const noexcept            { return m_config.time_scale; }

	/**
	* @brief Pauses or unpauses the timestep runner.
	* @param p If true, pauses the runner; if false, unpauses it. Default is true.
	* When paused, calls to tick() or push_time() will not advance time or call the update function.
	*/
    void   pause(bool p=true) noexcept      { m_paused = p; }
	/// @brief Returns whether the timestep runner is currently paused.
    [[nodiscard]] bool   paused() const noexcept               { return m_paused; }
	/// @brief Unpauses the timestep runner. Alias for pause(false).
	void   resume() noexcept                { m_paused = false; }
	/// @brief Toggles the paused state of the timestep runner.
	void   toggle_pause() noexcept          { m_paused = !m_paused; }

    /**
	* @brief Get the current accumulator value.
	* @return The amount of time currently accumulated towards the next fixed timestep, as a std::chrono::nanoseconds duration.
	*/
    [[nodiscard]] std::chrono::nanoseconds  accumulator() const noexcept    { return m_accumulator; }
	/**
	* @brief Get the last frame's raw delta time before clamping and time scaling.
	* @return The last frame's delta time as a std::chrono::nanoseconds duration.
	*/
    [[nodiscard]] std::chrono::nanoseconds  last_delta() const noexcept     { return m_last_delta; } 
	/**
	* @brief Get the number of fixed update steps executed in the last tick.
	* @return The number of fixed update steps executed during the last call to tick() or push_time().
	*/
    [[nodiscard]] size_t                    last_steps() const noexcept     { return m_last_steps; } 
	/**
	* @brief Get the interpolation factor for the last frame.
	* @return The interpolation factor as a double in the range [0, 1].
	*/
    [[nodiscard]] double alpha() const noexcept
		{ return    static_cast<double>(m_accumulator.count()) /
			        static_cast<double>(current_step_duration().count()); 
    }

     /// @brief Returns whether a step error was caught in user code during the last tick.
    [[nodiscard]] bool step_error_caught() const noexcept { return m_step_error_caught; }

    /**
	* @brief Sets the step function to be called for each fixed timestep.
	* @param fn The step function, which takes a single parameter of type std::chrono::nanoseconds.
	*/
    void   set_step_function(OnStepFunction fn)          { m_on_update_function = std::move(fn); }
	/// @brief Returns whether a step function is set.
    [[nodiscard]] bool   has_step_function() const noexcept {
        return static_cast<bool>(m_on_update_function);
    }

    /**
	* @brief Sets the error function to be called when a step error is caught.
	* @param fn The error function, which takes no parameters and returns void.
	*/
    void set_error_function(OnErrorFunction fn) { m_on_error_function = std::move(fn); }
    /// @brief Returns whether an error function is set.
    [[nodiscard]] bool has_error_function() const noexcept {
         return static_cast<bool>(m_on_error_function); 
        }

    /**
    * @brief Helper to get the step duration for the current cycle index
    */
    [[nodiscard]] inline std::chrono::nanoseconds current_step_duration() const noexcept {
        if (m_config.step_sequence_length > 0) 
            return m_config.step_sequence[m_step_sequence_index];
        return m_config.step;
    }

private:
    using steady_clock 				= std::chrono::steady_clock;

	/**
	* @brief Advances the timestep runner using a provided time point from a steady clock.
	* @param tick_timepoint The current time point from a steady clock.
	* @return The interpolation alpha value in the range [0, 1), representing the
	*         fraction of the next fixed timestep that has been accumulated.
	*/
    [[nodiscard]] double tick_with_clock(steady_clock::time_point tick_timepoint) noexcept {
        if (m_paused) { m_last_delta = std::chrono::nanoseconds(0); m_last_steps = 0; m_last = tick_timepoint; return alpha(); }
        if (m_last.time_since_epoch().count() == 0) m_last = tick_timepoint; // first call safety
        auto raw = tick_timepoint - m_last;
        m_last = tick_timepoint;
        return advance(std::chrono::duration_cast<std::chrono::nanoseconds>(raw));
    }

	/**
	* @brief Advances the timestep runner by a specified elapsed time.
    * @details This is the core logic that handles clamping, time scaling, stepping, and accumulator management.
    * It is called by both tick_with_clock() and push_time().
	* @param raw_elapsed The elapsed time since the last call, as a std::chrono::nanoseconds duration.
	* @return The interpolation alpha value in the range [0, 1), representing the
	*         fraction of the next fixed timestep that has been accumulated.
	*/
    [[nodiscard]] double advance(std::chrono::nanoseconds raw_elapsed) noexcept {
        if (m_paused) { m_last_delta = std::chrono::nanoseconds(0); m_last_steps = 0; return alpha(); }
        m_step_error_caught = false;
        m_last_delta = raw_elapsed;

		// Clamp [Safety]
        std::chrono::nanoseconds dt = raw_elapsed;
        if (dt > m_config.safety_max_delta) dt = m_config.safety_max_delta;

		// Time scale
        if (m_config.time_scale != 1.0) {
			    dt = std::chrono::duration_cast<std::chrono::nanoseconds>(dt * m_config.time_scale);
        }

        m_accumulator += dt;

        // Step Loop [with Safety Cap]
        size_t steps = 0;
        std::chrono::nanoseconds current_step_dt = current_step_duration();
        while (m_accumulator >= current_step_dt && steps < m_config.safety_max_substeps) {
            try {
                if (m_on_update_function) m_on_update_function(current_step_dt);
            } catch (...) {
                m_step_error_caught = true;
                if (m_on_error_function) m_on_error_function();
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
        m_last_steps = steps;

		// Trim excess accumulator [With Safety Cap]
		const std::chrono::nanoseconds max_acc = current_step_dt * m_config.safety_max_accumulator_overflow;
		if (m_accumulator > max_acc) { m_accumulator = max_acc; }
		
        // Return (trimmed) alpha for interpolation into next step
        return alpha();
    }

private:
	/// @brief Update / Step function to call each fixed step
    OnStepFunction              	m_on_update_function{};
	/// @brief Configuration settings for the timestep runner
    Config                        	m_config{};

	/// @brief Last time point recorded (for tick())
    steady_clock::time_point      	m_last{};
	/// @brief Accumulator for leftover time between steps
    std::chrono::nanoseconds      	m_accumulator{0};
    /// @brief Index into step_sequence for cycling timesteps
    size_t                        	m_step_sequence_index{0};
	/// @brief Paused state
    bool                          	m_paused{false};

    /// @brief Step Error Caught
    bool                            m_step_error_caught{false};
    /// @brief Optional error callback for step errors
    OnErrorFunction                 m_on_error_function{};

	// --- Telemetry ---
	/// @brief Frame delta tracking (pre-clamp, pre-scale) for telemetry
    std::chrono::nanoseconds      	m_last_delta{0};
	/// @brief Number of steps taken in last tick for telemetry
    size_t                        	m_last_steps{0};
};

} // namespace ishap::timestep
