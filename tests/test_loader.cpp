#include <limits>
#include <string>

#include "bt/csv.hpp"
#include "doctest.h"

using namespace bt;

namespace {

const std::string H = "Date,Open,High,Low,Close,Adj Close,Volume\n";

LoadResult load(const std::string& body, NullPolicy p = NullPolicy::Fail) {
  LoadOptions o;
  o.null_policy = p;
  return load_csv_string(H + body, o);
}

// Asserts the load failed with exactly this code on exactly this 1-based file line.
void expect_error(const LoadResult& r, DataError code, int line) {
  REQUIRE_FALSE(r.ok());
  CHECK(std::string(to_string(r.error->code)) == to_string(code));
  CHECK(r.error->line == line);
  CHECK(r.bars.empty());
}

}  // namespace

TEST_CASE("loader: happy path") {
  const auto r = load(
      "2024-01-01,100,105,99,104,103.5,1000\n"
      "2024-01-02,104,106,103,105,104.5,2000\n");
  REQUIRE(r.ok());
  REQUIRE(r.bars.size() == 2);
  CHECK(r.warnings.empty());
  const Bar& b = r.bars[0];
  CHECK(b.date == parse_date("2024-01-01").value());
  CHECK(b.open == 100.0);
  CHECK(b.high == 105.0);
  CHECK(b.low == 99.0);
  CHECK(b.close == 104.0);
  CHECK(b.adj_close == 103.5);
  CHECK(b.volume == 1000);
  CHECK(r.bars[1].date == b.date + 1);
}

TEST_CASE("loader: CRLF, UTF-8 BOM, blank lines, spaces around fields") {
  const std::string text =
      "\xEF\xBB\xBF" "Date,Open,High,Low,Close,Adj Close,Volume\r\n"
      "2024-01-01, 100 ,105,99,104,103.5,1000\r\n"
      "\r\n"
      "2024-01-02,104,106,103,105,104.5,2000\r\n"
      "\r\n\r\n";
  const auto r = load_csv_string(text);
  REQUIRE(r.ok());
  REQUIRE(r.bars.size() == 2);
  CHECK(r.bars[0].open == 100.0);
}

TEST_CASE("loader: index data with zero volume is valid") {
  const auto r = load("2024-01-01,21000.5,21100,20900,21050,21050,0\n");
  REQUIRE(r.ok());
  CHECK(r.bars[0].volume == 0);
}

TEST_CASE("loader: header errors (line 1)") {
  expect_error(load_csv_string(""), DataError::BadHeader, 1);
  expect_error(load_csv_string("Date,Open,High,Low,Close,Volume\n2024-01-01,1,1,1,1,1\n"), DataError::BadHeader, 1);
  expect_error(load_csv_string("Date,Open,High,Low,Close,AdjClose,Volume\n"), DataError::BadHeader, 1);
  expect_error(load_csv_string("date,open,high,low,close,adj close,volume\n"), DataError::BadHeader, 1);
}

TEST_CASE("loader: header only -> NoData") {
  const auto r = load_csv_string(H);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error->code == DataError::NoData);
}

TEST_CASE("loader: wrong field count") {
  expect_error(load("2024-01-01,100,105,99,104,103.5,1000\n2024-01-02,104,106,103,105,104.5\n"),
               DataError::BadFieldCount, 3);
  expect_error(load("2024-01-01,100,105,99,104,103.5,1000,7\n"), DataError::BadFieldCount, 2);
}

TEST_CASE("loader: bad dates") {
  expect_error(load("2024-02-30,100,105,99,104,103.5,1000\n"), DataError::BadDate, 2);
  expect_error(load("01/02/2024,100,105,99,104,103.5,1000\n"), DataError::BadDate, 2);
  expect_error(load(",100,105,99,104,103.5,1000\n"), DataError::BadDate, 2);
}

TEST_CASE("loader: non-numeric text is always a hard error, even with NullPolicy::Skip") {
  for (NullPolicy p : {NullPolicy::Fail, NullPolicy::Skip}) {
    expect_error(load("2024-01-01,abc,105,99,104,103.5,1000\n", p), DataError::BadNumber, 2);
    expect_error(load("2024-01-01,100,105,99,12x,103.5,1000\n", p), DataError::BadNumber, 2);
    expect_error(load("2024-01-01,100,inf,99,104,103.5,1000\n", p), DataError::BadNumber, 2);
    expect_error(load("2024-01-01,100,105,99,104,103.5,1.5\n", p), DataError::BadNumber, 2);  // volume must be integer
    expect_error(load("2024-01-01,100,105,99,104,103.5,12abc\n", p), DataError::BadNumber, 2);
    expect_error(load("2024-01-01,1e999,105,99,104,103.5,1000\n", p), DataError::BadNumber, 2);  // out of range
  }
}

TEST_CASE("loader: null rows, policy Fail") {
  expect_error(load("2024-01-01,null,null,null,null,null,null\n"), DataError::NullField, 2);
  expect_error(load("2024-01-01,100,105,99,104,103.5,1000\n2024-01-02,NaN,105,99,104,103.5,1000\n"),
               DataError::NullField, 3);
  expect_error(load("2024-01-01,100,105,99,104,,1000\n"), DataError::NullField, 2);       // empty field
  expect_error(load("2024-01-01,100,105,99,104,103.5,NULL\n"), DataError::NullField, 2);  // case-insensitive
}

