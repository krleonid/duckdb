#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/checkpoint/write_overflow_strings_to_disk.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/checkpoint/table_data_writer.hpp"

namespace duckdb {

ColumnCheckpointState::ColumnCheckpointState(const RowGroup &row_group, ColumnData &original_column,
                                             PartialBlockManager &partial_block_manager)
    : row_group(row_group), original_column(original_column), partial_block_manager(partial_block_manager),
      original_column_mutable(original_column) {
}

ColumnCheckpointState::~ColumnCheckpointState() {
}

unique_ptr<BaseStatistics> ColumnCheckpointState::GetStatistics() {
	D_ASSERT(global_stats);
	return std::move(global_stats);
}

shared_ptr<ColumnData> ColumnCheckpointState::CreateEmptyColumnData() {
	throw InternalException("CreateEmptyColumnData not implemented for this column checkpoint state");
}

ColumnData &ColumnCheckpointState::GetResultColumn() {
	if (!result_column) {
		result_column = CreateEmptyColumnData();
	}
	return *result_column;
}

shared_ptr<ColumnData> ColumnCheckpointState::GetFinalResult() {
	if (!result_column) {
		// no result column instantiated - that means we haven't changed anything and can directly return the
		// original column
		return original_column_mutable.shared_from_this();
	}
	result_column->SetCount(original_column.count.load());
	return result_column;
}

PartialBlockForCheckpoint::PartialBlockForCheckpoint(ColumnData &data, ColumnSegment &segment, PartialBlockState state,
                                                     BlockManager &block_manager)
    : PartialBlock(state, block_manager, segment.block) {
	PartialBlockForCheckpoint::AddSegmentToTail(data, segment, 0);
}

PartialBlockForCheckpoint::~PartialBlockForCheckpoint() {
}

bool PartialBlockForCheckpoint::IsFlushed() {
	// segments are cleared on Flush
	return segments.empty();
}

void PartialBlockForCheckpoint::Flush(QueryContext context, const idx_t free_space_left) {
	if (IsFlushed()) {
		throw InternalException("Flush called on partial block that was already flushed");
	}

	// zero-initialize unused memory
	FlushInternal(free_space_left);

	// At this point, we've already copied all data from tail_segments
	// into the page owned by first_segment. We flush all segment data to
	// disk with the following call.
	// persist the first segment to disk and point the remaining segments to the same block
	bool fetch_new_block = state.block_id == INVALID_BLOCK;
	if (fetch_new_block) {
		state.block_id = block_manager.GetFreeBlockId();
	}

	for (idx_t i = 0; i < segments.size(); i++) {
		auto &segment = segments[i];
		if (i == 0) {
			// the first segment is converted to persistent - this writes the data for ALL segments to disk
			D_ASSERT(segment.offset_in_block == 0);
			segment.segment.ConvertToPersistent(context, &block_manager, state.block_id);
			// update the block after it has been converted to a persistent segment
			block_handle = segment.segment.block;
		} else {
			// subsequent segments are MARKED as persistent - they don't need to be rewritten
			segment.segment.MarkAsPersistent(block_handle, segment.offset_in_block);
			if (fetch_new_block) {
				// if we fetched a new block we need to increase the reference count to the block
				block_manager.IncreaseBlockReferenceCount(state.block_id);
			}
		}
	}
	Clear();
}

void PartialBlockForCheckpoint::Merge(PartialBlock &other_p, idx_t offset, idx_t other_size) {
	auto &other = other_p.Cast<PartialBlockForCheckpoint>();

	auto &buffer_manager = block_manager.buffer_manager;
	// pin the source block
	auto old_handle = buffer_manager.Pin(other.block_handle);
	// pin the target block
	auto new_handle = buffer_manager.Pin(block_handle);
	// memcpy the contents of the old block to the new block
	memcpy(new_handle.GetDataMutable() + offset, old_handle.Ptr(), other_size);

	// now copy over all segments to the new block
	// move over the uninitialized regions
	for (auto &region : other.uninitialized_regions) {
		region.start += offset;
		region.end += offset;
		uninitialized_regions.push_back(region);
	}

	// move over the segments
	for (auto &segment : other.segments) {
		AddSegmentToTail(segment.data, segment.segment, NumericCast<uint32_t>(segment.offset_in_block + offset));
	}

	other.Clear();
}

void PartialBlockForCheckpoint::AddSegmentToTail(ColumnData &data, ColumnSegment &segment, uint32_t offset_in_block) {
	segments.emplace_back(data, segment, offset_in_block);
}

void PartialBlockForCheckpoint::Clear() {
	uninitialized_regions.clear();
	block_handle.reset();
	segments.clear();
}

void ColumnCheckpointState::FlushSegment(unique_ptr<ColumnSegment> segment, BufferHandle handle, idx_t segment_size) {
	handle.Destroy();
	FlushSegmentInternal(std::move(segment), segment_size);
}

void ColumnCheckpointState::FlushSegmentInternal(unique_ptr<ColumnSegment> segment, idx_t segment_size) {
	auto block_size = partial_block_manager.GetBlockManager().GetBlockSize();
	if (segment_size > block_size) {
		throw InternalException("segment size exceeds block size in ColumnCheckpointState::FlushSegmentInternal");
	}

	auto tuple_count = segment->count.load();
	if (tuple_count == 0) { // LCOV_EXCL_START
		return;
	} // LCOV_EXCL_STOP

	// Merge the segment statistics into the global statistics.
	global_stats->Merge(segment->GetStats());

	block_id_t block_id = INVALID_BLOCK;
	uint32_t offset_in_block = 0;

	unique_lock<mutex> partial_block_lock;
	if (segment->GetStats().IsConstant()) {
		// Constant block.
		segment->ConvertToPersistent(partial_block_manager.GetClientContext(), nullptr, INVALID_BLOCK);
	} else if (segment_size != 0) {
		// Non-constant block with data that has to go to disk.
		auto &db = original_column.GetDatabase();
		auto &buffer_manager = BufferManager::GetBufferManager(db);
		partial_block_lock = partial_block_manager.GetLock();

		auto cast_segment_size = NumericCast<uint32_t>(segment_size);
		auto allocation = partial_block_manager.GetBlockAllocation(cast_segment_size);
		block_id = allocation.state.block_id;
		offset_in_block = allocation.state.offset;

		if (allocation.partial_block) {
			// Use an existing block.
			D_ASSERT(offset_in_block > 0);
			auto &pstate = *allocation.partial_block;
			// pin the source block
			auto old_handle = buffer_manager.Pin(segment->block);
			// pin the target block
			auto new_handle = buffer_manager.Pin(pstate.block_handle);
			// memcpy the contents of the old block to the new block
			memcpy(new_handle.GetDataMutable() + offset_in_block, old_handle.Ptr(), segment_size);
			pstate.AddSegmentToTail(*result_column, *segment, offset_in_block);
		} else {
			// Create a new block for future reuse.
			if (segment->SegmentSize() != block_size) {
				// the segment is smaller than the block size
				// allocate a new block and copy the data over
				D_ASSERT(segment->SegmentSize() < block_size);
				segment->Resize(block_size);
			}
			D_ASSERT(offset_in_block == 0);
			allocation.partial_block = partial_block_manager.CreatePartialBlock(
			    *result_column, *segment, allocation.state, *allocation.block_manager);
		}
		// Writer will decide whether to reuse this block.
		partial_block_manager.RegisterPartialBlock(std::move(allocation));

	} else {
		// Empty segment, which does not have to go to disk.
		// We still need to change its type to persistent, because we need to write its metadata.
		segment->segment_type = ColumnSegmentType::PERSISTENT;
		segment->block.reset();
	}

	// construct the data pointer
	DataPointer data_pointer(segment->GetStats().Copy());
	data_pointer.block_pointer.block_id = block_id;
	data_pointer.block_pointer.offset = offset_in_block;
	data_pointer.row_start = 0;
	if (!data_pointers.empty()) {
		auto &last_pointer = data_pointers.back();
		data_pointer.row_start = last_pointer.row_start + last_pointer.tuple_count;
	}
	data_pointer.tuple_count = tuple_count;
	auto &compression_function = segment->GetCompressionFunction();
	data_pointer.compression_type = compression_function.type;
	if (compression_function.serialize_state) {
		data_pointer.segment_state = compression_function.serialize_state(*segment);
	}

	// append the segment to the new segment tree
	GetResultColumn().GetSegmentTree().AppendSegment(std::move(segment));
	data_pointers.push_back(std::move(data_pointer));
}

void ColumnCheckpointState::FlushTransientSegmentsInPlace() {
	// NOTE: global_stats->Merge() is intentionally NOT called here.
	// Stats merging is deferred to WritePersistentSegments(), which is invoked by
	// FinalizeCheckpoint() after FlushTransientTail() returns with has_changes=false.
	//
	// Reset column-level stats to empty: WritePersistentSegments will accumulate fresh
	// stats from segments into global_stats, and RowGroup::WriteToDisk will MergeStatistics
	// into this column. Without the reset, that would be a self-merge (the column's stats
	// already reflect all segments). Self-merge is idempotent for min/max but not guaranteed
	// for all stat types (sketches, distinct counts).
	if (original_column_mutable.stats) {
		lock_guard<mutex> l(original_column_mutable.stats_lock);
		original_column_mutable.stats->statistics = BaseStatistics::CreateEmpty(original_column.type);
	}

	auto &block_manager = partial_block_manager.GetBlockManager();
	auto block_size = block_manager.GetBlockSize();
	auto context = partial_block_manager.GetClientContext();
	auto &db = original_column.GetDatabase();
	auto &buffer_manager = BufferManager::GetBufferManager(db);

	// Pack multiple small transient segments into shared blocks to avoid burning a
	// full block per segment. Each entry records the segment reference and its
	// offset within the shared block. The lead segment (index 0) owns the block
	// buffer; follower data is memcpy'd in before ConvertToPersistent is called.
	struct PackEntry {
		ColumnSegment &segment;
		uint32_t offset_in_block;
		uint32_t actual_size;
	};
	vector<PackEntry> pack;
	idx_t pack_used = 0;

	auto flush_pack = [&]() {
		if (pack.empty()) {
			return;
		}
		auto &lead = pack[0].segment;
		if (lead.SegmentSize() != block_size) {
			D_ASSERT(lead.SegmentSize() < block_size);
			lead.Resize(block_size);
		}
		// memcpy each follower's compacted data into the lead's buffer
		if (pack.size() > 1) {
			auto lead_pin = buffer_manager.Pin(lead.block);
			for (idx_t i = 1; i < pack.size(); i++) {
				auto &entry = pack[i];
				auto src_pin = buffer_manager.Pin(entry.segment.block);
				memcpy(lead_pin.GetDataMutable() + entry.offset_in_block, src_pin.Ptr(), entry.actual_size);
			}
		}
		auto block_id = block_manager.GetFreeBlockId();
		lead.ConvertToPersistent(context, &block_manager, block_id);
		for (idx_t i = 1; i < pack.size(); i++) {
			pack[i].segment.MarkAsPersistent(lead.block, pack[i].offset_in_block);
			block_manager.IncreaseBlockReferenceCount(block_id);
		}
		pack.clear();
		pack_used = 0;
	};

	// No data.Lock() is needed here: this path is only taken during FULL_CHECKPOINT,
	// which guarantees exclusive access — no concurrent writers can modify the segment list.
	bool seen_transient = false;
	for (auto &segment_node : original_column_mutable.data.SegmentNodes()) {
		auto &segment = segment_node.GetNode();
		if (segment.segment_type != ColumnSegmentType::TRANSIENT) {
			D_ASSERT(!seen_transient);
			flush_pack();
			continue;
		}
		seen_transient = true;
		if (segment.count.load() == 0) {
			continue;
		}
		if (segment.GetStats().IsConstant()) {
			flush_pack();
			segment.ConvertToPersistent(context, nullptr, INVALID_BLOCK);
			continue;
		}

		// 3.3: call finalize_append to get actual used bytes (also compacts string dicts)
		auto &func = segment.GetCompressionFunction();
		idx_t actual_size =
		    func.finalize_append ? func.finalize_append(segment, segment.GetStatsMutable()) : segment.SegmentSize();
		D_ASSERT(actual_size <= block_size);

		if (actual_size >= block_size) {
			// Full block: no packing benefit, write directly
			flush_pack();
			auto block_id = block_manager.GetFreeBlockId();
			segment.ConvertToPersistent(context, &block_manager, block_id);
			continue;
		}

		// 3.2: accumulate small segments; flush when the current block is full
		if (pack_used + actual_size > block_size) {
			flush_pack();
		}
		pack.push_back({segment, NumericCast<uint32_t>(pack_used), NumericCast<uint32_t>(actual_size)});
		pack_used += actual_size;
	}

	flush_pack();
}

PersistentColumnData ColumnCheckpointState::ToPersistentData() {
	auto &type = result_column ? result_column->type : original_column.type;
	PersistentColumnData data(type);
	data.pointers = std::move(data_pointers);
	return data;
}

} // namespace duckdb
