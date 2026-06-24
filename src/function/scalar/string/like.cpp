#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

bool Glob(const char *string, idx_t slen, const char *pattern, idx_t plen, bool allow_question_mark) {
	idx_t sidx = 0;
	idx_t pidx = 0;
main_loop : {
	// main matching loop
	while (sidx < slen && pidx < plen) {
		char s = string[sidx];
		char p = pattern[pidx];
		switch (p) {
		case '*': {
			// asterisk: match any set of characters
			// skip any subsequent asterisks
			pidx++;
			while (pidx < plen && pattern[pidx] == '*') {
				pidx++;
			}
			// if the asterisk is the last character, the pattern always matches
			if (pidx == plen) {
				return true;
			}
			// recursively match the remainder of the pattern
			for (; sidx < slen; sidx++) {
				if (Glob(string + sidx, slen - sidx, pattern + pidx, plen - pidx)) {
					return true;
				}
			}
			return false;
		}
		case '?':
			// when enabled: matches anything but null
			if (allow_question_mark) {
				break;
			}
			DUCKDB_EXPLICIT_FALLTHROUGH;
		case '[':
			pidx++;
			goto parse_bracket;
		case '\\':
			// escape character, next character needs to match literally
			pidx++;
			// check that we still have a character remaining
			if (pidx == plen) {
				return false;
			}
			p = pattern[pidx];
			if (s != p) {
				return false;
			}
			break;
		default:
			// not a control character: characters need to match literally
			if (s != p) {
				return false;
			}
			break;
		}
		sidx++;
		pidx++;
	}
	while (pidx < plen && pattern[pidx] == '*') {
		pidx++;
	}
	// we are finished only if we have consumed the full pattern
	return pidx == plen && sidx == slen;
}
parse_bracket : {
	// inside a bracket
	if (pidx == plen) {
		return false;
	}
	// check the first character
	// if it is an exclamation mark we need to invert our logic
	char p = pattern[pidx];
	char s = string[sidx];
	bool invert = false;
	if (p == '!') {
		invert = true;
		pidx++;
	}
	bool found_match = invert;
	idx_t start_pos = pidx;
	bool found_closing_bracket = false;
	// now check the remainder of the pattern
	while (pidx < plen) {
		p = pattern[pidx];
		// if the first character is a closing bracket, we match it literally
		// otherwise it indicates an end of bracket
		if (p == ']' && pidx > start_pos) {
			// end of bracket found: we are done
			found_closing_bracket = true;
			pidx++;
			break;
		}
		// we either match a range (a-b) or a single character (a)
		// check if the next character is a dash
		if (pidx + 1 == plen) {
			// no next character!
			break;
		}
		bool matches;
		if (pattern[pidx + 1] == '-') {
			// range! find the next character in the range
			if (pidx + 2 == plen) {
				break;
			}
			char next_char = pattern[pidx + 2];
			// check if the current character is within the range
			matches = s >= p && s <= next_char;
			// shift the pattern forward past the range
			pidx += 3;
		} else {
			// no range! perform a direct match
			matches = p == s;
			// shift the pattern forward past the character
			pidx++;
		}
		if (found_match == invert && matches) {
			// found a match! set the found_matches flag
			// we keep on pattern matching after this until we reach the end bracket
			// however, we don't need to update the found_match flag anymore
			found_match = !invert;
		}
	}
	if (!found_closing_bracket) {
		// no end of bracket: invalid pattern
		return false;
	}
	if (!found_match) {
		// did not match the bracket: return false;
		return false;
	}
	// finished the bracket matching: move forward
	sidx++;
	goto main_loop;
}
}

namespace {
struct StandardCharacterReader {
	static void NextCharacter(const char *sdata, idx_t slen, idx_t &sidx) {
		sidx++;
		while (sidx < slen && !IsCharacter(sdata[sidx])) {
			sidx++;
		}
	}

	static char Operation(const char *data, idx_t pos) {
		return data[pos];
	}
};

struct ASCIILCaseReader {
	static void NextCharacter(const char *sdata, idx_t slen, idx_t &sidx) {
		sidx++;
	}

