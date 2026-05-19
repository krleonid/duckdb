#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/appender.hpp"
#include "test_helpers.hpp"
#include <chrono>

using namespace duckdb;

static idx_t GetDbFileSize(const string &path) {
	auto fs = FileSystem::CreateLocal();
	if (!fs->FileExists(path)) {
		return 0;
	}
	auto fh = fs->OpenFile(path, FileFlags::FILE_FLAGS_READ);
	return NumericCast<idx_t>(fs->GetFileSize(*fh));
}

// Returns bytes written to the DB file by a single CHECKPOINT call.
static idx_t MeasureCheckpointWrite(Connection &con, const string &db_path) {
	auto size_before = GetDbFileSize(db_path);
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	return GetDbFileSize(db_path) - size_before;
}

TEST_CASE("Incremental checkpoint writes only the new tail, not the full column data", "[api][.]") {
	auto db_path = TestDirectoryPath() + "/incremental_checkpoint_bytes.db";
	auto ctrl_path = TestDirectoryPath() + "/incremental_checkpoint_ctrl.db";
	DeleteDatabase(db_path);
	DeleteDatabase(ctrl_path);

	// ---- Phase 1: build a table large enough that a full rewrite is measurably expensive ----
	idx_t full_rewrite_bytes = 0;
	{
		DuckDB db(db_path);
		Connection con(db);

		REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
		REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));

		// Three columns, 100K rows — spans multiple row groups and segments per column.
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (id INTEGER, label VARCHAR, score DOUBLE)"));

		{
			Appender appender(con, "t");
			for (int32_t i = 0; i < 100000; i++) {
				appender.BeginRow();
				appender.Append<int32_t>(i);
				appender.Append<const char *>(("label_" + std::to_string(i % 1000)).c_str());
				appender.Append<double>(i * 0.5);
				appender.EndRow();
			}
		}

		// First checkpoint: forces a full write of all transient segments.
		full_rewrite_bytes = MeasureCheckpointWrite(con, db_path);
		REQUIRE(full_rewrite_bytes > 0);

		// Verify row count.
		auto res = con.Query("SELECT COUNT(*) FROM t");
		REQUIRE(CHECK_COLUMN(res, 0, {100000}));
	}

	// ---- Phase 2: reopen and append a tiny batch, then measure incremental write ----
	idx_t incremental_bytes = 0;
	idx_t block_size = 0;
	{
		DuckDB db(db_path);
		Connection con(db);

		REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
		REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));

		// Query block_size so we can assert an absolute upper bound.
		auto block_info = con.Query("SELECT block_size FROM pragma_database_size()");
		block_size = block_info->GetValue(0, 0).GetValue<idx_t>();

		// Append only 50 rows — creates small transient tail segments per column.
		{
			Appender appender(con, "t");
			for (int32_t i = 100000; i < 100050; i++) {
				appender.BeginRow();
				appender.Append<int32_t>(i);
				appender.Append<const char *>(("tail_" + std::to_string(i)).c_str());
				appender.Append<double>(i * 0.5);
				appender.EndRow();
			}
		}

		// Incremental checkpoint: should only flush the tail, not all 100K rows.
		incremental_bytes = MeasureCheckpointWrite(con, db_path);

		// Correctness: all rows survive.
		auto res = con.Query("SELECT COUNT(*) FROM t");
		REQUIRE(CHECK_COLUMN(res, 0, {100050}));

		auto min_res = con.Query("SELECT MIN(id), MAX(id) FROM t");
		REQUIRE(CHECK_COLUMN(min_res, 0, {0}));
		REQUIRE(CHECK_COLUMN(min_res, 1, {100049}));
	}

	// ---- Phase 3: reopen again and verify persisted data is complete and correct ----
	{
		DuckDB db(db_path);
		Connection con(db);

		auto res = con.Query("SELECT COUNT(*) FROM t");
		REQUIRE(CHECK_COLUMN(res, 0, {100050}));

		// Spot-check an old row.
		auto old_row = con.Query("SELECT label, score FROM t WHERE id = 42");
		REQUIRE(CHECK_COLUMN(old_row, 0, {"label_42"}));
		REQUIRE(CHECK_COLUMN(old_row, 1, {21.0}));

		// Spot-check a tail row.
		auto tail_row = con.Query("SELECT label, score FROM t WHERE id = 100049");
		REQUIRE(CHECK_COLUMN(tail_row, 0, {"tail_100049"}));
		REQUIRE(CHECK_COLUMN(tail_row, 1, {50024.5}));
	}

	// ---- Phase 4: control — same 50-row append but forcing WriteToDisk via one UPDATE ----
	// Build an identical 100K-row baseline, reopen, append the same 50 rows PLUS one
	// UPDATE (which triggers WriteToDisk for the whole column).  This measures what the
	// non-incremental path would have written for the same append workload.
	idx_t control_bytes = 0;
	{
		{
			DuckDB db(ctrl_path);
			Connection con(db);

			REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
			REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));
			REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (id INTEGER, label VARCHAR, score DOUBLE)"));

			{
				Appender appender(con, "t");
				for (int32_t i = 0; i < 100000; i++) {
					appender.BeginRow();
					appender.Append<int32_t>(i);
					appender.Append<const char *>(("label_" + std::to_string(i % 1000)).c_str());
					appender.Append<double>(i * 0.5);
					appender.EndRow();
				}
			}
			REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		}
		{
			DuckDB db(ctrl_path);
			Connection con(db);

			REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
			REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));

			{
				Appender appender(con, "t");
				for (int32_t i = 100000; i < 100050; i++) {
					appender.BeginRow();
					appender.Append<int32_t>(i);
					appender.Append<const char *>(("tail_" + std::to_string(i)).c_str());
					appender.Append<double>(i * 0.5);
					appender.EndRow();
				}
			}

			// One UPDATE on a persistent row forces WriteToDisk for the entire column.
			REQUIRE_NO_FAIL(con.Query("UPDATE t SET score = -1.0 WHERE id = 0"));

			control_bytes = MeasureCheckpointWrite(con, ctrl_path);
			REQUIRE(control_bytes > 0);
		}
	}

	// ---- Assertions ----
	INFO("block_size=" << block_size << " full_rewrite_bytes=" << full_rewrite_bytes
	                   << " incremental_bytes=" << incremental_bytes << " control_bytes=" << control_bytes);

	// 1. Incremental path writes far less than a full WriteToDisk of the same baseline
	//    (control_bytes approximates what WriteToDisk would cost for the same append).
	REQUIRE(incremental_bytes < control_bytes);

	// 2. Tight absolute upper bound: 50 rows across 3 columns fit in a single packed
	//    block; allow up to 10 blocks for metadata/WAL overhead.
	REQUIRE(block_size > 0);
	REQUIRE(incremental_bytes <= 10 * block_size);

	DeleteDatabase(db_path);
	DeleteDatabase(ctrl_path);
}