TEST_CASE("loader: null rows, policy Skip -> warnings with line numbers") {
  const auto r = load(
      "2024-01-01,100,105,99,104,103.5,1000\n"      // line 2
      "2024-01-02,null,null,null,null,null,null\n"  // line 3 (skipped)
      "\n"                                           // line 4 (blank)
      "2024-01-03,104,106,103,105,104.5,2000\n"     // line 5
      "2024-01-04,,,,,,\n",                          // line 6 (skipped)
      NullPolicy::Skip);
  REQUIRE(r.ok());
  REQUIRE(r.bars.size() == 2);
  REQUIRE(r.warnings.size() == 2);
  CHECK(r.warnings[0].line == 3);
  CHECK(r.warnings[1].line == 6);
}

TEST_CASE("loader: a skipped null row does not take part in date-order checks") {
  // The null row is dated BEFORE the previous accepted row; skipping it must not raise an order error.
  const auto r = load(
      "2024-01-05,100,105,99,104,103.5,1000\n"
      "2024-01-02,null,null,null,null,null,null\n"
      "2024-01-08,104,106,103,105,104.5,2000\n",
      NullPolicy::Skip);
  REQUIRE(r.ok());
  CHECK(r.bars.size() == 2);
  // ...and the next accepted row is compared with the last ACCEPTED one (Jan 5), not the skipped one.
  expect_error(load("2024-01-05,100,105,99,104,103.5,1000\n"
                    "2024-01-09,null,null,null,null,null,null\n"
                    "2024-01-04,104,106,103,105,104.5,2000\n",
                    NullPolicy::Skip),
               DataError::NonIncreasingDate, 4);
}

TEST_CASE("loader: zero and negative prices") {
  expect_error(load("2024-01-01,0,105,99,104,103.5,1000\n"), DataError::NonPositivePrice, 2);
  expect_error(load("2024-01-01,100,105,99,-1,103.5,1000\n"), DataError::NonPositivePrice, 2);
  expect_error(load("2024-01-01,100,105,99,104,0,1000\n"), DataError::NonPositivePrice, 2);  // adj_close
}

TEST_CASE("loader: volume") {
  expect_error(load("2024-01-01,100,105,99,104,103.5,-5\n"), DataError::NegativeVolume, 2);
}

TEST_CASE("loader: high < low (checked before open/close range)") {
  expect_error(load("2024-01-01,100,98,99,99,99,1000\n"), DataError::HighBelowLow, 2);
}

TEST_CASE("loader: open/close outside [low, high]") {
  expect_error(load("2024-01-01,106,105,99,104,104,1000\n"), DataError::OpenOutsideRange, 2);  // open > high
  expect_error(load("2024-01-01,98,105,99,104,104,1000\n"), DataError::OpenOutsideRange, 2);   // open < low
  expect_error(load("2024-01-01,100,105,99,106,106,1000\n"), DataError::CloseOutsideRange, 2);  // close > high
  expect_error(load("2024-01-01,100,105,99,98,98,1000\n"), DataError::CloseOutsideRange, 2);    // close < low
  // Boundaries are inclusive.
  CHECK(load("2024-01-01,99,105,99,105,105,1000\n").ok());
}

TEST_CASE("loader: duplicate vs non-increasing dates are different errors") {
  expect_error(load("2024-01-02,100,105,99,104,103.5,1000\n2024-01-02,100,105,99,104,103.5,1000\n"),
               DataError::DuplicateDate, 3);
  expect_error(load("2024-01-02,100,105,99,104,103.5,1000\n2024-01-01,100,105,99,104,103.5,1000\n"),
               DataError::NonIncreasingDate, 3);
  // A repeat that is not adjacent is "earlier than previous", i.e. non-increasing.
  expect_error(load("2024-01-01,100,105,99,104,103.5,1000\n"
                    "2024-01-02,100,105,99,104,103.5,1000\n"
                    "2024-01-01,100,105,99,104,103.5,1000\n"),
               DataError::NonIncreasingDate, 4);
}

TEST_CASE("loader: stops at the first error and reports its line") {
  expect_error(load("2024-01-01,100,105,99,104,103.5,1000\n"
                    "2024-01-02,0,105,99,104,103.5,1000\n"    // line 3: first error
                    "2024-01-03,abc,105,99,104,103.5,1000\n"),  // line 4: would also be an error
               DataError::NonPositivePrice, 3);
}

TEST_CASE("loader: blank lines are counted in line numbers") {
  expect_error(load("2024-01-01,100,105,99,104,103.5,1000\n\n\n2024-01-02,0,105,99,104,103.5,1000\n"),
               DataError::NonPositivePrice, 5);
}

TEST_CASE("loader: missing file") {
  const auto r = load_csv_file("this/file/does/not/exist.csv");
  REQUIRE_FALSE(r.ok());
  CHECK(r.error->code == DataError::FileOpen);
}

TEST_CASE("loader: error text names source and line") {
  const auto r = load("2024-01-01,0,105,99,104,103.5,1000\n");
  REQUIRE_FALSE(r.ok());
  const std::string msg = format_error("x.csv", *r.error);
  CHECK(msg.find("x.csv") != std::string::npos);
  CHECK(msg.find("line 2") != std::string::npos);
}

TEST_CASE("check_bar: programmatic bars") {
  Bar b{1, 100, 105, 99, 104, 104, 10};
  CHECK_FALSE(check_bar(b).has_value());
  b.close = std::numeric_limits<double>::quiet_NaN();
  CHECK(check_bar(b).value() == DataError::BadNumber);
}
