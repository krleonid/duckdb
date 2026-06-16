#include "duckdb/storage/block_manager.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"
#include "duckdb/logging/logger.hpp"
#include <chrono>

namespace duckdb {

BlockManager::BlockManager(BufferManager &buffer_manager, const optional_idx block_alloc_size_p,
                           const optional_idx block_header_size_p)
    : buffer_manager(buffer_manager), metadata_manager(make_uniq<MetadataManager>(*this, buffer_manager)),
      block_alloc_size(block_alloc_size_p), block_header_size(block_header_size_p) {
}

bool BlockManager::BlockIsRegistered(block_id_t block_id) {
	int64_t wait_ms = 0;
	int64_t hold_ms = 0;
	bool result = false;
	{
		auto start = std::chrono::steady_clock::now();
		lock_guard<mutex> lock(blocks_lock);
		auto acquired = std::chrono::steady_clock::now();
		wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(acquired - start).count();
		// check if the block already exists
		auto entry = blocks.find(block_id);
		if (entry == blocks.end()) {
			result = false;
		} else {
			// already exists: check if it hasn't expired yet
			result = !entry->second.expired();
		}
		auto released = std::chrono::steady_clock::now();
		hold_ms = std::chrono::duration_cast<std::chrono::milliseconds>(released - acquired).count();
	}
	// Log AFTER releasing the lock
	if (wait_ms > 100 || hold_ms > 10) {
		try {
			auto &db = buffer_manager.GetDatabase();
			DUCKDB_LOG_WARNING(db, "blocks_lock in BlockIsRegistered: wait=" + to_string(wait_ms) +
			                           "ms hold=" + to_string(hold_ms) + "ms");
		} catch (...) {
			// Silently ignore if database is not available
		}
	}
	return result;
}

shared_ptr<BlockHandle> BlockManager::TryGetBlock(block_id_t block_id) {
	int64_t wait_ms = 0;
	int64_t hold_ms = 0;
	shared_ptr<BlockHandle> result = nullptr;
	{
		auto start = std::chrono::steady_clock::now();
		lock_guard<mutex> lock(blocks_lock);
		auto acquired = std::chrono::steady_clock::now();
		wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(acquired - start).count();
		// check if the block already exists
		auto entry = blocks.find(block_id);
		if (entry == blocks.end()) {
			// the block does not exist
			result = nullptr;
		} else {
			// the block exists - try to lock it
			result = entry->second.lock();
		}
		auto released = std::chrono::steady_clock::now();
		hold_ms = std::chrono::duration_cast<std::chrono::milliseconds>(released - acquired).count();
	}
	// Log AFTER releasing the lock
	if (wait_ms > 100 || hold_ms > 10) {
		try {
			auto &db = buffer_manager.GetDatabase();
			DUCKDB_LOG_WARNING(db, "blocks_lock in TryGetBlock: wait=" + to_string(wait_ms) +
			                           "ms hold=" + to_string(hold_ms) + "ms");
		} catch (...) {
			// Silently ignore if database is not available
		}
	}
	return result;
}

shared_ptr<BlockHandle> BlockManager::RegisterBlock(block_id_t block_id) {
	int64_t wait_ms = 0;
	int64_t hold_ms = 0;
	shared_ptr<BlockHandle> result = nullptr;
	{
		auto start = std::chrono::steady_clock::now();
		lock_guard<mutex> lock(blocks_lock);
		auto acquired = std::chrono::steady_clock::now();
		wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(acquired - start).count();
		// check if the block already exists
		auto entry = blocks.find(block_id);
		if (entry != blocks.end()) {
			// already exists: check if it hasn't expired yet
			auto existing_ptr = entry->second.lock();
			if (existing_ptr) {
				//! it hasn't! return it
				result = existing_ptr;
			}
		}
		if (!result) {
			// create a new block pointer for this block
			result = make_shared_ptr<BlockHandle>(*this, block_id, MemoryTag::BASE_TABLE);
			// register the block pointer in the set of blocks as a weak pointer
			blocks[block_id] = weak_ptr<BlockHandle>(result);
		}
		auto released = std::chrono::steady_clock::now();
		hold_ms = std::chrono::duration_cast<std::chrono::milliseconds>(released - acquired).count();
	}
	// Log AFTER releasing the lock
	if (wait_ms > 100 || hold_ms > 10) {
		try {
			auto &db = buffer_manager.GetDatabase();
			DUCKDB_LOG_WARNING(db, "blocks_lock in RegisterBlock: wait=" + to_string(wait_ms) +
			                           "ms hold=" + to_string(hold_ms) + "ms");
		} catch (...) {
			// Silently ignore if database is not available
		}
	}
	return result;
}

shared_ptr<BlockHandle> BlockManager::ConvertToPersistent(QueryContext context, block_id_t block_id,
                                                          shared_ptr<BlockHandle> old_block, BufferHandle old_handle,
                                                          ConvertToPersistentMode mode) {
	// register a block with the new block id
	auto new_block = RegisterBlock(block_id);
	D_ASSERT(new_block->GetMemory().GetState() == BlockState::BLOCK_UNLOADED);
	D_ASSERT(new_block->GetMemory().GetReaders() == 0);

	if (mode == ConvertToPersistentMode::THREAD_SAFE) {
		// safe mode - create a copy of the old block and operate on that
		// this ensures we don't modify the old block - which allows other concurrent operations on the old block to
		// continue
		auto old_block_copy = buffer_manager.AllocateMemory(old_block->GetMemory().GetMemoryTag(), this, false);
		auto copy_pin = buffer_manager.Pin(old_block_copy);
		memcpy(copy_pin.Ptr(), old_handle.Ptr(), GetBlockSize());
		old_block = std::move(old_block_copy);
		old_handle = std::move(copy_pin);
	}

	auto &old_block_memory = old_block->GetMemory();
	auto lock = old_block_memory.GetLock();
	D_ASSERT(old_block_memory.GetState() == BlockState::BLOCK_LOADED);
	D_ASSERT(old_block_memory.GetBuffer(lock));
	if (old_block_memory.GetReaders() > 1) {
		throw InternalException(
		    "BlockManager::ConvertToPersistent in destructive mode - cannot be called for block %d as old_block has "
		    "multiple readers active",
		    block_id);
	}

	// Temp buffers can be larger than the storage block size.
	// But persistent buffers cannot.
	D_ASSERT(old_block_memory.GetBuffer(lock)->AllocSize() <= GetBlockAllocSize());

	// convert the buffer to a block
	auto converted_buffer = ConvertBlock(block_id, *old_block_memory.GetBuffer(lock));

	// persist the new block to disk
	Write(context, *converted_buffer, block_id);

	// now convert the actual block
	old_block_memory.ConvertToPersistent(lock, *new_block, std::move(converted_buffer));

	// destroy the old buffer
	lock.unlock();
	old_handle.Destroy();
	old_block.reset();

	// potentially purge the queue
	auto purge_queue = buffer_manager.GetBufferPool().AddToEvictionQueue(new_block);
	if (purge_queue) {
		buffer_manager.GetBufferPool().PurgeQueue(*new_block);
	}
	return new_block;
}

shared_ptr<BlockHandle> BlockManager::ConvertToPersistent(QueryContext context, block_id_t block_id,
                                                          shared_ptr<BlockHandle> old_block,
                                                          ConvertToPersistentMode mode) {
	// pin the old block to ensure we have it loaded in memory
	auto handle = buffer_manager.Pin(old_block);
	return ConvertToPersistent(context, block_id, std::move(old_block), std::move(handle), mode);
}

void BlockManager::UnregisterBlock(block_id_t id) {
	D_ASSERT(id < MAXIMUM_BLOCK);
	int64_t wait_ms = 0;
	int64_t hold_ms = 0;
	{
		auto start = std::chrono::steady_clock::now();
		lock_guard<mutex> lock(blocks_lock);
		auto acquired = std::chrono::steady_clock::now();
		wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(acquired - start).count();
		// on-disk block: erase from list of blocks in manager
		blocks.erase(id);
		auto released = std::chrono::steady_clock::now();
		hold_ms = std::chrono::duration_cast<std::chrono::milliseconds>(released - acquired).count();
	}
	// Log AFTER releasing the lock
	if (wait_ms > 100 || hold_ms > 10) {
		try {
			auto &db = buffer_manager.GetDatabase();
			DUCKDB_LOG_WARNING(db, "blocks_lock in UnregisterBlock: wait=" + to_string(wait_ms) +
			                           "ms hold=" + to_string(hold_ms) + "ms");
		} catch (...) {
			// Silently ignore if database is not available
		}
	}
}

void BlockManager::UnregisterPersistentBlock(BlockHandle &block) {
	if (in_destruction) {
		return;
	}
	auto id = block.BlockId();
	D_ASSERT(id < MAXIMUM_BLOCK);
	UnregisterBlock(id);
}

MetadataManager &BlockManager::GetMetadataManager() {
	return *metadata_manager;
}

void BlockManager::Write(QueryContext context, FileBuffer &block, block_id_t block_id) {
	// Fallback to the old Write.
	Write(block, block_id);
}

void BlockManager::Truncate() {
}

} // namespace duckdb