TEST_CASE("Incremental checkpoint is faster than full rewrite on append-only workload", "[api][.]") {
	auto db_path = TestDirectoryPath() + "/incremental_checkpoint_latency.db";
	DeleteDatabase(db_path);

	auto elapsed_ms = [](std::chrono::steady_clock::time_point t0) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	};

	int64_t full_ms = 0;
	int64_t incremental_ms = 0;

	{
		DuckDB db(db_path);
		Connection con(db);

		REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
		REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (id INTEGER, label VARCHAR, score DOUBLE)"));

		{
			Appender appender(con, "t");
			for (int32_t i = 0; i < 100000; i++) {
				appender.BeginRow();
				appender.Append<int32_t>(i);
				appender.Append<const char *>(("label_" + std::to_string(i % 1000)).c_str());
				appender.Append<double>(i * 0.5);
				appender.EndRow();
			}
		}

		// Full rewrite timing.
		auto t0 = std::chrono::steady_clock::now();
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		full_ms = elapsed_ms(t0);
	}

	{
		DuckDB db(db_path);
		Connection con(db);

		REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
		REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));

		{
			Appender appender(con, "t");
			for (int32_t i = 100000; i < 100050; i++) {
				appender.BeginRow();
				appender.Append<int32_t>(i);
				appender.Append<const char *>(("tail_" + std::to_string(i)).c_str());
				appender.Append<double>(i * 0.5);
				appender.EndRow();
			}
		}

		// Incremental checkpoint timing.
		auto t0 = std::chrono::steady_clock::now();
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		incremental_ms = elapsed_ms(t0);
	}

	// Incremental checkpoint must be faster than the full rewrite.
	// We only assert a weak bound (< full time) since CI machines vary.
	INFO("full_ms=" << full_ms << " incremental_ms=" << incremental_ms);
	REQUIRE(incremental_ms <= full_ms);

	DeleteDatabase(db_path);
}

