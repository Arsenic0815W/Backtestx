#pragma once
// The one place a backtest turns CSV files into price series.
//
// DEFAULT = ADJUSTED prices. Reason: Yahoo's Close is split/bonus-adjusted but NOT dividend-adjusted, so on every
// ex-dividend day the raw Close drops by roughly the dividend and indicators see a fake fall (and fake crossovers).
// Adj Close removes that. Cost of the default: an adjusted price is not a price you could have traded at (old prices are
// scaled down by the later dividends), so notionals, share counts and the percentage fees that depend on them are computed
// on adjusted rupees - fees on old trades are slightly understated. `PriceBasis::Raw` restores the file's own Close.
// (Some files, e.g. Yahoo's NIFTYBEES.NS, have Adj Close == Close everywhere: for them the basis changes nothing.)
//
// Order of operations per file:  load + validate  ->  drop excluded dates  ->  implausible-move guard  ->  price basis.
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bt/bar.hpp"
#include "bt/csv.hpp"

namespace bt {

enum class PriceBasis {
  Adjusted,  // back_adjust() every bar by adj_close/close  (DEFAULT)
  Raw,       // the file's O/H/L/C as they are
};

struct DataFile {
  DataFile(Symbol sym, std::string file, std::vector<Date> exclude = {}, double max_move = 0.5)
      : symbol(std::move(sym)), path(std::move(file)), exclude_dates(std::move(exclude)), max_daily_move(max_move) {}
  Symbol symbol;
  std::string path;
  // Bars to drop, for known-bad data (the CSV itself is never edited). Every date MUST be a bar of the loaded file;
  // a date that is not is an error (a typo that silently excludes nothing is the worst outcome). An excluded date simply
  // becomes a gap, which the calendar alignment and the benchmark pairing already handle.
  std::vector<Date> exclude_dates;
  // Loud guard: if |Close / previous Close - 1| exceeds this for any consecutive pair of remaining bars, loading FAILS
  // with the dates and prices, so a glitch cannot flow silently into signals or beta. It only catches large jumps; it
  // cannot see small bad ticks. Use a large value (e.g. 1000) to switch it off.
  double max_daily_move = 0.5;
};

struct DatasetOptions {
  LoadOptions load;                      // null-row policy (default: strict)
  PriceBasis basis = PriceBasis::Adjusted;
};

struct DatasetError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// What was actually loaded from one file (for the run metadata).
struct LoadedFileInfo {
  Symbol symbol;
  std::string path;
  std::size_t bars = 0;                  // bars kept (after exclusions)
  std::size_t skipped_null_rows = 0;
  Date first = 0, last = 0;
  std::vector<Date> excluded;            // dates actually dropped
  double largest_move = 0.0;             // largest |close-to-close| among the kept bars
  Date largest_move_date = 0;
};

// Loads every file (strict validation, line-numbered errors) and applies exclusions, the guard and the price basis.
// Throws DatasetError("path: ...") on the first problem. `info`, if given, receives one entry per file.
std::vector<SymbolSeries> load_dataset(const std::vector<DataFile>& files, const DatasetOptions& opt = {},
                                       std::vector<LoadedFileInfo>* info = nullptr);

}  // namespace bt