	static char Operation(const char *data, idx_t pos) {
		return (char)StringUtil::ASCII_TO_LOWER_MAP[(uint8_t)data[pos]];
	}
};

template <char PERCENTAGE, char UNDERSCORE, bool HAS_ESCAPE, class READER = StandardCharacterReader>
bool TemplatedLikeOperator(const char *sdata, idx_t slen, const char *pdata, idx_t plen, char escape) {
	idx_t pidx = 0;
	idx_t sidx = 0;
	for (; pidx < plen && sidx < slen; pidx++) {
		char pchar = READER::Operation(pdata, pidx);
		char schar = READER::Operation(sdata, sidx);
		if (HAS_ESCAPE && pchar == escape) {
			pidx++;
			if (pidx == plen) {
				throw SyntaxException("Like pattern must not end with escape character!");
			}
			if (pdata[pidx] != schar) {
				return false;
			}
			sidx++;
		} else if (pchar == UNDERSCORE) {
			READER::NextCharacter(sdata, slen, sidx);
		} else if (pchar == PERCENTAGE) {
			pidx++;
			while (pidx < plen && pdata[pidx] == PERCENTAGE) {
				pidx++;
			}
			if (pidx == plen) {
				return true; /* tail is acceptable */
			}
			for (; sidx < slen; sidx++) {
				if (TemplatedLikeOperator<PERCENTAGE, UNDERSCORE, HAS_ESCAPE, READER>(
				        sdata + sidx, slen - sidx, pdata + pidx, plen - pidx, escape)) {
					return true;
				}
			}
			return false;
		} else if (pchar == schar) {
			sidx++;
		} else {
			return false;
		}
	}
	while (pidx < plen && pdata[pidx] == PERCENTAGE) {
		pidx++;
	}
	return pidx == plen && sidx == slen;
}

struct LikeSegment {
	explicit LikeSegment(string pattern) : pattern(std::move(pattern)) {
	}

	string pattern;
};

struct LikeMatcher : public FunctionData {
	LikeMatcher(string like_pattern_p, vector<LikeSegment> segments, bool has_start_percentage, bool has_end_percentage)
	    : like_pattern(std::move(like_pattern_p)), segments(std::move(segments)),
	      has_start_percentage(has_start_percentage), has_end_percentage(has_end_percentage) {
	}

	bool Match(string_t &str) {
		auto str_data = const_uchar_ptr_cast(str.GetData());
		auto str_len = str.GetSize();
		idx_t segment_idx = 0;
		idx_t end_idx = segments.size() - 1;
		if (!has_start_percentage) {
			// no start sample_size: match the first part of the string directly
			auto &segment = segments[0];
			if (str_len < segment.pattern.size()) {
				return false;
			}
			if (memcmp(str_data, segment.pattern.c_str(), segment.pattern.size()) != 0) {
				return false;
			}
			str_data += segment.pattern.size();
			str_len -= segment.pattern.size();
			segment_idx++;
			if (segments.size() == 1) {
				// only one segment, and it matches
				// we have a match if there is an end sample_size, OR if the memcmp was an exact match (remaining str is
				// empty)
				return has_end_percentage || str_len == 0;
			}
		}
		// main match loop: for every segment in the middle, use Contains to find the needle in the haystack
		for (; segment_idx < end_idx; segment_idx++) {
			auto &segment = segments[segment_idx];
			// find the pattern of the current segment
			idx_t next_offset =
			    FindStrInStr(str_data, str_len, const_uchar_ptr_cast(segment.pattern.c_str()), segment.pattern.size());
			if (next_offset == DConstants::INVALID_INDEX) {
				// could not find this pattern in the string: no match
				return false;
			}
			idx_t offset = next_offset + segment.pattern.size();
			str_data += offset;
			str_len -= offset;
		}
		if (!has_end_percentage) {
			end_idx--;
			// no end sample_size: match the final segment now
			auto &segment = segments.back();
			if (str_len < segment.pattern.size()) {
				return false;
			}
			if (memcmp(str_data + str_len - segment.pattern.size(), segment.pattern.c_str(), segment.pattern.size()) !=
			    0) {
				return false;
			}
			return true;
		} else {
			auto &segment = segments.back();
			// find the pattern of the current segment
			idx_t next_offset =
			    FindStrInStr(str_data, str_len, const_uchar_ptr_cast(segment.pattern.c_str()), segment.pattern.size());
			return next_offset != DConstants::INVALID_INDEX;
		}
	}