TEST_CASE("Checkpoint with concurrent reader does not crash and preserves data", "[api][.]") {
	auto db_path = TestDirectoryPath() + "/incremental_checkpoint_concurrent.db";
	DeleteDatabase(db_path);

	{
		DuckDB db(db_path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
		REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (id INTEGER, val INTEGER)"));
		{
			Appender appender(con, "t");
			for (int32_t i = 0; i < 1000; i++) {
				appender.BeginRow();
				appender.Append<int32_t>(i);
				appender.Append<int32_t>(i);
				appender.EndRow();
			}
		}
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}

	{
		DuckDB db(db_path);
		Connection writer(db);
		Connection reader(db);

		REQUIRE_NO_FAIL(writer.Query("SET wal_autocheckpoint = '1TB'"));
		REQUIRE_NO_FAIL(writer.Query("PRAGMA disable_checkpoint_on_shutdown"));

		{
			Appender appender(writer, "t");
			for (int32_t i = 1000; i < 1050; i++) {
				appender.BeginRow();
				appender.Append<int32_t>(i);
				appender.Append<int32_t>(i);
				appender.EndRow();
			}
		}

		// Open a read transaction — triggers CONCURRENT_CHECKPOINT path.
		REQUIRE_NO_FAIL(reader.Query("BEGIN TRANSACTION"));
		auto read_result = reader.Query("SELECT COUNT(*) FROM t");
		REQUIRE(CHECK_COLUMN(read_result, 0, {1050}));

		// CHECKPOINT while reader transaction is open. Must not crash.
		REQUIRE_NO_FAIL(writer.Query("CHECKPOINT"));

		// Reader snapshot is stable.
		auto read_result2 = reader.Query("SELECT COUNT(*) FROM t");
		REQUIRE(CHECK_COLUMN(read_result2, 0, {1050}));

		REQUIRE_NO_FAIL(reader.Query("COMMIT"));

		auto write_result = writer.Query("SELECT COUNT(*) FROM t");
		REQUIRE(CHECK_COLUMN(write_result, 0, {1050}));
	}

	// Data survives restart.
	{
		DuckDB db(db_path);
		Connection con(db);

		auto res = con.Query("SELECT COUNT(*) FROM t");
		REQUIRE(CHECK_COLUMN(res, 0, {1050}));

		auto tail = con.Query("SELECT val FROM t WHERE id = 1049");
		REQUIRE(CHECK_COLUMN(tail, 0, {1049}));

		auto base = con.Query("SELECT val FROM t WHERE id = 0");
		REQUIRE(CHECK_COLUMN(base, 0, {0}));
	}

	DeleteDatabase(db_path);
}

TEST_CASE("Multiple sequential incremental checkpoints do not cause unbounded file growth", "[api][.]") {
	auto db_path = TestDirectoryPath() + "/incremental_checkpoint_growth.db";
	DeleteDatabase(db_path);

	// Build a modest persistent baseline (10K rows).
	idx_t baseline_size = 0;
	idx_t block_size = 0;
	{
		DuckDB db(db_path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
		REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (id INTEGER, val VARCHAR)"));
		{
			Appender appender(con, "t");
			for (int32_t i = 0; i < 10000; i++) {
				appender.BeginRow();
				appender.Append<int32_t>(i);
				appender.Append<const char *>(("v_" + std::to_string(i)).c_str());
				appender.EndRow();
			}
		}
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));

		auto block_info = con.Query("SELECT block_size FROM pragma_database_size()");
		block_size = block_info->GetValue(0, 0).GetValue<idx_t>();
		baseline_size = GetDbFileSize(db_path);
	}
	REQUIRE(block_size > 0);
	REQUIRE(baseline_size > 0);

	// Run 20 rounds of: reopen → append 50 rows → CHECKPOINT.
	// Each round is a pure append-only workload so the incremental fast path fires.
	// Total appended rows = 1000, which is 10% of the baseline.  File growth should
	// be proportional to the appended data, not to the number of checkpoint rounds.
	static constexpr int kRounds = 20;
	static constexpr int kRowsPerRound = 50;

	for (int round = 0; round < kRounds; round++) {
		DuckDB db(db_path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
		REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));
		{
			Appender appender(con, "t");
			int32_t base = 10000 + round * kRowsPerRound;
			for (int32_t i = base; i < base + kRowsPerRound; i++) {
				appender.BeginRow();
				appender.Append<int32_t>(i);
				appender.Append<const char *>(("tail_" + std::to_string(i)).c_str());
				appender.EndRow();
			}
		}
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}

	// Verify final state on restart.
	{
		DuckDB db(db_path);
		Connection con(db);

		auto res = con.Query("SELECT COUNT(*) FROM t");
		REQUIRE(CHECK_COLUMN(res, 0, {10000 + kRounds * kRowsPerRound}));

		// Spot-check first and last tail rows.
		auto first_tail = con.Query("SELECT val FROM t WHERE id = 10000");
		REQUIRE(CHECK_COLUMN(first_tail, 0, {"tail_10000"}));

		auto last_tail =
		    con.Query("SELECT val FROM t WHERE id = " + std::to_string(10000 + kRounds * kRowsPerRound - 1));
		REQUIRE(CHECK_COLUMN(last_tail, 0, {"tail_" + std::to_string(10000 + kRounds * kRowsPerRound - 1)}));
	}

	// File size growth must be bounded: each round writes at most a few blocks.
	// Allow 5 blocks per round + 5 blocks of per-checkpoint metadata overhead.
	auto final_size = GetDbFileSize(db_path);
	auto growth = final_size > baseline_size ? final_size - baseline_size : 0;
	INFO("baseline_size=" << baseline_size << " final_size=" << final_size << " growth=" << growth
	                      << " block_size=" << block_size << " rounds=" << kRounds);
	REQUIRE(growth <= static_cast<idx_t>(kRounds) * 10 * block_size);

	DeleteDatabase(db_path);
}
