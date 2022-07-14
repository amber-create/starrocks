// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "exec/pipeline/scan/chunk_buffer_limiter.h"

#include "glog/logging.h"

namespace starrocks::pipeline {

void DynamicChunkBufferLimiter::update_avg_row_bytes(size_t added_sum_row_bytes, size_t added_num_rows,
                                                     size_t max_chunk_rows) {
    std::lock_guard<std::mutex> lock(_mutex);

    _sum_row_bytes += added_sum_row_bytes;
    _num_rows += added_num_rows;
    size_t avg_row_bytes = 0;
    if (_num_rows > 0) {
        avg_row_bytes = _sum_row_bytes / _num_rows;
    }
    if (avg_row_bytes == 0) {
        return;
    }

    size_t chunk_mem_usage = avg_row_bytes * max_chunk_rows;
    size_t new_capacity = std::max<size_t>(_mem_limit / chunk_mem_usage, 1);
    _capacity = std::min(new_capacity, _max_capacity);
}

ChunkBufferTokenPtr DynamicChunkBufferLimiter::pin(int num_chunks) {
    size_t prev_value = _pinned_chunks_counter.fetch_add(num_chunks);
    if (prev_value + num_chunks > _capacity) {
        _unpin(num_chunks);
        return nullptr;
    }
    return std::make_unique<DynamicChunkBufferLimiter::Token>(_pinned_chunks_counter, num_chunks);
}

void DynamicChunkBufferLimiter::_unpin(int num_chunks) {
    int prev_value = _pinned_chunks_counter.fetch_sub(num_chunks);
    DCHECK_GE(prev_value, 1);
}

} // namespace starrocks::pipeline
