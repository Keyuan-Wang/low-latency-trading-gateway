/**
 * @file bench_overall.cpp
 * @brief Overall throughput benchmark: mixed operations on a single book.
 *
 * Prefills both sides of the book, then runs a fixed mix of operation types
 * (35% cancel, 30% modify, 25% limit rest, 5% limit cross, 5% market) in a single long
 * run. Uses iters=1 + a large batch_size so the book evolves realistically
 * throughout the measurement window.  Reports ops_s as total_ops / total_seconds
 * — a single global throughput metric for comparing versions.
 *
 * Randomness is fully deterministic via SplitMix64 seeded from Args::seed +
 * iter_idx, so runs are reproducible across versions and machines.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class OverallScenario : public benchmark_runner::IBenchScenario {
public:
	const char* Name() const override { return "overall"; }
	[[nodiscard]] std::uint64_t max_batch_size() const override {
		return 1'000'000;
	}

		void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
			// Pool must hold: prefilled + new ops during run
			const std::uint64_t pool_size =
				2 * args.orders + 2 * args.levels + args.batch_size + 5000;
			rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
			op_rng_ = benchmark_runner::SplitMix64(args.seed * 1337ULL + iter_idx * 331ULL);
			live_.clear();
			planning_live_.clear();
			live_ids_.clear();
			pending_.clear();
			pending_.reserve(args.batch_size);

			book_ = build_prefilled_book(args, pool_size, live_);
			planning_book_ = build_prefilled_book(args, pool_size, planning_live_);
			rebuild_live_ids();

			id_counter_ = 1'000'000ULL;
			for (std::uint64_t i = 0; i < args.batch_size; ++i) {
				generate_pending_op();
			}
			planning_book_.reset();
		}

		bool RunOp(const benchmark_runner::Args& args, std::uint64_t,
				   std::uint64_t batch_idx, std::uint64_t& ok) override {
			const auto& op = pending_[batch_idx];
			switch (op.type) {
			case PendingOp::kCancel: {
				const auto code = book_->cancel_order(op.handle);
				if (code == matching::ErrorCode::Success) ++ok;
				break;
			}
			case PendingOp::kModify: {
				const auto res =
					book_->modify_order(op.handle, matching::Side::Buy, op.price, op.qty, op.oid);
				if (res.code == matching::ErrorCode::Success) ++ok;
				break;
			}
			case PendingOp::kLimit:
			case PendingOp::kCross: {
				const auto res =
					book_->add_limit_order(op.oid, matching::Side::Buy, op.price, op.qty, op.oid);
				if (res.code == matching::ErrorCode::Success ||
					res.code == matching::ErrorCode::MarketRemainderCancelled) ++ok;
				break;
			}
			case PendingOp::kMarket: {
				const auto res =
					book_->add_market_order(op.oid, matching::Side::Buy, op.qty, op.oid);
				if (res.code == matching::ErrorCode::Success ||
					res.code == matching::ErrorCode::MarketRemainderCancelled) ++ok;
				break;
			}
			}

			return true;
		}

		void Teardown() override {
			book_.reset();
			planning_book_.reset();
		}

	private:
		struct PendingOp {
			enum Type : std::uint8_t { kCancel, kModify, kLimit, kCross, kMarket };
			Type type = kLimit;
			matching::OrderHandle handle = matching::kInvalidHandle;
			std::uint64_t oid = 0;
			std::int64_t price = 0;
			std::uint64_t qty = 0;
		};

		struct LiveOrder {
			matching::OrderHandle handle = matching::kInvalidHandle;
			std::uint64_t quantity = 0;
		};

		using LiveMap = std::unordered_map<std::uint64_t, LiveOrder>;

		std::unique_ptr<matching::OrderBook>
		build_prefilled_book(const benchmark_runner::Args& args,
		                     std::uint64_t pool_size,
		                     LiveMap& live) {
			auto book = std::make_unique<matching::OrderBook>(pool_size);
			live.clear();
			const std::uint64_t per_level =
				std::max<std::uint64_t>(1, args.orders / std::max<std::uint64_t>(1, args.levels));

			std::uint64_t id = 100'000ULL;
			for (std::uint64_t lvl = 0; lvl < args.levels; ++lvl) {
				const std::int64_t ask_price = 1000 + static_cast<std::int64_t>(lvl);
				for (std::uint64_t j = 0; j < per_level; ++j) {
					const auto res =
						book->add_limit_order(id, matching::Side::Sell, ask_price, 1, id);
					track_add(live, id, res);
					++id;
				}
			}

			for (std::uint64_t lvl = 0; lvl < args.levels; ++lvl) {
				const std::int64_t bid_price = 999 - static_cast<std::int64_t>(lvl);
				for (std::uint64_t j = 0; j < per_level; ++j) {
					const auto res =
						book->add_limit_order(id, matching::Side::Buy, bid_price, 1, id);
					track_add(live, id, res);
					++id;
				}
			}
			return book;
		}

		void track_add(LiveMap& live, std::uint64_t id, const matching::AddResult& res) {
			if (res.code == matching::ErrorCode::Success &&
				res.remaining_quantity > 0 &&
				res.handle != matching::kInvalidHandle) {
				live[id] = LiveOrder{res.handle, res.remaining_quantity};
			}
		}

		void track_result(LiveMap& live, std::uint64_t id, const matching::AddResult& res) {
			for (const auto& trade : res.trades) {
				auto it = live.find(trade.maker_order_id);
				if (it == live.end()) continue;
				if (trade.quantity >= it->second.quantity) {
					live.erase(it);
				} else {
					it->second.quantity -= trade.quantity;
				}
			}
			track_add(live, id, res);
		}

		void generate_pending_op() {
			const std::uint64_t roll = op_rng_.next() % 100;
			if (roll < 35 && generate_cancel()) return;
			if (roll < 65 && generate_modify()) return;
			if (roll < 90) { generate_resting_limit(); return; }
			if (roll < 95) { generate_crossing_limit(); return; }
			generate_market();
		}

		bool generate_cancel() {
			const std::uint64_t id = select_live_id();
			if (id == 0) return false;
			auto live_it = live_.find(id);
			auto planning_it = planning_live_.find(id);
			if (live_it == live_.end() || planning_it == planning_live_.end()) return false;

			PendingOp op;
			op.type = PendingOp::kCancel;
			op.handle = live_it->second.handle;
			const auto code = planning_book_->cancel_order(planning_it->second.handle);
			if (code != matching::ErrorCode::Success) return false;
			live_.erase(live_it);
			planning_live_.erase(planning_it);
			pending_.push_back(op);
			return true;
		}

		bool generate_modify() {
			const std::uint64_t id = select_live_id();
			if (id == 0) return false;
			auto live_it = live_.find(id);
			auto planning_it = planning_live_.find(id);
			if (live_it == live_.end() || planning_it == planning_live_.end()) return false;

			PendingOp op;
			op.type = PendingOp::kModify;
			op.handle = live_it->second.handle;
			op.price = 1 + static_cast<std::int64_t>(rng_.next() % 998);
			op.qty = 1;
			op.oid = id_counter_++;
			const auto res =
				planning_book_->modify_order(planning_it->second.handle,
				                             matching::Side::Buy, op.price, op.qty, op.oid);
			live_.erase(live_it);
			planning_live_.erase(planning_it);
			track_result(live_, id, res);
			track_result(planning_live_, id, res);
			if (res.code != matching::ErrorCode::Success) return false;
			pending_.push_back(op);
			return true;
		}

		void generate_resting_limit() {
			PendingOp op;
			op.type = PendingOp::kLimit;
			op.oid = id_counter_++;
			op.price = 1 + static_cast<std::int64_t>(rng_.next() % 998);
			op.qty = 1;
			const auto res =
				planning_book_->add_limit_order(op.oid, matching::Side::Buy,
				                                op.price, op.qty, op.oid);
			track_result(live_, op.oid, res);
			track_result(planning_live_, op.oid, res);
			pending_.push_back(op);
		}

		void generate_crossing_limit() {
			PendingOp op;
			op.type = PendingOp::kCross;
			op.oid = id_counter_++;
			op.price = 1000 + static_cast<std::int64_t>(rng_.next() % 10);
			op.qty = 5;
			const auto res =
				planning_book_->add_limit_order(op.oid, matching::Side::Buy,
				                                op.price, op.qty, op.oid);
			track_result(live_, op.oid, res);
			track_result(planning_live_, op.oid, res);
			pending_.push_back(op);
		}

		void generate_market() {
			PendingOp op;
			op.type = PendingOp::kMarket;
			op.oid = id_counter_++;
			op.qty = 10;
			const auto res =
				planning_book_->add_market_order(op.oid, matching::Side::Buy,
				                                 op.qty, op.oid);
			track_result(live_, op.oid, res);
			track_result(planning_live_, op.oid, res);
			pending_.push_back(op);
		}

		void rebuild_live_ids() {
			live_ids_.clear();
			live_ids_.reserve(live_.size());
			for (const auto& [id, _] : live_) live_ids_.push_back(id);
		}

		std::uint64_t select_live_id() {
			if (live_.empty()) return 0;
			if (live_ids_.empty() || live_ids_.size() > live_.size() * 8) rebuild_live_ids();
			for (int attempt = 0; attempt < 16 && !live_ids_.empty(); ++attempt) {
				const auto id = live_ids_[rng_.next() % live_ids_.size()];
				if (live_.contains(id)) return id;
			}
			rebuild_live_ids();
			if (live_ids_.empty()) return 0;
			return live_ids_[rng_.next() % live_ids_.size()];
		}

		std::unique_ptr<matching::OrderBook> book_;
		std::unique_ptr<matching::OrderBook> planning_book_;
		benchmark_runner::SplitMix64 rng_{42};
		benchmark_runner::SplitMix64 op_rng_{42};
		std::uint64_t id_counter_ = 0;
		LiveMap live_;
		LiveMap planning_live_;
		std::vector<std::uint64_t> live_ids_;
		std::vector<PendingOp> pending_;
};

}  // namespace

int main(int argc, char** argv) {
	OverallScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