	static unique_ptr<LikeMatcher> CreateLikeMatcher(string like_pattern, char escape = '\0') {
		vector<LikeSegment> segments;
		idx_t last_non_pattern = 0;
		bool has_start_percentage = false;
		bool has_end_percentage = false;
		for (idx_t i = 0; i < like_pattern.size(); i++) {
			auto ch = like_pattern[i];
			if (ch == escape || ch == '%' || ch == '_') {
				// special character, push a constant pattern
				if (i > last_non_pattern) {
					segments.emplace_back(like_pattern.substr(last_non_pattern, i - last_non_pattern));
				}
				last_non_pattern = i + 1;
				if (ch == escape || ch == '_') {
					// escape or underscore: could not create efficient like matcher
					// FIXME: we could handle escaped percentages here
					return nullptr;
				} else {
					// sample_size
					if (i == 0) {
						has_start_percentage = true;
					}
					if (i + 1 == like_pattern.size()) {
						has_end_percentage = true;
					}
				}
			}
		}
		if (last_non_pattern < like_pattern.size()) {
			segments.emplace_back(like_pattern.substr(last_non_pattern, like_pattern.size() - last_non_pattern));
		}
		if (segments.empty()) {
			return nullptr;
		}
		return make_uniq<LikeMatcher>(std::move(like_pattern), std::move(segments), has_start_percentage,
		                              has_end_percentage);
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LikeMatcher>(like_pattern, segments, has_start_percentage, has_end_percentage);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<LikeMatcher>();
		return like_pattern == other.like_pattern;
	}

private:
	string like_pattern;
	vector<LikeSegment> segments;
	bool has_start_percentage;
	bool has_end_percentage;
};

unique_ptr<FunctionData> LikeBindFunction(ClientContext &context, ScalarFunction &bound_function,
                                          vector<unique_ptr<Expression>> &arguments) {
	// pattern is the second argument. If its constant, we can already prepare the pattern and store it for later.
	D_ASSERT(arguments.size() == 2 || arguments.size() == 3);
	for (auto &arg : arguments) {
		if (arg->return_type.id() == LogicalTypeId::VARCHAR && !StringType::GetCollation(arg->return_type).empty()) {
			return nullptr;
		}
	}
	if (arguments[1]->IsFoldable()) {
		Value pattern_str = ExpressionExecutor::EvaluateScalar(context, *arguments[1]);
		return LikeMatcher::CreateLikeMatcher(pattern_str.ToString());
	}
	return nullptr;
}

bool LikeOperatorFunction(const char *s, idx_t slen, const char *pattern, idx_t plen, char escape) {
	return TemplatedLikeOperator<'%', '_', true>(s, slen, pattern, plen, escape);
}

bool LikeOperatorFunction(const char *s, idx_t slen, const char *pattern, idx_t plen) {
	return TemplatedLikeOperator<'%', '_', false>(s, slen, pattern, plen, '\0');
}

bool LikeOperatorFunction(string_t &s, string_t &pat) {
	return LikeOperatorFunction(s.GetData(), s.GetSize(), pat.GetData(), pat.GetSize());
}

bool LikeOperatorFunction(string_t &s, string_t &pat, char escape) {
	return LikeOperatorFunction(s.GetData(), s.GetSize(), pat.GetData(), pat.GetSize(), escape);
}

char GetEscapeChar(string_t escape) {
	// Only one escape character should be allowed
	if (escape.GetSize() > 1) {
		throw SyntaxException("Invalid escape string. Escape string must be empty or one character.");
	}
	return escape.GetSize() == 0 ? '\0' : *escape.GetData();
}

struct LikeEscapeOperator {
	template <class TA, class TB, class TC>
	static inline bool Operation(TA str, TB pattern, TC escape) {
		char escape_char = GetEscapeChar(escape);
		return LikeOperatorFunction(str.GetData(), str.GetSize(), pattern.GetData(), pattern.GetSize(), escape_char);
	}
};

struct NotLikeEscapeOperator {
	template <class TA, class TB, class TC>
	static inline bool Operation(TA str, TB pattern, TC escape) {
		return !LikeEscapeOperator::Operation(str, pattern, escape);
	}
};

struct LikeOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return LikeOperatorFunction(str, pattern);
	}
};

