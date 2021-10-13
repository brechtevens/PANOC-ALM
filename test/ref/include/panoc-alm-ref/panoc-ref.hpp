#pragma once

#include <panoc-alm/inner/decl/panoc.hpp>
#include <panoc-alm/inner/directions/lbfgs.hpp>

/// Reference implementations that are more readable than the optimized
/// implementations, used for tests as well
namespace pa_ref {

using pa::crvec;
using pa::PANOCParams;
using pa::Problem;
using pa::real_t;
using pa::rvec;
using pa::vec;

class PANOCSolver {
  public:
    using Params = PANOCParams;

    using Stats = pa::PANOCSolver<>::Stats;

    PANOCSolver(Params params, pa::LBFGSParams lbfgsparams)
        : params(params), lbfgs(lbfgsparams) {}

    Stats operator()(const Problem &problem,        // in
                     crvec Σ,                       // in
                     real_t ε,                      // in
                     bool always_overwrite_results, // in
                     rvec x,                        // inout
                     rvec y,                        // inout
                     rvec err_z);                   // out

    void stop() { stop_signal.store(true, std::memory_order_relaxed); }

  private:
    Params params;
    pa::PANOCDirection<pa::LBFGS> lbfgs;
    std::atomic<bool> stop_signal{false};
};

namespace detail {

vec eval_g(const Problem &p, crvec x);
vec eval_ẑ(const Problem &p, crvec x, crvec y, crvec Σ);
vec eval_ŷ(const Problem &p, crvec x, crvec y, crvec Σ);

real_t eval_ψ(const Problem &p, crvec x, crvec y, crvec Σ);
vec eval_grad_ψ(const Problem &p, crvec x, crvec y, crvec Σ);

vec gradient_step(const Problem &p, crvec x, crvec y, crvec Σ, real_t γ);
vec T_γ(const Problem &p, crvec x, crvec y, crvec Σ, real_t γ);

real_t eval_φ(const Problem &p, crvec x, crvec y, crvec Σ, real_t γ);

real_t estimate_lipschitz(const Problem &p, crvec x, crvec y, crvec Σ,
                          const PANOCParams &params);

real_t calc_error_stop_crit(const Problem &p, crvec xₖ, crvec x̂ₖ, crvec y,
                            crvec Σ, real_t γ);

bool lipschitz_check(const Problem &p, crvec xₖ, crvec x̂ₖ, crvec y, crvec Σ,
                     real_t γ, real_t L);

bool linesearch_condition(const Problem &p, crvec xₖ, crvec xₖ₊₁, crvec rₖ,
                          crvec y, crvec Σ, real_t γ, real_t σ);

} // namespace detail

} // namespace pa_ref
