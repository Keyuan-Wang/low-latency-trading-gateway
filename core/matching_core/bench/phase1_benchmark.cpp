/**
 * @file phase1_benchmark.cpp
 * @brief Phase-1 single-operation latency micro-benchmark for @ref matching::OrderBook.
 *
 * @details
 * Measures wall-clock latency of individual order-book operations under
 * various synthetic scenarios. Each iteration constructs a fresh book,
 * optionally pre-fills it with resting orders, executes one timed operation,
 * and records the elapsed nanoseconds.
 *
 * Supported scenarios:
 * - **lmt_rest**: limit order that rests on empty book (no match).
 * - **lmt_cross_deep**: limit order that sweeps through many resting price levels.
 * - **mkt_sweep_deep**: market order that sweeps through many resting price levels.
 * - **cxl_miss**: cancel for an order-id not on the book (fast-path rejection).
 * - **dup_reject**: insert with a duplicate order-id (fast-path rejection).
 *
 * Output prints percentile latencies (p50, p95, p99), average, throughput
 * in ops/s, and optionally appends a CSV row with the same metrics.
 */

#include "matching/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

/** @brief Convenience alias for the monotonic clock used in all measurements. */
using Clock = std::chrono::steady_clock;

/**
 * @brief Parsed command-line arguments controlling the benchmark run.
 */
struct Args {
    std::string scenario = "lmt_benchmark";   ///< Benchmark scenario name (see @ref main).
    std::uint64_t orders = 10000;              ///< Number of resting orders used in prefill.
    std::uint64_t levels = 100;                ///< Number of distinct price levels in prefill.
    std::uint64_t iters = 2000;                ///< Number of timed iterations to execute.
    std::uint64_t seed = 42;                   ///< PRNG seed (reserved for future use).
    std::string out_csv;                       ///< If non-empty, append results as a CSV row to this file.
};

/**
 * @brief Parse benchmark parameters from `argv`.
 *
 * Recognised flags:
 * - `--scenario <name>`
 * - `--orders <N>`
 * - `--levels <N>`
 * - `--iters <N>`
 * - `--seed <N>`
 * - `--out <path>`
 *
 * @param argc Argument count from `main`.
 * @param argv Argument vector from `main`.
 * @return Populated @ref Args struct with defaults overridden by any supplied flags.
 */
static Args parse_args(int argc, char** argv) {
    Args a{};
    for (int i = 1; i < argc; ++i) {
      std::string s = argv[i];
      auto next = [&](std::uint64_t& v) { v = std::stoull(argv[++i]); };
      if (s == "--scenario") a.scenario = argv[++i];
      else if (s == "--orders") next(a.orders);
      else if (s == "--levels") next(a.levels);
      else if (s == "--iters") next(a.iters);
      else if (s == "--seed") next(a.seed);
      else if (s == "--out") a.out_csv = argv[++i];
    }
    return a;
}

/**
 * @brief Compute the @p p -th percentile (linear interpolation) of a vector of
 *        nanosecond durations.
 *
 * Sorts the input in-place (copy). Uses the standard "percentile rank" formula
 * with linear interpolation between the two bracketing samples.
 *
 * @param v  Vector of durations in nanoseconds.
 * @param p  Percentile in [0.0, 1.0] (e.g. 0.50 for median, 0.99 for p99).
 * @return   Linearly interpolated value at the requested percentile, or 0.0 if
 *           the vector is empty.
 */
