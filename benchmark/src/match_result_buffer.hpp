#pragma once

#include "matching_core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace benchmark_runner {

struct MatchResultRecord {
	std::uint64_t op_index = 0;
	llmes::matching_core::AddResult result{};
	std::size_t trade_begin = 0;
	std::size_t trade_count = 0;
};

class MatchResultBuffer {
public:
	MatchResultBuffer() = default;

	MatchResultBuffer(std::size_t result_capacity, std::size_t trade_capacity) {
		reserve(result_capacity, trade_capacity);
	}

	void reserve(std::size_t result_capacity, std::size_t trade_capacity) {
		records_.reserve(result_capacity);
		trades_.reserve(trade_capacity);
	}

	void reset(std::size_t result_capacity, std::size_t trade_capacity) {
		records_.clear();
		records_.resize(result_capacity);
		trades_.clear();
		trades_.reserve(trade_capacity);
		record_count_ = 0;
		next_op_index_ = 0;
	}

	void clear() noexcept {
		records_.clear();
		trades_.clear();
		record_count_ = 0;
		next_op_index_ = 0;
	}

	[[gnu::always_inline]] inline std::size_t
	append_result(const llmes::matching_core::AddResult& result) {
		const std::size_t record_index = record_count_++;
		if (record_index == records_.size()) {
			records_.push_back({});
		}
		records_[record_index] = MatchResultRecord{
				next_op_index_++,
				result,
				trades_.size(),
				0,
		};
		return record_index;
	}

	[[gnu::always_inline]] inline void
	set_result(std::size_t record_index,
						 const llmes::matching_core::AddResult& result) noexcept {
		records_[record_index].result = result;
	}

	[[gnu::always_inline]] inline void
	attach_trades(std::size_t record_index, std::span<const llmes::matching_core::Trade> trades) {
		auto& record = records_[record_index];
		record.trade_begin = trades_.size();
		record.trade_count = trades.size();
		trades_.insert(trades_.end(), trades.begin(), trades.end());
	}

	[[gnu::always_inline]] inline void
	append_trade(std::size_t record_index,
							 const llmes::matching_core::Trade& trade) {
		auto& record = records_[record_index];
		if (record.trade_count == 0) {
			record.trade_begin = trades_.size();
		}
		trades_.push_back(trade);
		++record.trade_count;
	}

	[[nodiscard]] std::span<const MatchResultRecord> records() const noexcept {
		return std::span<const MatchResultRecord>(records_.data(), record_count_);
	}

	[[nodiscard]] std::span<const llmes::matching_core::Trade> trades() const noexcept {
		return trades_;
	}

	[[nodiscard]] std::span<const llmes::matching_core::Trade>
	trades_for(const MatchResultRecord& record) const noexcept {
		return std::span<const llmes::matching_core::Trade>(
				trades_.data() + record.trade_begin,
				record.trade_count);
	}

private:
	std::vector<MatchResultRecord> records_;
	std::vector<llmes::matching_core::Trade> trades_;
	std::size_t record_count_ = 0;
	std::uint64_t next_op_index_ = 0;
};

}  // namespace benchmark_runner
