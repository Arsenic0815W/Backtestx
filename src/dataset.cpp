#include "bt/dataset.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "bt/adjust.hpp"

namespace bt {
namespace {

std::string pct(double x) {
  char b[32];
  std::snprintf(b, sizeof b, "%+.1f%%", x * 100.0);
  return b;
}

std::string num(double x) {
  char b[32];
  std::snprintf(b, sizeof b, "%.6g", x);
  return b;
}

}  // namespace

std::vector<SymbolSeries> load_dataset(const std::vector<DataFile>& files, const DatasetOptions& opt,
                                       std::vector<LoadedFileInfo>* info) {
  std::vector<SymbolSeries> out;
  out.reserve(files.size());
  if (info) info->clear();
  for (const DataFile& f : files) {
    LoadResult r = load_csv_file(f.path, opt.load);
    if (!r.ok()) throw DatasetError(format_error(f.path, *r.error));
    std::vector<Bar> bars = std::move(r.bars);

    LoadedFileInfo fi;
    fi.symbol = f.symbol;
    fi.path = f.path;
    fi.skipped_null_rows = r.warnings.size();

    // ---- exclusions: strict, and done BEFORE the guard and BEFORE price adjustment ----
    std::vector<Date> ex = f.exclude_dates;
    std::sort(ex.begin(), ex.end());
    ex.erase(std::unique(ex.begin(), ex.end()), ex.end());
    for (Date d : ex) {
      const auto it = std::find_if(bars.begin(), bars.end(), [d](const Bar& b) { return b.date == d; });
      if (it == bars.end())
        throw DatasetError(f.path + ": exclude_dates contains " + format_date(d) + ", which is not a bar in this file (typo?)");
      bars.erase(it);
      fi.excluded.push_back(d);
    }
    if (bars.empty()) throw DatasetError(f.path + ": no bars left after exclusions");

    // ---- implausible-move guard on the file's own Close ----
    for (std::size_t i = 1; i < bars.size(); ++i) {
      const double move = bars[i].close / bars[i - 1].close - 1.0;
      if (std::fabs(move) > fi.largest_move) {
        fi.largest_move = std::fabs(move);
        fi.largest_move_date = bars[i].date;
      }
      if (std::fabs(move) > f.max_daily_move)
        throw DatasetError(f.path + ": " + format_date(bars[i].date) + ": close moved " + pct(move) + " (" +
                           num(bars[i - 1].close) + " on " + format_date(bars[i - 1].date) + " -> " + num(bars[i].close) +
                           ") which exceeds max_daily_move " + pct(f.max_daily_move).substr(1) +
                           ". If this is a data glitch, add the bad date(s) to data.exclude_dates; if it is real, raise max_daily_move.");
    }

    fi.bars = bars.size();
    fi.first = bars.front().date;
    fi.last = bars.back().date;
    out.push_back({f.symbol, opt.basis == PriceBasis::Adjusted ? back_adjust(bars) : std::move(bars)});
    if (info) info->push_back(std::move(fi));
  }
  return out;
}

}  // namespace bt