bool ILikeOperatorFunction(string_t &str, string_t &pattern, char escape = '\0') {
	auto str_data = str.GetData();
	auto str_size = str.GetSize();
	auto pat_data = pattern.GetData();
	auto pat_size = pattern.GetSize();

	// lowercase both the str and the pattern
	idx_t str_llength = LowerLength(str_data, str_size);
	auto str_ldata = make_unsafe_uniq_array_uninitialized<char>(str_llength);
	LowerCase(str_data, str_size, str_ldata.get());

	idx_t pat_llength = LowerLength(pat_data, pat_size);
	auto pat_ldata = make_unsafe_uniq_array_uninitialized<char>(pat_llength);
	LowerCase(pat_data, pat_size, pat_ldata.get());
	string_t str_lcase(str_ldata.get(), UnsafeNumericCast<uint32_t>(str_llength));
	string_t pat_lcase(pat_ldata.get(), UnsafeNumericCast<uint32_t>(pat_llength));
	// '\0' is the "no escape" sentinel: use the non-escape matcher so embedded NUL bytes are matched literally
	if (escape == '\0') {
		return LikeOperatorFunction(str_lcase, pat_lcase);
	}
	return LikeOperatorFunction(str_lcase, pat_lcase, escape);
}

struct ILikeEscapeOperator {
	template <class TA, class TB, class TC>
	static inline bool Operation(TA str, TB pattern, TC escape) {
		char escape_char = GetEscapeChar(escape);
		return ILikeOperatorFunction(str, pattern, escape_char);
	}
};

struct NotILikeEscapeOperator {
	template <class TA, class TB, class TC>
	static inline bool Operation(TA str, TB pattern, TC escape) {
		return !ILikeEscapeOperator::Operation(str, pattern, escape);
	}
};

struct ILikeOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return ILikeOperatorFunction(str, pattern);
	}
};

struct NotLikeOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return !LikeOperatorFunction(str, pattern);
	}
};

struct NotILikeOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return !ILikeOperator::Operation<TA, TB, TR>(str, pattern);
	}
};

struct ILikeOperatorASCII {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return TemplatedLikeOperator<'%', '_', false, ASCIILCaseReader>(str.GetData(), str.GetSize(), pattern.GetData(),
		                                                                pattern.GetSize(), '\0');
	}
};

struct NotILikeOperatorASCII {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return !ILikeOperatorASCII::Operation<TA, TB, TR>(str, pattern);
	}
};

struct GlobOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return Glob(str.GetData(), str.GetSize(), pattern.GetData(), pattern.GetSize());
	}
};

// This can be moved to the scalar_function class
template <typename FUNC>
void LikeEscapeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &str = args.data[0];
	auto &pattern = args.data[1];
	auto &escape = args.data[2];

	TernaryExecutor::Execute<string_t, string_t, string_t, bool>(
	    str, pattern, escape, result, args.size(), FUNC::template Operation<string_t, string_t, string_t>);
}

