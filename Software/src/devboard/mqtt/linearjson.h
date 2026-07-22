#include <array>
#include <cstdint>
#include <cstdio>

#include <array>
#include <type_traits>
#include <utility>

// Linear JSON generator library

// For best results (lowest code and RAM usage):
// - Use the scatter-gather mechanism to break up large JSON payloads into smaller segments
// - Put constant segments into flash memory
// - Where possible, use pointers to existing data instead of copying into RAM
// - Break up large values into smaller chunks to avoid needing to concatenate first
// - Use J_FUNCTION to generate large or variable-length payloads on-the-fly

#define J_MORE 0x80  // High-bit flag: "More chunks are coming for this value"

struct JsonItem;
typedef const JsonItem* (*JsonGeneratorFunc)(size_t index);

typedef enum {
  J_END = 0,  // End of this segment
  J_SKIP,     // Skip this item (used for padding)
  J_OBJ_START,
  J_OBJ_END,
  J_LIST_START,
  J_LIST_END,
  J_BOOL,
  J_PBOOL,
  J_UINT32,
  J_FLOAT,
  J_PUINT8,
  J_PUINT16,
  J_PUINT32,
  J_PFLOAT,
  J_CHAR4,
  J_STR,
  J_PSTR,
  J_FUNCTION
} JsonType;

typedef union {
  uint32_t u32;
  float f;
  const bool* pbool;
  const uint8_t* pu8;
  const uint16_t* pu16;
  const uint32_t* pu32;
  const float* pf;
  char c4[4];
  const char* str;
  const char** pstr;
  JsonGeneratorFunc func;
} JsonVal;

typedef struct JsonItem {
  const char* key;
  uint32_t type;
  JsonVal val;
} JsonItem;

typedef struct {
  const JsonItem** segments;
  size_t seg_count;
  size_t seg_idx;
  size_t item_idx;
  size_t array_idx;
  size_t offset;
  uint8_t stage;
  uint8_t has_prev;
  uint8_t in_quote;
  uint32_t prev_type;
  char val_buf[24];
  size_t escape_offset;

  uint8_t in_func;
  size_t func_idx;
  size_t saved_item_idx;
  size_t func_item_idx;
  const JsonItem* func_segment;
} JsonState;

namespace Json {
inline JsonItem End() {
  return {nullptr, J_END, {}};
}
inline JsonItem Skip() {
  return {nullptr, J_SKIP, {}};
}

// Structural Tokens (Return a single JsonItem)
inline JsonItem ObjStart() {
  return {nullptr, J_OBJ_START, {}};
}
inline JsonItem ObjEnd() {
  return {nullptr, J_OBJ_END, {}};
}
inline JsonItem ListStart() {
  return {nullptr, J_LIST_START, {}};
}
inline JsonItem ListEnd() {
  return {nullptr, J_LIST_END, {}};
}

// Core Type Deductor (Evaluated at compile-time)
template <typename T>
inline JsonItem MakeItem(const char* key, T val, bool more) {
  JsonItem item{key, 0, {}};
  uint32_t type = 0;
  using U = std::decay_t<T>;  // Drops const, volatile, and converts arrays to pointers

  if constexpr (std::is_same_v<U, uint32_t> || std::is_same_v<U, int>) {
    type = J_UINT32;
    item.val.u32 = val;
  } else if constexpr (std::is_same_v<U, float>) {
    type = J_FLOAT;
    item.val.f = val;
  } else if constexpr (std::is_same_v<U, bool>) {
    type = J_BOOL;
    item.val.u32 = val ? 1 : 0;
  } else if constexpr (std::is_same_v<U, const bool*> || std::is_same_v<U, bool*>) {
    type = J_PBOOL;
    item.val.pbool = val;
  } else if constexpr (std::is_same_v<U, const uint8_t*> || std::is_same_v<U, uint8_t*>) {
    type = J_PUINT8;
    item.val.pu8 = val;
  } else if constexpr (std::is_same_v<U, const uint16_t*> || std::is_same_v<U, uint16_t*>) {
    type = J_PUINT16;
    item.val.pu16 = val;
  } else if constexpr (std::is_same_v<U, const uint32_t*> || std::is_same_v<U, uint32_t*> ||
                       std::is_same_v<U, const int*> || std::is_same_v<U, int*>) {
    type = J_PUINT32;
    item.val.pu32 = reinterpret_cast<const uint32_t*>(val);
  } else if constexpr (std::is_same_v<U, const float*> || std::is_same_v<U, float*>) {
    type = J_PFLOAT;
    item.val.pf = val;
  } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
    type = J_STR;
    item.val.str = val;
  } else if constexpr (std::is_same_v<U, const char**> || std::is_same_v<U, char**>) {
    type = J_PSTR;
    item.val.pstr = val;
  } else if constexpr (std::is_same_v<U, JsonItem>) {
    // Passing through a JsonItem directly (used for nested structures)
    type = val.type & 0x7F;
    item.val = val.val;
  } else {
    static_assert(sizeof(T*) == 0, "Unsupported type for Json::MakeItem");
  }

