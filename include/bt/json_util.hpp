#pragma once
// Strict JSON config helpers: unknown keys and wrong types are errors, never silently ignored.
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"

namespace bt {

struct ConfigError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

namespace cfg {

using json = nlohmann::json;

inline const char* type_name(const json& j) {
  if (j.is_object()) return "object";
  if (j.is_array()) return "array";
  if (j.is_string()) return "string";
  if (j.is_boolean()) return "boolean";
  if (j.is_number()) return "number";
  return "null";
}

inline std::string join(const std::string& path, const char* key) { return path.empty() ? key : path + "." + key; }

inline void expect_object(const json& j, const std::string& path) {
  if (!j.is_object())
    throw ConfigError((path.empty() ? "config" : path) + ": expected an object, got " + type_name(j));
}

inline void check_keys(const json& j, std::initializer_list<const char*> allowed, const std::string& path) {
  expect_object(j, path);
  for (auto it = j.begin(); it != j.end(); ++it) {
    bool ok = false;
    for (const char* a : allowed)
      if (it.key() == a) ok = true;
    if (!ok) throw ConfigError(join(path, it.key().c_str()) + ": unknown key");
  }
}

// Optional number with a default; must lie in [min_v, max_v].
inline double get_num(const json& j, const char* key, double def, const std::string& path, double min_v = 0.0,
                      double max_v = std::numeric_limits<double>::infinity()) {
  const auto it = j.find(key);
  if (it == j.end()) return def;
  if (!it->is_number()) throw ConfigError(join(path, key) + ": expected a number, got " + type_name(*it));
  const double v = it->get<double>();
  if (!std::isfinite(v) || v < min_v || v > max_v)
    throw ConfigError(join(path, key) + ": value " + std::to_string(v) + " out of allowed range");
  return v;
}

inline std::string get_str(const json& j, const char* key, const std::string& def, const std::string& path) {
  const auto it = j.find(key);
  if (it == j.end()) return def;
  if (!it->is_string()) throw ConfigError(join(path, key) + ": expected a string, got " + type_name(*it));
  return it->get<std::string>();
}

// Optional INTEGER (a JSON number written without a fraction/exponent, so 14 is accepted and 14.0 is not).
inline long long get_int(const json& j, const char* key, long long def, const std::string& path, long long min_v,
                         long long max_v) {
  const auto it = j.find(key);
  if (it == j.end()) return def;
  if (!it->is_number_integer()) throw ConfigError(join(path, key) + ": expected an integer, got " + type_name(*it));
  const long long v = it->get<long long>();
  if (v < min_v || v > max_v)
    throw ConfigError(join(path, key) + ": value " + std::to_string(v) + " out of allowed range [" +
                      std::to_string(min_v) + ", " + std::to_string(max_v) + "]");
  return v;
}

// Optional array of non-empty strings (empty vector if absent).
inline std::vector<std::string> get_str_list(const json& j, const char* key, const std::string& path) {
  std::vector<std::string> out;
  const auto it = j.find(key);
  if (it == j.end()) return out;
  if (!it->is_array()) throw ConfigError(join(path, key) + ": expected an array of strings, got " + type_name(*it));
  for (const auto& e : *it) {
    if (!e.is_string() || e.get<std::string>().empty())
      throw ConfigError(join(path, key) + ": expected an array of non-empty strings");
    out.push_back(e.get<std::string>());
  }
  return out;
}

inline bool get_bool(const json& j, const char* key, bool def, const std::string& path) {
  const auto it = j.find(key);
  if (it == j.end()) return def;
  if (!it->is_boolean()) throw ConfigError(join(path, key) + ": expected true/false, got " + type_name(*it));
  return it->get<bool>();
}

}  // namespace cfg
}  // namespace bt