// Execution function for ILIKE / NOT ILIKE ... ESCAPE. Mirrors ILikeFunction: when the pattern and escape are
// both constant, lowercase the pattern once and reuse a scratch buffer for the per-row string lowercasing. If the
// escape character does not occur in the pattern, escape handling is a no-op, so we build the case-folded
// LikeMatcher and use the fast SIMD contains path; otherwise we fall back to the generic escape-aware matcher on
// the lowercased values. Non-constant pattern/escape falls back to the per-row ternary path.
template <bool INVERT>
void ILikeEscapeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &str_vec = args.data[0];
	auto &pattern_vec = args.data[1];
	auto &escape_vec = args.data[2];

	if (pattern_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(pattern_vec) &&
	    escape_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(escape_vec)) {
		auto pattern = *ConstantVector::GetData<string_t>(pattern_vec);
		auto escape = *ConstantVector::GetData<string_t>(escape_vec);
		char escape_char = GetEscapeChar(escape);

		// lowercase the pattern exactly once, up front
		idx_t pat_llength = LowerLength(pattern.GetData(), pattern.GetSize());
		auto pat_ldata = make_unsafe_uniq_array_uninitialized<char>(pat_llength);
		LowerCase(pattern.GetData(), pattern.GetSize(), pat_ldata.get());
		string_t pat_lcase(pat_ldata.get(), UnsafeNumericCast<uint32_t>(pat_llength));

		// the matcher cannot honor escape semantics, so only use it when the escape char never appears in the
		// (lowercased) pattern, in which case escape is irrelevant and the pattern is a plain LIKE pattern
		unique_ptr<LikeMatcher> matcher;
		bool escape_active =
		    escape_char != '\0' && memchr(pat_lcase.GetData(), escape_char, pat_lcase.GetSize()) != nullptr;
		if (!escape_active) {
			matcher = LikeMatcher::CreateLikeMatcher(string(pat_lcase.GetData(), pat_lcase.GetSize()));
		}

		// reusable scratch buffer for lowercasing each string value (grown on demand)
		idx_t scratch_size = 0;
		unsafe_unique_array<char> scratch;
		UnaryExecutor::Execute<string_t, bool>(str_vec, result, args.size(), [&](string_t str) {
			idx_t str_llength = LowerLength(str.GetData(), str.GetSize());
			if (str_llength > scratch_size) {
				scratch = make_unsafe_uniq_array_uninitialized<char>(str_llength);
				scratch_size = str_llength;
			}
			LowerCase(str.GetData(), str.GetSize(), scratch.get());
			string_t str_lcase(scratch.get(), UnsafeNumericCast<uint32_t>(str_llength));
			// '\0' escape means no escape: use the non-escape matcher so embedded NUL bytes are matched literally
			bool match = matcher ? matcher->Match(str_lcase)
			                     : (escape_char == '\0' ? LikeOperatorFunction(str_lcase, pat_lcase)
			                                            : LikeOperatorFunction(str_lcase, pat_lcase, escape_char));
			return INVERT ? !match : match;
		});
		return;
	}
	// non-constant pattern/escape: fall back to the generic per-row implementation
	if (INVERT) {
		TernaryExecutor::Execute<string_t, string_t, string_t, bool>(
		    str_vec, pattern_vec, escape_vec, result, args.size(),
		    NotILikeEscapeOperator::Operation<string_t, string_t, string_t>);
	} else {
		TernaryExecutor::Execute<string_t, string_t, string_t, bool>(
		    str_vec, pattern_vec, escape_vec, result, args.size(),
		    ILikeEscapeOperator::Operation<string_t, string_t, string_t>);
	}
}

template <class ASCII_OP>
unique_ptr<BaseStatistics> ILikePropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() >= 1);
	// can only propagate stats if the children have stats
	if (!StringStats::CanContainUnicode(child_stats[0])) {
		expr.function.SetFunctionCallback(ScalarFunction::BinaryFunction<string_t, string_t, bool, ASCII_OP>);
	}
	return nullptr;
}

// Execution function for ILIKE / NOT ILIKE on the (possibly) Unicode path.
// When the pattern is constant we lowercase it exactly once instead of once per row, and we reuse a single
// scratch buffer to lowercase each string value instead of heap-allocating per row. This avoids two heap
// allocations + a redundant pattern case-fold on every row that the generic ILikeOperatorFunction incurs.
// (The ASCII-only fast path installed by ILikePropagateStats already avoids allocations, so it is unaffected.)
template <class OP, bool INVERT>
void ILikeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &pattern_vec = input.data[1];
	if (pattern_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(pattern_vec)) {
		// constant pattern: lowercase it exactly once, up front
		auto pattern = *ConstantVector::GetData<string_t>(pattern_vec);
		idx_t pat_llength = LowerLength(pattern.GetData(), pattern.GetSize());
		auto pat_ldata = make_unsafe_uniq_array_uninitialized<char>(pat_llength);
		LowerCase(pattern.GetData(), pattern.GetSize(), pat_ldata.get());
		string_t pat_lcase(pat_ldata.get(), UnsafeNumericCast<uint32_t>(pat_llength));

		// Build a case-insensitive matcher from the lowercased pattern. Because both the pattern and each string
		// value are lowercased, a case-sensitive LikeMatcher over lowercased data is equivalent to ILIKE, and it
		// uses the fast SIMD FindStrInStr/memcmp contains path. Returns null for patterns with '_' or only '%';
		// in that case we fall back to the generic recursive matcher on the lowercased values.
		auto matcher = LikeMatcher::CreateLikeMatcher(string(pat_lcase.GetData(), pat_lcase.GetSize()));

		// reusable scratch buffer for lowercasing each string value (grown on demand)
		idx_t scratch_size = 0;
		unsafe_unique_array<char> scratch;
		UnaryExecutor::Execute<string_t, bool>(input.data[0], result, input.size(), [&](string_t str) {
			idx_t str_llength = LowerLength(str.GetData(), str.GetSize());
			if (str_llength > scratch_size) {
				scratch = make_unsafe_uniq_array_uninitialized<char>(str_llength);
				scratch_size = str_llength;
			}
			LowerCase(str.GetData(), str.GetSize(), scratch.get());
			string_t str_lcase(scratch.get(), UnsafeNumericCast<uint32_t>(str_llength));
			bool match = matcher ? matcher->Match(str_lcase) : LikeOperatorFunction(str_lcase, pat_lcase);
			return INVERT ? !match : match;
		});
		return;
	}
	// non-constant pattern: fall back to the generic per-row implementation
	BinaryExecutor::ExecuteStandard<string_t, string_t, bool, OP>(input.data[0], input.data[1], result, input.size());
}

