#pragma once
// Estimate the seconds remaining to reach 100% from a monotonically increasing
// progress percentage (0..100). Unit-agnostic: it works for byte-based recovery
// and record/phase-based scanning alike, because it only ever sees the progress
// number. The rate is an exponentially-smoothed "percent per second"; when
// progress stalls the last known rate is frozen (rather than decaying to zero,
// which would inflate the estimate), and a long gap between updates (pause or
// stall) resets the estimator so idle time is not counted as zero throughput.

#include <chrono>

namespace recovery {

class EtaEstimator {
public:
    using Clock = std::chrono::steady_clock;

    // Feed the current progress percentage (0..100). Returns the estimated
    // remaining seconds, or a negative value when no estimate is available yet.
    double update(int progress) {
        auto now = Clock::now();
        if (!armed_) {
            armed_ = true;
            last_p_ = progress;
            last_t_ = now;
            return last_eta_; // still no estimate (usually -1)
        }
        double dt = std::chrono::duration<double>(now - last_t_).count();
        int dp = progress - last_p_;
        last_p_ = progress;
        last_t_ = now;

        // A long gap means a pause or stall; forget the prior rate and re-seed,
        // so the idle interval isn't treated as zero throughput.
        if (dt > kGapSeconds) {
            rate_ = 0.0;
            return last_eta_; // stale, but only for one tick
        }
        if (dt < 1e-3) return last_eta_;

        // Only advance the smoothed rate when there is real progress movement.
        // dp == 0 (a stall) freezes the rate, keeping the last estimate honest
        // instead of letting the EMA decay toward zero and blow up the ETA.
        if (dp > 0) {
            double inst = static_cast<double>(dp) / dt;
            if (rate_ <= 0.0) {
                rate_ = inst; // seed on the first real movement
            } else {
                double alpha = dt / (kSmoothing + dt); // time constant ~kSmoothing s
                rate_ += alpha * (inst - rate_);
            }
        }

        if (rate_ <= 0.0 || progress >= 100) {
            last_eta_ = -1.0;
            return -1.0;
        }
        double eta = static_cast<double>(100 - progress) / rate_;
        if (eta > kMaxEta) eta = kMaxEta;
        last_eta_ = eta;
        return last_eta_;
    }

    // Estimated seconds remaining, or -1 when not yet available.
    double eta() const { return last_eta_; }

    // Rounded integer seconds (or -1). Use for JSON / atomic fields.
    long long eta_seconds() const {
        return last_eta_ >= 0.0 ? static_cast<long long>(last_eta_ + 0.5) : -1;
    }

    bool valid() const { return last_eta_ >= 0.0; }

    // Re-arm the estimator (forget rate and seed) at a phase transition, so a
    // byte-based phase's rate never carries over into a different progress
    // basis (which would otherwise produce a spurious estimate for one tick).
    void reset() {
        armed_ = false;
        last_p_ = 0;
        last_t_ = Clock::time_point{};
        rate_ = 0.0;
        last_eta_ = -1.0;
    }

private:
    static constexpr double kSmoothing = 5.0;   // EMA time constant (seconds)
    static constexpr double kGapSeconds = 3.0;  // > this gap resets the estimator
    static constexpr double kMaxEta = 7.0 * 24.0 * 3600.0; // clamp at 7 days

    bool armed_ = false;
    int  last_p_ = 0;
    Clock::time_point last_t_{};
    double rate_ = 0.0;
    double last_eta_ = -1.0;
};

} // namespace recovery
