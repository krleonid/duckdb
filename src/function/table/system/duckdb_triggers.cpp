#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"

namespace duckdb {

struct DuckDBTriggersData : public GlobalTableFunctionState {
	DuckDBTriggersData() : offset(0) {
	}

	vector<reference<TriggerCatalogEntry>> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBTriggersBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("trigger_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("trigger_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("table_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("timing");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("event_manipulation");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("columns");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("for_each");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("temporary");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBTriggersInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBTriggersData>();

	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::TRIGGER_ENTRY,
		                  [&](CatalogEntry &entry) { result->entries.push_back(entry.Cast<TriggerCatalogEntry>()); });
	}
	return std::move(result);
}

static string TriggerTimingToString(TriggerTiming timing) {
	switch (timing) {
	case TriggerTiming::BEFORE:
		return "BEFORE";
	case TriggerTiming::AFTER:
		return "AFTER";
	case TriggerTiming::INSTEAD_OF:
		return "INSTEAD OF";
	default:
		return "UNKNOWN";
	}
}

static string TriggerForEachToString(TriggerForEach for_each) {
	switch (for_each) {
	case TriggerForEach::ROW:
		return "ROW";
	case TriggerForEach::STATEMENT:
		return "STATEMENT";
	default:
		return "UNKNOWN";
	}
}

static string TriggerEventTypeToString(TriggerEventType event_type) {
	switch (event_type) {
	case TriggerEventType::INSERT_EVENT:
		return "INSERT";
	case TriggerEventType::DELETE_EVENT:
		return "DELETE";
	case TriggerEventType::UPDATE_EVENT:
		return "UPDATE";
	default:
		return "UNKNOWN";
	}
}

void DuckDBTriggersFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBTriggersData>();
	if (data.offset >= data.entries.size()) {
		return;
	}

	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &trigger = data.entries[data.offset++].get();

		idx_t col = 0;
		output.SetValue(col++, count, Value(trigger.catalog.GetName()));
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(trigger.catalog.GetOid())));
		output.SetValue(col++, count, Value(trigger.schema.name));
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(trigger.schema.oid)));
		output.SetValue(col++, count, Value(trigger.name));
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(trigger.oid)));
		output.SetValue(col++, count, Value(trigger.base_table->table_name));
		output.SetValue(col++, count, Value(TriggerTimingToString(trigger.timing)));
		output.SetValue(col++, count, Value(TriggerEventTypeToString(trigger.event_type)));
		output.SetValue(col++, count, Value(StringUtil::Join(trigger.columns, ", ")));
		output.SetValue(col++, count, Value(TriggerForEachToString(trigger.for_each)));
		output.SetValue(col++, count, Value(trigger.comment));
		output.SetValue(col++, count, Value::MAP(trigger.tags));
		output.SetValue(col++, count, Value::BOOLEAN(trigger.temporary));
		output.SetValue(col++, count, Value(trigger.ToSQL()));

		count++;
	}
	output.SetCardinality(count);
}

void DuckDBTriggersFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(
	    TableFunction("duckdb_triggers", {}, DuckDBTriggersFunction, DuckDBTriggersBind, DuckDBTriggersInit));
}

} // namespace duckdb