template <class OP, bool INVERT>
void RegularLikeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	if (func_expr.bind_info) {
		auto &matcher = func_expr.bind_info->Cast<LikeMatcher>();
		// use fast like matcher
		UnaryExecutor::Execute<string_t, bool>(input.data[0], result, input.size(), [&](string_t input) {
			return INVERT ? !matcher.Match(input) : matcher.Match(input);
		});
	} else {
		// use generic like matcher
		BinaryExecutor::ExecuteStandard<string_t, string_t, bool, OP>(input.data[0], input.data[1], result,
		                                                              input.size());
	}
}

} // namespace

ScalarFunction NotLikeFun::GetFunction() {
	ScalarFunction not_like("!~~", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                        RegularLikeFunction<NotLikeOperator, true>, LikeBindFunction);
	not_like.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return not_like;
}

ScalarFunction GlobPatternFun::GetFunction() {
	ScalarFunction glob("~~~", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                    ScalarFunction::BinaryFunction<string_t, string_t, bool, GlobOperator>);
	glob.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return glob;
}

ScalarFunction ILikeFun::GetFunction() {
	ScalarFunction ilike("~~*", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                     ILikeFunction<ILikeOperator, false>, nullptr, nullptr,
	                     ILikePropagateStats<ILikeOperatorASCII>);
	ilike.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return ilike;
}

ScalarFunction NotILikeFun::GetFunction() {
	ScalarFunction not_ilike("!~~*", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                         ILikeFunction<NotILikeOperator, true>, nullptr, nullptr,
	                         ILikePropagateStats<NotILikeOperatorASCII>);
	not_ilike.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return not_ilike;
}

ScalarFunction LikeFun::GetFunction() {
	ScalarFunction like("~~", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                    RegularLikeFunction<LikeOperator, false>, LikeBindFunction);
	like.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return like;
}

ScalarFunction NotLikeEscapeFun::GetFunction() {
	ScalarFunction not_like_escape("not_like_escape",
	                               {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                               LogicalType::BOOLEAN, LikeEscapeFunction<NotLikeEscapeOperator>);
	not_like_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return not_like_escape;
}

ScalarFunction IlikeEscapeFun::GetFunction() {
	ScalarFunction ilike_escape("ilike_escape", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                            LogicalType::BOOLEAN, ILikeEscapeFunction<false>);
	ilike_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return ilike_escape;
}

ScalarFunction NotIlikeEscapeFun::GetFunction() {
	ScalarFunction not_ilike_escape("not_ilike_escape",
	                                {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                LogicalType::BOOLEAN, ILikeEscapeFunction<true>);
	not_ilike_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return not_ilike_escape;
}
ScalarFunction LikeEscapeFun::GetFunction() {
	ScalarFunction like_escape("like_escape", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                           LogicalType::BOOLEAN, LikeEscapeFunction<LikeEscapeOperator>);
	like_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return like_escape;
}

} // namespace duckdb