  if (more)
    type |= J_MORE;
  item.type = type;
  return item;
}

// Variadic Pack Unroller: Applies the key to the first item and sets J_MORE until the final element
template <size_t... Is, typename... Args>
inline std::array<JsonItem, sizeof...(Args)> KeyValImpl(std::index_sequence<Is...>, const char* key, Args... args) {
  return {MakeItem((Is == 0 ? key : nullptr), args, (Is < sizeof...(Args) - 1))...};
}

// Public API: Works perfectly for BOTH single values and multi-value concatenations
template <typename... Args>
inline std::array<JsonItem, sizeof...(Args)> KeyVal(const char* key, Args... args) {
  return KeyValImpl(std::index_sequence_for<Args...>{}, key, args...);
}

// Public API: Variable length values inside Lists (Implicitly forces key to nullptr)
template <typename... Args>
inline std::array<JsonItem, sizeof...(Args)> Val(Args... args) {
  return KeyValImpl(std::index_sequence_for<Args...>{}, nullptr, args...);
}

// Array helpers: auto-deduce count from C array, constexpr type from value type
template <size_t N>
inline JsonItem Array(const float (&arr)[N]) {
  return {nullptr, J_PFLOAT | (N << 8), {.pf = arr}};
}
template <size_t N>
inline JsonItem Array(const bool (&arr)[N]) {
  return {nullptr, J_PBOOL | (N << 8), {.pbool = arr}};
}
template <size_t N>
inline JsonItem Array(const uint8_t (&arr)[N]) {
  return {nullptr, J_PUINT8 | (N << 8), {.pu8 = arr}};
}
template <size_t N>
inline JsonItem Array(const uint16_t (&arr)[N]) {
  return {nullptr, J_PUINT16 | (N << 8), {.pu16 = arr}};
}
template <size_t N>
inline JsonItem Array(const uint32_t (&arr)[N]) {
  return {nullptr, J_PUINT32 | (N << 8), {.pu32 = arr}};
}

// Array helpers with explicit length
inline JsonItem Array(const float* arr, uint32_t len) {
  return {nullptr, J_PFLOAT | (len << 8), {.pf = arr}};
}
inline JsonItem Array(const bool* arr, uint32_t len) {
  return {nullptr, J_PBOOL | (len << 8), {.pbool = arr}};
}
inline JsonItem Array(const uint8_t* arr, uint32_t len) {
  return {nullptr, J_PUINT8 | (len << 8), {.pu8 = arr}};
}
inline JsonItem Array(const uint16_t* arr, uint32_t len) {
  return {nullptr, J_PUINT16 | (len << 8), {.pu16 = arr}};
}
inline JsonItem Array(const uint32_t* arr, uint32_t len) {
  return {nullptr, J_PUINT32 | (len << 8), {.pu32 = arr}};
}

