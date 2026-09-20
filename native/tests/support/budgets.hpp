#pragma once

namespace vsa_test {

/// Whether the absolute CPU budgets of PLAN section 9 are asserted, rather than only measured.
///
/// The budgets are a claim about the hardware the mod targets. A shared CI runner is not that
/// hardware: it is about half the speed, and it shares its cores with the rest of the host, so a
/// render that costs 20 % of the block period on a developer machine measured 43 % there. Set
/// VSA_NO_PERF_BUDGETS=1 and the budget tests still build the scene, render it and report their
/// timings; only the pass/fail line is withheld.
///
/// Anything but "0" or the empty string counts as set. Read once, on first call.
bool perf_budgets_enforced();

}  // namespace vsa_test