static double percentile_ns(std::vector<std::uint64_t> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = p * (v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(pos);
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = pos - lo;
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

/**
 * @brief Build a deep synthetic order book by inserting resting sell orders
 *        across multiple price levels.
 *
 * Each level receives approximately `orders / levels` orders. Prices start at
 * 1000 and increase by 1 per level. All orders are of quantity 1.
 *
 * @param book     The order book to fill (assumed empty on entry).
 * @param orders   Total number of resting orders to insert.
 * @param levels   Number of distinct price levels to spread orders across.
 * @param id_base  Starting order-id; incremented by 1 for each inserted order.
 */
static void prefill_book(matching::OrderBook& book, std::uint64_t orders,
                         std::uint64_t levels, std::uint64_t id_base) {
    const std::uint64_t per_level = std::max<std::uint64_t>(1, orders / std::max<std::uint64_t>(1, levels));
    std::uint64_t id = id_base;
    for (std::uint64_t lvl = 0; lvl < levels; ++lvl) {
        const std::int64_t ask_price = 1000 + static_cast<std::int64_t>(lvl);
        for (std::uint64_t j = 0; j < per_level; ++j) {
            (void)book.add_limit_order(id, matching::Side::Sell, ask_price, 1, id);
            id++;
        }
    }
}

/**
 * @brief Entry point for the Phase-1 micro-benchmark.
 *
 * @details
 * Runs `iters` timed iterations. In each iteration:
 * 1. A fresh @ref matching::OrderBook is constructed.
 * 2. If the scenario requires a pre-filled book (lmt_cross_deep, mkt_sweep_deep,
 *    cxl_miss, dup_reject), @ref prefill_book is called outside the timed region.
 * 3. For `dup_reject`, one extra limit order is inserted to make order-id 7
 *    already resting, so the timed duplicate insert will hit the fast rejection path.
 * 4. A single operation is timed with `Clock::now()` — the scenario determines
 *    which operation.
 * 5. Wall-clock durations are collected and aggregated into percentile / average
 *    statistics printed to stdout and optionally appended to a CSV file.
 *
 * @return 0 on success, 2 if an unknown scenario name was supplied.
 */
int main(int argc, char** argv) {
    const Args a = parse_args(argc, argv);
    std::vector<std::uint64_t> lat_ns;
    lat_ns.reserve(a.iters);
    std::uint64_t ok = 0;
    const auto wall_start = Clock::now();
    for (std::uint64_t i = 0; i < a.iters; ++i) {
      matching::OrderBook book;
      const std::uint64_t id0 = 1'000'000ULL + i * 10'000ULL;

      // --- Setup phase (outside the timed region) ---
      if (a.scenario == "lmt_cross_deep" || a.scenario == "mkt_sweep_deep" ||
          a.scenario == "cxl_miss" || a.scenario == "dup_reject") {
        prefill_book(book, a.orders, a.levels, id0);
      }
      if (a.scenario == "dup_reject") {
        // Make order-id 7 already resting so the timed duplicate insert is rejected.
        (void)book.add_limit_order(7, matching::Side::Buy, 900, 10, id0 + 1);
      }

      // --- Timed operation ---
      auto t0 = Clock::now();
      matching::AddResult r{};
      matching::ErrorCode c = matching::ErrorCode::Success;
      if (a.scenario == "lmt_rest") {
        // Limit order at a price that does not cross; should rest on the book.
        r = book.add_limit_order(id0 + 2, matching::Side::Buy, 900, 10, id0 + 2);
        if (r.code == matching::ErrorCode::Success) ok++;
      } else if (a.scenario == "lmt_cross_deep") {
        // Aggressive buy limit order that crosses the entire pre-filled ask book.
        r = book.add_limit_order(id0 + 3, matching::Side::Buy, 5000,
                                 static_cast<std::uint32_t>(a.levels), id0 + 3);
        if (r.code == matching::ErrorCode::Success) ok++;
      } else if (a.scenario == "mkt_sweep_deep") {
        // Market buy that sweeps the pre-filled ask book and may leave remainder.
        r = book.add_market_order(id0 + 4, matching::Side::Buy,
                                  static_cast<std::uint32_t>(a.levels * 2), id0 + 4);
        if (r.code == matching::ErrorCode::Success ||
            r.code == matching::ErrorCode::MarketRemainderCancelled) ok++;
      } else if (a.scenario == "cxl_miss") {
        // Cancel an order-id that does not exist; exercises fast UnknownOrderId path.
        c = book.cancel_order(9'999'999'999ULL - i);
        if (c == matching::ErrorCode::UnknownOrderId) ok++;
      } else if (a.scenario == "dup_reject") {
        // Insert an order with an id that already rests on the book (id 7).
        r = book.add_limit_order(7, matching::Side::Sell, 2000, 10, id0 + 5);
        if (r.code == matching::ErrorCode::DuplicateOrderId) ok++;
      } else {
        std::cerr << "unknown scenario: " << a.scenario << "\n";
        return 2;
      }
      auto t1 = Clock::now();
      lat_ns.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    const auto wall_end = Clock::now();

    // --- Aggregate and report ---
    const auto wall_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count());
    const double sum_ns = std::accumulate(lat_ns.begin(), lat_ns.end(), 0.0);
    const double p50 = percentile_ns(lat_ns, 0.50);
    const double p95 = percentile_ns(lat_ns, 0.95);
    const double p99 = percentile_ns(lat_ns, 0.99);
    const double avg = sum_ns / static_cast<double>(a.iters);
    const double ops_s = (1e9 * static_cast<double>(a.iters)) / wall_ns;

    std::cout << "scenario=" << a.scenario
              << " iters=" << a.iters
              << " orders=" << a.orders
              << " levels=" << a.levels
              << " ok=" << ok
              << " avg_ns=" << avg
              << " p50_ns=" << p50
              << " p95_ns=" << p95
              << " p99_ns=" << p99
              << " ops_s=" << ops_s << "\n";

    if (!a.out_csv.empty()) {
      std::ofstream f(a.out_csv, std::ios::app);
      f << a.scenario << ","
        << a.orders << ","
        << a.levels << ","
        << a.iters << ","
        << avg << ","
        << p50 << ","
        << p95 << ","
        << p99 << ","
        << ops_s << ","
        << ok << "\n";
    }
    return 0;
  }