// Function helpers: encode repeat count in upper bytes of type
inline JsonItem Function(const char* key, JsonGeneratorFunc func) {
  return {key, J_FUNCTION, {.func = func}};
}
inline JsonItem Function(const char* key, JsonGeneratorFunc func, uint32_t count) {
  return {key, J_FUNCTION | (count << 8), {.func = func}};
}
inline JsonItem Function(JsonGeneratorFunc func, uint32_t count) {
  return {nullptr, J_FUNCTION | (count << 8), {.func = func}};
}
}  // namespace Json

namespace Json {
// Helper traits to calculate final total items at compile time
template <typename T>
struct ItemCount {
  static constexpr size_t value = 1;
};
template <size_t N>
struct ItemCount<std::array<JsonItem, N>> {
  static constexpr size_t value = N;
};

template <typename... Args>
constexpr auto MakePayload(Args... args) {
  constexpr size_t total = (0 + ... + ItemCount<Args>::value);
  std::array<JsonItem, total> result{};
  size_t idx = 0;

  // Fold expression helper lambda to copy elements into the flat array
  auto append = [&](auto item) {
    if constexpr (std::is_same_v<decltype(item), JsonItem>) {
      result[idx++] = item;
    } else {  // It's a std::array from KeyVal or Val
      for (const auto& i : item) {
        result[idx++] = i;
      }
    }
  };

  (append(args), ...);  // Expand and execute for all arguments
  return result;
}
}  // namespace Json

// Output stages
enum {
  S_COMMA,  // Add a preceding comma if needed
  S_KEY_O,  // Open a quote for the key
  S_KEY_S,  // Append key string
  S_KEY_E,  // Escaping a special char in the key
  S_KEY_C,  // Close the key quote
  S_COLON,  // Add a colon after the key
  S_VAL_O,  // Open a quote for the value if needed
  S_VAL_S,  // Append value string or number
  S_VAL_E,  // Escaping a special char in the value
  S_NEXT    // Move to the next item
};

static bool escape_char(JsonState* state, char c) {
  if (c > 126 || c < ' ' || c == '\\' || c == '"') {
    state->escape_offset = 0;
    if (c == '"') {
      state->val_buf[0] = '\"';
      state->val_buf[1] = '\0';
    } else {
      sprintf(state->val_buf, "u%04x", (unsigned char)c);
    }
    return true;
  }
  return false;
}

