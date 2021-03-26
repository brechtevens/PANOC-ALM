#pragma once

#include <cstddef>
#include <panoc-alm/inner/detail/panoc-helpers.hpp>
#include <panoc-alm/util/atomic_stop_signal.hpp>
#include <panoc-alm/util/solverstatus.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace pa {

struct PGAParams {
    struct {
        /// Relative step size for initial finite difference Lipschitz estimate.
        real_t ε = 1e-6;
        /// Minimum step size for initial finite difference Lipschitz estimate.
        real_t δ = 1e-12;
        /// Factor that relates step size γ and Lipschitz constant.
        real_t Lγ_factor = 0.95;
    } Lipschitz; ///< Parameters related to the Lipschitz constant estimate
                 ///  and step size.
    /// Maximum number of inner iterations.
    unsigned max_iter = 100;
    /// Maximum duration.
    std::chrono::microseconds max_time = std::chrono::minutes(5);

    /// When to print progress. If set to zero, nothing will be printed.
    /// If set to N != 0, progress is printed every N iterations.
    unsigned print_interval = 0;
};

/// Standard Proximal Gradient Algorithm without any bells and whistles.
///
/// @ingroup    grp_InnerSolvers
class PGA {
  public:
    using Params = PGAParams;

    PGA(const Params &params) : params(params) {}

    struct Stats {
        unsigned iterations = 0;
        real_t ε            = inf;
        std::chrono::microseconds elapsed_time;
        SolverStatus status = SolverStatus::Unknown;

        unsigned linesearch_failures = 0; // TODO: unused
        unsigned lbfgs_failures      = 0; // TODO: unused
        unsigned lbfgs_rejected      = 0; // TODO: unused
    };

    Stats operator()(const Problem &problem, // in
                     const vec &Σ,           // in
                     real_t ε,               // in
                     vec &x,                 // inout
                     vec &λ,                 // inout
                     vec &err_z);            // out

    std::string get_name() const { return "PGA"; }

    void stop() { stop_signal.stop(); }

    const Params &get_params() const { return params; }

  private:
    Params params;
    AtomicStopSignal stop_signal;
};

using std::chrono::duration_cast;
using std::chrono::microseconds;

