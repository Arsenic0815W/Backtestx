#include <string>

#include "bt/version.hpp"
#include "doctest.h"
#include "json.hpp"

TEST_CASE("hello doctest") {
  CHECK(1 + 1 == 2);
}

TEST_CASE("bt_core links and reports a version") {
  CHECK(std::string(bt::version()) == "0.1.0");
}

TEST_CASE("vendored nlohmann/json parses and round-trips") {
  const auto j = nlohmann::json::parse(R"({"symbol":"TCS.NS","qty":10})");
  CHECK(j.at("symbol").get<std::string>() == "TCS.NS");
  CHECK(j.at("qty").get<int>() == 10);
  CHECK(nlohmann::json::parse(j.dump()) == j);
}