size_t json_serialize(JsonState* state, char* out, size_t out_max) {
  size_t written = 0;

  while (written < out_max && state->seg_idx < state->seg_count) {
    const JsonItem* item;
    if (state->in_func) {
      item = &state->func_segment[state->item_idx];
      if (item->type == J_END) {
        state->in_func = 0;
        state->item_idx = state->saved_item_idx;
        continue;
      }
    } else {
      item = &state->segments[state->seg_idx][state->item_idx];
    }

    char c = 0;

    if (item->type == J_END) {
      state->seg_idx++;
      state->item_idx = 0;
      continue;
    } else if (item->type == J_SKIP) {
      state->item_idx++;
      continue;
    } else if ((item->type & 0x7F) == J_FUNCTION) {
      uint32_t count = item->type >> 8;
      if (count > 0 && state->func_idx >= count) {
        // We've called it enough times, move on to the next item
        state->func_idx = 0;
        state->item_idx++;
        continue;
      }
      state->func_segment = item->val.func(state->func_idx++);
      if (!state->func_segment) {
        state->func_idx = 0;
        state->item_idx++;
        continue;
      }
      state->in_func = 1;
      state->saved_item_idx = (count > 0) ? state->item_idx : state->item_idx + 1;
      state->item_idx = 0;
      continue;
    }

    switch (state->stage) {
      case S_COMMA: {
        uint32_t cur = item->type & 0x7F;

        bool is_null = ((item->val.pstr == nullptr && (cur == J_STR || cur == J_PUINT8 || cur == J_PUINT16 ||
                                                       cur == J_PUINT32 || cur == J_PFLOAT || cur == J_PBOOL)) ||
                        (cur == J_PSTR && *item->val.pstr == nullptr));

        if (is_null) {
          // Value is null, skip this and all the subsequent J_MORE items
          while (item->type & J_MORE) {
            state->item_idx++;
            item = &state->segments[state->seg_idx][state->item_idx];
          }
          // Skip the last one
          state->item_idx++;
          continue;
        } else if (state->has_prev && (state->prev_type & J_MORE)) {
          state->stage = S_VAL_O;
          continue;  // Jump straight to value appending
        }

        state->stage = S_KEY_O;
        if (state->has_prev) {
          uint32_t p = state->prev_type & 0x7F;
          if (p != J_OBJ_START && p != J_LIST_START && cur != J_OBJ_END && cur != J_LIST_END) {
            c = ',';
            break;
          }
        }
      }
      // fall through
      case S_KEY_O:
        if (item->key) {
          c = '"';
          state->stage = S_KEY_S;
          state->offset = 0;
          break;
        }
        state->stage = S_VAL_O;
        continue;
      case S_KEY_E:
        c = state->val_buf[state->escape_offset];
        if (c) {
          state->escape_offset++;
          break;
        }
        state->stage = S_KEY_S;
        // fall through
      case S_KEY_S:
        c = item->key[state->offset];
        if (c) {
          if (escape_char(state, c)) {
            c = '\\';
            state->stage = S_KEY_E;
          }
          state->offset++;
          break;
        }
        state->stage = S_KEY_C;
        // fall through
      case S_KEY_C:
        c = '"';
        state->stage = S_COLON;
        break;
      case S_COLON:
        c = ':';
        state->stage = S_VAL_O;
        break;

      case S_VAL_O: {
        state->offset = 0;
        uint32_t t = item->type & 0x7F;
        if (t <= J_LIST_END) {
          c = (t == J_OBJ_START) ? '{' : (t == J_OBJ_END) ? '}' : (t == J_LIST_START) ? '[' : ']';
          state->stage = S_NEXT;
          break;
        }
        // Pre-render numbers if needed before deciding quote placements
        if (t != J_STR && t != J_PSTR && t != J_CHAR4) {
          if (t == J_FLOAT || t == J_PFLOAT) {
            sprintf(state->val_buf, "%g",
                    (t == J_FLOAT) ? (double)item->val.f : (double)item->val.pf[state->array_idx]);
          } else if (t == J_BOOL || t == J_PBOOL) {
            bool v = (t == J_BOOL) ? (item->val.u32 != 0) : (item->val.pbool[state->array_idx] != 0);
            sprintf(state->val_buf, "%s", v ? "true" : "false");
          } else {
            uint32_t v = (t == J_UINT32)    ? item->val.u32
                         : (t == J_PUINT8)  ? item->val.pu8[state->array_idx]
                         : (t == J_PUINT16) ? item->val.pu16[state->array_idx]
                                            : item->val.pu32[state->array_idx];
            sprintf(state->val_buf, "%u", v);
            //printf("<load %u>", v);
          }
        }
        // Open a quote only if this is the absolute beginning of a value chain
        if (!(state->prev_type & J_MORE)) {
          state->in_quote = (t == J_STR || t == J_PSTR || t == J_CHAR4 || (item->type & J_MORE));
          if (state->in_quote) {
            c = '"';
            state->stage = S_VAL_S;
            break;
          }
        }
        state->stage = S_VAL_S;
      }
      // fall through
      case S_VAL_S: {
        uint32_t t2 = item->type & 0x7F;
        if (t2 == J_STR || t2 == J_PSTR) {
          // do {
          //     c = item->val.str[state->offset++];
          //     if(escape_char(state, c)) {
          //         out[written++] = '\\';
          //         state->stage = S_VAL_E;
          //         break;
          //     }
          //     out[written++] = c;
          // } while (c && written < out_max);
          const char* ptr = (t2 == J_STR) ? item->val.str : *item->val.pstr;
          c = ptr != nullptr ? ptr[state->offset] : 0;
          if (c) {
            if (escape_char(state, c)) {
              c = '\\';
              state->stage = S_VAL_E;
            }
            state->offset++;
            break;
          }
        } else if (t2 == J_CHAR4) {
          c = (state->offset < 4) ? item->val.c4[state->offset] : 0;
          if (c) {
            state->offset++;
            break;
          }
        } else {
          c = state->val_buf[state->offset];
          if (c) {
            state->offset++;
            break;
          }
        }
        // Current chunk exhausted. If more are expected, skip closing quote.
        if (item->type & J_MORE) {
          state->stage = S_NEXT;
          continue;
        }

        size_t array_len = (item->type >> 8);
        if (array_len > 0 && state->array_idx + 1 < array_len) {
          state->array_idx++;
          state->offset = 0;
          state->stage = S_VAL_O;
          c = ',';
          break;
        }
        state->array_idx = 0;

        if (state->in_quote) {
          c = '"';
          state->in_quote = 0;
          state->stage = S_NEXT;
          break;
        }
        state->stage = S_NEXT;
      }
      // fall through
      case S_NEXT:
        state->prev_type = item->type;
        state->has_prev = 1;
        state->item_idx++;
        state->stage = S_COMMA;
        state->offset = 0;
        continue;
      case S_VAL_E:
        c = state->val_buf[state->escape_offset];
        if (c) {
          state->escape_offset++;
          break;
        }
        state->stage = S_VAL_S;
        continue;
    }
    if (c)
      out[written++] = c;
  }
  return written;
}

