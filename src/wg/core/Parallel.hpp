#pragma once

#include <functional>

namespace wg {

// Runs fn(begin, end) over [0, count) split into contiguous chunks, on a pool
// of persistent worker threads (one per hardware thread, created on first
// use). The chunks must be independent. Ranges of fewer than 2 * minChunk
// items run inline. Calls from inside a chunk run inline too (no nesting).
void parallelFor(int count, const std::function<void(int, int)>& fn, int minChunk = 16);

} // namespace wg