inline PGA::Stats PGA::operator()(const Problem &problem, // in
                                  const vec &Σ,           // in
                                  real_t ε,               // in
                                  vec &x,                 // inout
                                  vec &y,                 // inout
                                  vec &err_z              // out
) {
    auto start_time = std::chrono::steady_clock::now();
    Stats s;

    const auto n = problem.n;
    const auto m = problem.m;

    vec xₖ = x,      // Value of x at the beginning of the iteration
        x̂ₖ(n),       // Value of x after a projected gradient step
        pₖ(n),       // Projected gradient step
        ŷₖ(m),       // <?>
        grad_ψₖ(n),  // ∇ψ(xₖ)
        grad_ψx̂ₖ(n); // ∇ψ(x̂ₖ)

    vec work_n(n), work_m(m);

    // Helper functions --------------------------------------------------------

    // Wrappers for helper functions that automatically pass along any arguments
    // that are constant within PGA (for readability in the main algorithm)
    auto calc_ψ_ŷ = [&problem, &y, &Σ](const vec &x, vec &ŷ) {
        return detail::calc_ψ_ŷ(problem, x, y, Σ, ŷ);
    };
    auto calc_ψ_grad_ψ = [&problem, &y, &Σ, &work_n, &work_m](const vec &x,
                                                              vec &grad_ψ) {
        return detail::calc_ψ_grad_ψ(problem, x, y, Σ, grad_ψ, work_n, work_m);
    };
    auto calc_grad_ψ = [&problem, &y, &Σ, &work_n, &work_m](const vec &x,
                                                            vec &grad_ψ) {
        detail::calc_grad_ψ(problem, x, y, Σ, grad_ψ, work_n, work_m);
    };
    auto calc_grad_ψ_from_ŷ = [&problem, &work_n](const vec &x, const vec &ŷ,
                                                  vec &grad_ψ) {
        detail::calc_grad_ψ_from_ŷ(problem, x, ŷ, grad_ψ, work_n);
    };
    auto calc_x̂ = [&problem](real_t γ, const vec &x, const vec &grad_ψ, vec &x̂,
                             vec &p) {
        detail::calc_x̂(problem, γ, x, grad_ψ, x̂, p);
    };
    auto calc_err_z = [&problem, &y, &Σ](const vec &x̂, vec &err_z) {
        detail::calc_err_z(problem, x̂, y, Σ, err_z);
    };
    auto print_progress = [&](unsigned k, real_t ψₖ, const vec &grad_ψₖ,
                              const vec &pₖ, real_t γₖ, real_t εₖ) {
        std::cout << "[PGA]   " << std::setw(6) << k
                  << ": ψ = " << std::setw(13) << ψₖ
                  << ", ‖∇ψ‖ = " << std::setw(13) << grad_ψₖ.norm()
                  << ", ‖p‖ = " << std::setw(13) << pₖ.norm()
                  << ", γ = " << std::setw(13) << γₖ
                  << ", εₖ = " << std::setw(13) << εₖ << "\r\n";
    };

    // Estimate Lipschitz constant ---------------------------------------------

    // Finite difference approximation of ∇²ψ in starting point
    vec h(n);
    h = (x * params.Lipschitz.ε).cwiseAbs().cwiseMax(params.Lipschitz.δ);
    x += h;

    // Calculate ∇ψ(x₀ + h)
    calc_grad_ψ(x, /* in ⟹ out */ grad_ψx̂ₖ);

    // Calculate ∇ψ(x₀)
    real_t ψₖ = calc_ψ_grad_ψ(xₖ, /* in ⟹ out */ grad_ψₖ);

    // Estimate Lipschitz constant
    real_t Lₖ = (grad_ψx̂ₖ - grad_ψₖ).norm() / h.norm();
    if (Lₖ < std::numeric_limits<real_t>::epsilon()) {
        Lₖ = std::numeric_limits<real_t>::epsilon();
    } else if (not std::isfinite(Lₖ)) {
        s.status = SolverStatus::NotFinite;
        return s;
    }

    real_t γₖ = params.Lipschitz.Lγ_factor / Lₖ;

    unsigned no_progress = 0;

    // Main loop
    // =========================================================================
    for (unsigned k = 0; k <= params.max_iter; ++k) {
        // From previous iteration:
        //  - xₖ
        //  - grad_ψₖ
        //  - ψₖ

        // Quadratic upper bound -----------------------------------------------

        // Projected gradient step: x̂ₖ and pₖ
        calc_x̂(γₖ, xₖ, grad_ψₖ, /* in ⟹ out */ x̂ₖ, pₖ);
        // Calculate ψ(x̂ₖ) and ŷ(x̂ₖ)
        real_t ψx̂ₖ = calc_ψ_ŷ(x̂ₖ, /* in ⟹ out */ ŷₖ);
        // Calculate ∇ψ(xₖ)ᵀpₖ and ‖pₖ‖²
        real_t grad_ψₖᵀpₖ = grad_ψₖ.dot(pₖ);
        real_t norm_sq_pₖ = pₖ.squaredNorm();
        real_t margin     = 0; // 1e-6 * std::abs(ψₖ₊₁); // TODO: Why?

        // Decrease step size until quadratic upper bound is satisfied
        while (ψx̂ₖ > ψₖ + margin + grad_ψₖᵀpₖ + 0.5 * Lₖ * norm_sq_pₖ) {
            Lₖ *= 2;
            γₖ /= 2;

            // Projected gradient step: x̂ₖ and pₖ (with new step size)
            calc_x̂(γₖ, xₖ, grad_ψₖ, /* in ⟹ out */ x̂ₖ, pₖ);
            // Calculate ψ(x̂ₖ) and ŷ(x̂ₖ)
            ψx̂ₖ = calc_ψ_ŷ(x̂ₖ, /* in ⟹ out */ ŷₖ);
            // Calculate ∇ψ(xₖ)ᵀpₖ and ‖pₖ‖²
            grad_ψₖᵀpₖ = grad_ψₖ.dot(pₖ);
            norm_sq_pₖ = pₖ.squaredNorm();
        }

        // Calculate ∇ψ(x̂ₖ)
        calc_grad_ψ_from_ŷ(x̂ₖ, ŷₖ, /* in ⟹ out */ grad_ψx̂ₖ);

        // Check stop condition ------------------------------------------------

        real_t εₖ = detail::calc_error_stop_crit(pₖ, γₖ, grad_ψx̂ₖ, grad_ψₖ);

        // Print progress
        if (params.print_interval != 0 && k % params.print_interval == 0)
            print_progress(k, ψₖ, grad_ψₖ, pₖ, γₖ, εₖ);

        auto time_elapsed    = std::chrono::steady_clock::now() - start_time;
        bool out_of_time     = time_elapsed > params.max_time;
        bool out_of_iter     = k == params.max_iter;
        bool interrupted     = stop_signal.stop_requested();
        bool not_finite      = not std::isfinite(εₖ);
        bool conv            = εₖ <= ε;
        bool max_no_progress = no_progress > 1;
        bool exit = conv || out_of_iter || out_of_time || not_finite ||
                    interrupted || max_no_progress;
        if (exit) {
            // TODO: We could cache g(x) and ẑ, but would that faster?
            //       It saves 1 evaluation of g per ALM iteration, but requires
            //       many extra stores in the inner loops.
            // TODO: move the computation of ẑ and g(x) to ALM?
            calc_err_z(x̂ₖ, /* in ⟹ out */ err_z);
            x              = std::move(x̂ₖ);
            y              = std::move(ŷₖ);
            s.iterations   = k; // TODO: what do we count as an iteration?
            s.ε            = εₖ;
            s.elapsed_time = duration_cast<microseconds>(time_elapsed);
            s.status       = conv              ? SolverStatus::Converged
                             : out_of_time     ? SolverStatus::MaxTime
                             : out_of_iter     ? SolverStatus::MaxIter
                             : not_finite      ? SolverStatus::NotFinite
                             : max_no_progress ? SolverStatus::NoProgress
                                               : SolverStatus::Interrupted;
            return s;
        }

        if (xₖ == x̂ₖ) // TODO: this is quite expensive to do on each iteration
            ++no_progress;
        else
            no_progress = 0;

        xₖ.swap(x̂ₖ);
        grad_ψₖ.swap(grad_ψx̂ₖ);
        ψₖ = ψx̂ₖ;
    }
    throw std::logic_error("[PGA]   loop error");
}

} // namespace pa