/* Example usage:

uint32_t func_val = 0;
static auto func_items = Json::MakePayload(
    Json::KeyVal("func_val", &func_val),
    Json::KeyVal("func_str", "Hello from function"),
    Json::End()
);

const JsonItem* test_func(size_t index) {
    func_val++;
    return func_items.data();
}

uint16_t user_id = 7044;
float live_temp = 36.6f;
float temps[3] = { 36.6f, 36.7f, 36.8f };

// Compiler automatically expands and builds a flat std::array<JsonItem, 9>
auto payload = Json::MakePayload(
    Json::ObjStart(),
        // Concatenation: String literal -> Pointer -> String literal
        Json::KeyVal("sessio\\n_id", "I\\D_", &user_id, "_ACT\"IVE"), 
        
        // Standard single value pairs (still uses KeyVal!)
        Json::KeyVal("temp", &live_temp),
        Json::KeyVal("temps", Json::ListStart()),
            Json::Array(temps),
        Json::ListEnd(),
        Json::KeyVal("code", 200),
        Json::KeyVal("success", true),

        Json::Function("func", test_func, 3),

        Json::KeyVal("history", Json::ListStart()),
        Json::End()

); auto payload2 = Json::MakePayload(

            // Concatenation inside a list
            Json::Val("LOG_", &user_id), 
        Json::ListEnd(),
    Json::ObjEnd(),
    Json::End()
);


int main(void) {
    // Scatter-gather list of segments
    const JsonItem *segments[] = { payload.data(), payload2.data() };

    user_id = 1;
    live_temp = 0.1f;
    payload2[0].key = "moo";

    JsonState json_state = { .segments = segments, .seg_count = 2 };

    char chunk[32];
    int n;
    while ((n = json_serialize(&json_state, chunk, sizeof(chunk))) > 0) {
        printf("%.*s", n, chunk);
    }
    printf("\n");

    return 0;
}
*/
