#pragma once

// ============================================================================
// jsonx/json_codec.hpp — bounded first-party JSON codec, manifest grade
// (MVP-2 batch design section 3.1; cpp-design D-1: no vendored parser)
//
// Parses and writes exactly the value model the runtime's machine files
// need: null, bool, u64, UTF-8 string, array, and order-preserving
// object. Hard bounds are part of the contract, not tuning knobs:
// input <= 1 MiB, nesting depth <= 8, and NO floating-point or signed
// numbers (the manifest carries none; a float in a machine file is a
// schema violation, not a rounding question). Unknown structure fails
// closed with a typed error naming the offset.
//
// The writer is deterministic: the value the caller builds IS the byte
// order (no sorting, no reformatting) — the cognition-bundle manifest is
// written once and digested, so byte identity is load-bearing.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/types.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qiven::runtime::jsonx
{
inline constexpr usize max_input_bytes = 1024 * 1024;
inline constexpr u32 max_depth         = 8;

inline constexpr i32 err_malformed = 64; // ARCH/cpp-design section 5: IPC/protocol frame class
inline constexpr i32 err_bounds    = 65;

struct JsonValue;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue
{
    enum class Kind : u8
    {
        Null,
        Bool,
        Number, // u64 only
        String,
        Array,
        Object,
    };

    Kind kind        = Kind::Null;
    bool bool_value  = false;
    u64 number_value = 0;
    std::string string_value;
    std::vector<JsonValue> array;
    JsonObject object;

    [[nodiscard]] static JsonValue make_null()
    {
        return JsonValue {};
    }
    [[nodiscard]] static JsonValue make_bool(bool value)
    {
        return JsonValue { Kind::Bool, value, 0, "", {}, {} };
    }
    [[nodiscard]] static JsonValue make_number(u64 value)
    {
        return JsonValue { Kind::Number, false, value, "", {}, {} };
    }
    [[nodiscard]] static JsonValue make_string(std::string value)
    {
        return JsonValue { Kind::String, false, 0, std::move(value), {}, {} };
    }
    [[nodiscard]] static JsonValue make_array(std::vector<JsonValue> values)
    {
        return JsonValue { Kind::Array, false, 0, "", std::move(values), {} };
    }
    [[nodiscard]] static JsonValue make_object(JsonObject members)
    {
        return JsonValue { Kind::Object, false, 0, "", {}, std::move(members) };
    }

    // Null-result accessors: missing keys and wrong types read as absence;
    // callers that REQUIRE a field check has() + kind explicitly.
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept
    {
        if (kind != Kind::Object)
        {
            return nullptr;
        }
        for (const auto& member : object)
        {
            if (member.first == key)
            {
                return &member.second;
            }
        }
        return nullptr;
    }
    [[nodiscard]] bool has(std::string_view key) const noexcept
    {
        return find(key) != nullptr;
    }
    [[nodiscard]] bool is(std::string_view key, Kind expected) const noexcept
    {
        const JsonValue* found = find(key);
        return found != nullptr && found->kind == expected;
    }
    [[nodiscard]] u64 number_or(std::string_view key, u64 fallback) const noexcept
    {
        const JsonValue* found = find(key);
        return found != nullptr && found->kind == Kind::Number ? found->number_value : fallback;
    }
    [[nodiscard]] std::string string_or(std::string_view key, std::string fallback) const
    {
        const JsonValue* found = find(key);
        return found != nullptr && found->kind == Kind::String ? found->string_value : fallback;
    }
};

// Strict parse of a complete UTF-8 JSON document. Rejects: truncated
// input, trailing garbage, duplicate object keys, invalid escapes,
// invalid UTF-8, negative/floating/oversized numbers, depth/size bound
// violations. Offset is included in the error message.
[[nodiscard]] qiven::Result<JsonValue> parse(std::string_view bytes);

// Deterministic compact serialization. Strings escape " \ and the C0
// controls (uXXXX for non-printables); numbers render as unsigned
// decimal. Building an ill-formed value (key with '=' shape issues do
// not exist here) cannot occur — the model only holds valid pieces.
[[nodiscard]] std::string write(const JsonValue& value);
} // namespace qiven::runtime::jsonx
