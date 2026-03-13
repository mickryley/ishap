#include <gtest/gtest.h>
#include <ishap/ishap.hpp>
#include <thread>

using namespace std::chrono_literals;

TEST(IshapTests, AccumulatorFiresCorrectSteps) {
    int update_count = 0;
    ishap::timestep::FixedTimestepRunner runner{
        [&](std::chrono::nanoseconds dt) { update_count++; },
        {.step = 10ms}
    };

    const double alpha = runner.push_time(30ms);

    EXPECT_EQ(update_count, 3);
    EXPECT_EQ(runner.accumulator().count(), 0);
    EXPECT_DOUBLE_EQ(alpha, 0.0);
}

TEST(IshapTests, PartialAccumulatorAndAlpha) {
    ishap::timestep::FixedTimestepRunner runner{
        nullptr, {.step = 100ms}
    };

    const double alpha1 = runner.push_time(25ms);
    EXPECT_NEAR(alpha1, 0.25, 0.0001);

    const double alpha2 = runner.push_time(50ms);
    EXPECT_NEAR(alpha2, 0.75, 0.0001);
}

TEST(IshapTests, SequenceDriftCorrection60Hz) {
    ishap::timestep::FixedTimestepRunner runner;
    runner.set_hz(60.0);

    std::chrono::nanoseconds total_simulated{0};
    for(int i = 0; i < 60; ++i) {
        auto step_dur = runner.current_step_duration();
        total_simulated += step_dur;
        [[maybe_unused]] const double a = runner.push_time(step_dur);
    }

    EXPECT_EQ(total_simulated.count(), 1'000'000'000);
}

TEST(IshapTests, SafetyMaxDeltaClamping) {
    int updates = 0;
    ishap::timestep::FixedTimestepRunner runner{
        [&](auto){ updates++; },
        {.step = 10ms, .safety_max_delta = 50ms}
    };

    [[maybe_unused]] const double a = runner.push_time(5s);
    EXPECT_EQ(updates, 5);
}

TEST(IshapTests, SafetySubstepCap) {
    int updates = 0;
    ishap::timestep::FixedTimestepRunner runner{
        [&](auto){ updates++; },
        {.step = 10ms, .safety_max_substeps = 3}
    };

    [[maybe_unused]] const double a = runner.push_time(100ms);
    EXPECT_EQ(updates, 3);
}

TEST(IshapTests, AccumulatorOverflowTrimming) {
    ishap::timestep::FixedTimestepRunner runner{
        nullptr,
        {.step = 10ms, .safety_max_substeps = 1, .safety_max_accumulator_overflow = 2}
    };

    [[maybe_unused]] const double a = runner.push_time(100ms);
    EXPECT_EQ(runner.accumulator(), 20ms);
}

TEST(IshapTests, TimeScaling) {
    int updates = 0;
    ishap::timestep::FixedTimestepRunner runner{
        [&](auto){ updates++; },
        {.step = 10ms, .time_scale = 2.0}
    };

    [[maybe_unused]] const double a = runner.push_time(10ms);
    EXPECT_EQ(updates, 2);
}

TEST(IshapTests, PauseFunctionality) {
    int updates = 0;
    ishap::timestep::FixedTimestepRunner runner{[&](auto){ updates++; }};

    runner.pause(true);
    [[maybe_unused]] const double a1 = runner.push_time(100ms);
    EXPECT_EQ(updates, 0);

    runner.resume();
    [[maybe_unused]] const double a2 = runner.push_time(100ms);
    EXPECT_GT(updates, 0);
}

TEST(IshapTests, UserCodeExceptionHandling) {
    bool error_called = false;
    ishap::timestep::FixedTimestepRunner runner{
        [](auto){ throw std::runtime_error("User crash"); },
        {.step = 10ms}
    };

    runner.set_error_function([&](){ error_called = true; });

    EXPECT_NO_THROW({
        [[maybe_unused]] const double a = runner.push_time(10ms);
    });

    EXPECT_TRUE(runner.step_error_caught());
    EXPECT_TRUE(error_called);
}

TEST(IshapTests, InvalidInputs) {
    ishap::timestep::FixedTimestepRunner runner;
    auto original_step = runner.step();

    runner.set_hz(-60.0);
    EXPECT_EQ(runner.step(), original_step);

    runner.set_step(0ns);
    EXPECT_EQ(runner.step(), original_step);
}

TEST(IshapTests, TelemetryAccuracy) {
    ishap::timestep::FixedTimestepRunner runner{
        [](auto){},
        {.step = 10ms}
    };

    // 1. Single step scenario
    // Push 15ms. Should result in 1 step (10ms) and 5ms accumulator.
    [[maybe_unused]] const double a1 = runner.push_time(15ms);

    EXPECT_EQ(runner.last_steps(), 1);
    EXPECT_EQ(runner.last_delta(), 15ms); // last_delta is the raw pushed time
    EXPECT_EQ(runner.accumulator(), 5ms);

    // 2. Zero step scenario
    // Push 2ms. Accumulator becomes 7ms. Steps should be 0.
    [[maybe_unused]] const double a2 = runner.push_time(2ms);

    EXPECT_EQ(runner.last_steps(), 0);
    EXPECT_EQ(runner.last_delta(), 2ms);
    EXPECT_EQ(runner.accumulator(), 7ms);
}

TEST(IshapTests, HzCalculationPrecision) {
    ishap::timestep::FixedTimestepRunner runner;

    // Case A: Exact Integer Division (100 Hz -> 10ms)
    runner.set_hz(100.0);
    EXPECT_DOUBLE_EQ(runner.hz(), 100.0);
    // 10ms is exactly 1e7 ns, so 1.0 / 0.01 = 100.0
    EXPECT_DOUBLE_EQ(runner.hz_calculated(), 100.0);

    // Case B: Inexact Division (60 Hz)
    runner.set_hz(60.0);
    EXPECT_DOUBLE_EQ(runner.hz(), 60.0);
    // Calculated Hz relies on the integer nanosecond step (16,666,667ns)
    // 1 / 0.016666667 ~= 59.999998
    EXPECT_NE(runner.hz_calculated(), 60.0);
    EXPECT_NEAR(runner.hz_calculated(), 60.0, 0.0001);
}

TEST(IshapTests, HotSwapStepSize) {
    int count = 0;
    ishap::timestep::FixedTimestepRunner runner{
        [&](auto){ count++; },
        {.step = 10ms}
    };

    // Run normally
    [[maybe_unused]] const double a1 = runner.push_time(10ms);
    EXPECT_EQ(count, 1);

    // Change to 50ms steps dynamically
    runner.set_step(50ms);

    // Pushing 20ms should NOT trigger a step now (acc = 0 + 20 < 50)
    [[maybe_unused]] const double a2 = runner.push_time(20ms);
    EXPECT_EQ(count, 1);

    // Pushing another 30ms should trigger the step (acc = 50)
    [[maybe_unused]] const double a3 = runner.push_time(30ms);
    EXPECT_EQ(count, 2);
}

TEST(IshapTests, SequenceIndexResetsOnConfigChange) {
    ishap::timestep::FixedTimestepRunner runner;
    // Set to 60Hz to generate a step sequence
    runner.set_hz(60.0);

    auto dur1 = runner.current_step_duration();
    [[maybe_unused]] const double a1 = runner.push_time(dur1);
    // Internally, sequence index should have advanced

    // Changing Step/Hz should reset the index to 0
    runner.set_hz(30.0);

    // Duration matches the new config immediately
    EXPECT_EQ(runner.current_step_duration(), 33'333'333ns);
}

TEST(IshapTests, ResetClearsAccumulator) {
    ishap::timestep::FixedTimestepRunner runner{
        [](auto){},
        {.step = 100ms}
    };

    // Fill accumulator halfway
    [[maybe_unused]] const double a1 = runner.push_time(50ms);
    EXPECT_NE(runner.accumulator(), 0ns);
    EXPECT_GT(runner.alpha(), 0.0);

    runner.reset();

    EXPECT_EQ(runner.accumulator(), 0ns);
    EXPECT_DOUBLE_EQ(runner.alpha(), 0.0);
    EXPECT_EQ(runner.last_steps(), 0);
    EXPECT_EQ(runner.last_delta(), 0ns);
}

TEST(IshapTests, MoveSemanticsPreserveState) {
    int steps = 0;
    ishap::timestep::FixedTimestepRunner runner{
        [&](auto){ steps++; },
        {.step = 10ms}
    };

    // Pre-load some state
    [[maybe_unused]] const double a1 = runner.push_time(5ms); // Accumulator = 5ms

    // Move Construction
    ishap::timestep::FixedTimestepRunner moved_runner = std::move(runner);

    // New runner should have the 5ms accumulator.
    EXPECT_EQ(moved_runner.accumulator(), 5ms);

    // Push 5ms to the NEW runner -> should trigger step (5+5=10)
    [[maybe_unused]] const double a2 = moved_runner.push_time(5ms);
    EXPECT_EQ(steps, 1);
}

TEST(IshapTests, TimeScaleNegativeClampsToZero) {
    int steps = 0;
    ishap::timestep::FixedTimestepRunner runner{
        [&](auto){ steps++; },
        {.step = 10ms}
    };

    // User accidentally passes negative scale
    runner.set_time_scale(-5.0);
    EXPECT_DOUBLE_EQ(runner.time_scale(), 0.0);

    // Should behave like pause
    [[maybe_unused]] const double a = runner.push_time(100ms);
    EXPECT_EQ(steps, 0);
    EXPECT_EQ(runner.accumulator(), 0ns);
}

TEST(IshapTests, TogglePause) {
    ishap::timestep::FixedTimestepRunner runner;
    EXPECT_FALSE(runner.paused());

    runner.toggle_pause();
    EXPECT_TRUE(runner.paused());

    runner.toggle_pause();
    EXPECT_FALSE(runner.paused());
}

TEST(IshapTests, StepSequenceActuallyCycles) {
    ishap::timestep::FixedTimestepRunner runner;
    runner.set_hz(60.0);

    bool value_changed = false;
    auto first_val = runner.current_step_duration();

    // Run through a few cycles to see if the step duration adjusts
    for(int i=0; i < 10; ++i) {
        [[maybe_unused]] const double a = runner.push_time(first_val);

        if (runner.current_step_duration() != first_val) {
            value_changed = true;
            break;
        }
    }

    EXPECT_TRUE(value_changed) << "Step duration should vary for non-integer Hz (e.g. 60Hz)";
}

TEST(IshapTests, TickAdvancesTime) {
    int steps = 0;
    ishap::timestep::FixedTimestepRunner runner{
        [&](auto){ steps++; },
        {.step = 10ms}
    };

    // Initial tick to set m_last
    [[maybe_unused]] const double t1 = runner.tick();

    // Sleep for enough time to guarantee at least 1 step (20ms >> 10ms)
    std::this_thread::sleep_for(30ms);

    [[maybe_unused]] const double t2 = runner.tick();

    EXPECT_GE(steps, 1);
    EXPECT_LE(steps, 4);
}

TEST(IshapTests, ZeroDeltaDoesNothing) {
    int count = 0;
    ishap::timestep::FixedTimestepRunner runner{
        [&](auto){ count++; },
        {.step = 10ms}
    };

    [[maybe_unused]] const double a = runner.push_time(0ns);

    EXPECT_EQ(count, 0);
    // Accumulator should not change
    EXPECT_EQ(runner.accumulator(), 0ns);
    // Last delta should record the 0ns input
    EXPECT_EQ(runner.last_delta(), 0ns);
}

TEST(IshapTests, LongRunStability) {
    // Setup: 100Hz (10ms steps)
    size_t total_steps = 0;
    ishap::timestep::FixedTimestepRunner runner{
        [&](auto){ total_steps++; },
        {.step = 10ms}
    };

    // Simulate 1 Hour of runtime
    const int seconds_to_simulate = 3600;
    const int expected_steps = seconds_to_simulate * 100;

    // We push 1s chunks. 1s / 10ms = 100 steps per push.
    // We MUST increase the max substeps to allow processing 100 steps at once.
    runner.set_max_substeps(1000);

    // We also need to increase max_delta (default is 250ms) to allow 1s inputs.
    runner.set_max_delta(2s);

    for(int i = 0; i < seconds_to_simulate; ++i) {
        [[maybe_unused]] const double a = runner.push_time(1s);
    }

    EXPECT_EQ(total_steps, expected_steps);
    EXPECT_EQ(runner.accumulator(), 0ns) << "Accumulator should be empty after exact integer multiples";
}

TEST(IshapTests, CopySemanticsCreateIndependentState) {
    int steps1 = 0;
    ishap::timestep::FixedTimestepRunner runner1{
        [&](auto){ steps1++; },
        {.step = 10ms}
    };

    // 1. Add some state to runner1 (5ms accumulated)
    [[maybe_unused]] const double a1 = runner1.push_time(5ms);

    // 2. Create runner2 as a COPY of runner1
    ishap::timestep::FixedTimestepRunner runner2 = runner1;

    int steps2 = 0;
    // Bind a DIFFERENT function to runner2 to distinguish them
    runner2.set_step_function([&](auto){ steps2++; });

    // Verify state was copied (runner2 should also have 5ms accumulated)
    EXPECT_EQ(runner2.accumulator(), 5ms);

    // 3. Advance runner1 -> Should trigger step (5ms + 5ms = 10ms)
    [[maybe_unused]] const double a2 = runner1.push_time(5ms);

    // runner1 should have fired
    EXPECT_EQ(steps1, 1);
    EXPECT_EQ(runner1.accumulator(), 0ns);

    // runner2 should remain UNTOUCHED (still at 5ms accumulator, 0 steps)
    EXPECT_EQ(steps2, 0);
    EXPECT_EQ(runner2.accumulator(), 5ms);
}

TEST(IshapTests, ConstexprStandardsInitialization) {
    ishap::timestep::FixedTimestepRunner default_runner;
    EXPECT_DOUBLE_EQ(default_runner.hz(), 60.0);
    EXPECT_EQ(default_runner.step(), std::chrono::nanoseconds(16'666'666));

    // Test injecting a pre-calculated NTSC config directly
    int count = 0;
    ishap::timestep::FixedTimestepRunner ntsc_runner{
        [&](auto) { count++; },
        ishap::timestep::standards::k_config_ntsc_film_23976
    };

    EXPECT_DOUBLE_EQ(ntsc_runner.hz(), 24.0 * 1000.0 / 1001.0);

    EXPECT_EQ(ntsc_runner.current_step_duration(), std::chrono::nanoseconds(41'708'333));
}