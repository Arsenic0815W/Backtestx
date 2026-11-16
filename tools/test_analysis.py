"""Tests for the scripts in tools/analysis/ on small synthetic data (no real files needed)."""
import contextlib
import datetime as dt
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "analysis"))  # needed because tests run with python -I
import _common as C  # noqa: E402
import data_quality as DQ  # noqa: E402
import niftybees_vs_index as NV  # noqa: E402
import raw_vs_adjusted_signals as RA  # noqa: E402


def bar(d, close, adj=None, vol=100):
    return C.Bar(d, close, close, close, close, close if adj is None else adj, vol)


def weekdays(n, start=dt.date(2020, 1, 6)):
    out, d = [], start
    while len(out) < n:
        if d.weekday() < 5:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


def write_csv(path, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("Date,Open,High,Low,Close,Adj Close,Volume\n")
        for r in rows:
            f.write(r + "\n")


class Detector(unittest.TestCase):
    def series(self):
        days = weekdays(200)
        idx = {d: bar(d, 10000 * (1 + 0.0004 * i + 0.02 * ((i * 7) % 5 - 2) / 100)) for i, d in enumerate(days)}
        etf = {d: bar(d, idx[d].close / 100.0) for d in days}     # tracks the index exactly (ratio constant)
        return days, etf, idx

    def test_clean_series_has_no_suspects(self):
        _, etf, idx = self.series()
        self.assertEqual(NV.suspects(etf, idx, 0.08), [])

    def test_finds_a_single_spike_and_a_two_bar_glitch_exactly(self):
        days, etf, idx = self.series()
        bad = {days[100]: 1.12, days[50]: 0.10, days[51]: 0.10}   # a +12 % spike and a two-bar -90 % glitch
        for d, f in bad.items():
            etf[d] = bar(d, etf[d].close * f)
        found = {d for d, _ in NV.suspects(etf, idx, 0.08)}
        self.assertEqual(found, set(bad))

    def test_the_old_two_bar_neighbourhood_gives_false_positives_next_to_a_two_bar_glitch(self):
        days, etf, idx = self.series()
        for d in (days[50], days[51]):
            etf[d] = bar(d, etf[d].close * 0.10)
        wide = {d for d, _ in NV.suspects(etf, idx, 0.08, half_window=3)}
        narrow = {d for d, _ in NV.suspects(etf, idx, 0.08, half_window=2)}
        self.assertEqual(wide, {days[50], days[51]})
        self.assertTrue(narrow > wide, "with only 2 bars each side the good bars next to the glitch are contaminated")
        self.assertIn(days[49], narrow)
        self.assertIn(days[52], narrow)

    def test_threshold_is_respected(self):
        days, etf, idx = self.series()
        etf[days[80]] = bar(days[80], etf[days[80]].close * 1.05)
        self.assertEqual(NV.suspects(etf, idx, 0.08), [])
        self.assertEqual({d for d, _ in NV.suspects(etf, idx, 0.03)}, {days[80]})


class ProxyStats(unittest.TestCase):
    def test_constant_ratio_gives_zero_tracking_error_and_full_correlation(self):
        days = weekdays(300)
        idx = {d: bar(d, 100 * (1.0004 ** i) * (1 + 0.01 * ((i * 13) % 7 - 3) / 3)) for i, d in enumerate(days)}
        etf = {d: bar(d, idx[d].adj / 40.0) for d in days}
        s = NV.proxy_stats(etf, idx, [])
        self.assertAlmostEqual(s["cagr_gap"], 0.0, places=9)
        self.assertAlmostEqual(s["tracking_error_annualised"], 0.0, places=9)
        self.assertAlmostEqual(s["daily_return_correlation"], 1.0, places=9)
        self.assertEqual(s["days_differing_by_over_3_points"], 0)

    def test_a_steady_drift_shows_up_as_the_cagr_gap(self):
        days = weekdays(1000)
        g = 1.00005                                   # extra growth per calendar day on the ETF
        idx = {d: bar(d, 100 * (1.0003 ** i)) for i, d in enumerate(days)}
        etf = {d: bar(d, idx[d].close * g ** (d - days[0]).days) for d in days}
        s = NV.proxy_stats(etf, idx, [])
        # etf growth = index growth * g^days, so (1 + etf_cagr) = (1 + index_cagr) * g^365.25 and the gap is (1 + index_cagr) * (g^365.25 - 1)
        self.assertAlmostEqual(s["cagr_gap"], (1 + s["index_cagr"]) * (g ** 365.25 - 1), places=9)
        self.assertGreater(s["cagr_gap"], 0.0)

    def test_excluding_dates_removes_them(self):
        days = weekdays(100)
        idx = {d: bar(d, 100.0 + i) for i, d in enumerate(days)}
        etf = {d: bar(d, (100.0 + i) / 10) for i, d in enumerate(days)}
        etf[days[40]] = bar(days[40], 5.0)
        with_bad = NV.proxy_stats(etf, idx, [])
        without = NV.proxy_stats(etf, idx, [days[40]])
        self.assertEqual(with_bad["common_dates"], 100)
        self.assertEqual(without["common_dates"], 99)
        self.assertLess(with_bad["daily_return_correlation"], 0.9)
        self.assertGreater(without["daily_return_correlation"], 0.99)


class RawVsAdjusted(unittest.TestCase):
    def test_a_constant_adjustment_factor_changes_no_signal(self):
        days = weekdays(300)
        bars = [bar(d, 100 + 20 * ((i // 30) % 2) + (i % 5), adj=(100 + 20 * ((i // 30) % 2) + (i % 5)) * 0.8) for i, d in enumerate(days)]
        r = RA.analyse(bars, 5, 20)
        self.assertEqual(r["adjustment_steps"], 0)
        self.assertEqual(r["days_state_differs"], 0)
        self.assertEqual(r["state_changes_raw"], r["state_changes_adjusted"])
        self.assertGreater(r["state_changes_raw"], 2)

    def test_any_difference_requires_a_step_inside_the_window(self):
        days = weekdays(400)
        bars = []
        for i, d in enumerate(days):
            close = 100 + 15 * ((i // 40) % 2) + (i % 7)
            factor = 0.7 if i < 150 else 1.0       # one step (an old-dividend-like jump) at bar 150
            bars.append(bar(d, close, adj=close * factor))
        r = RA.analyse(bars, 5, 20)
        self.assertEqual(r["adjustment_steps"], 1)
        self.assertTrue(r["states_agree_on_every_day_without_a_step_in_the_window"])
        self.assertEqual(r["days_state_differs"], r["of_which_with_a_step_in_the_window"])


class Facts(unittest.TestCase):
    def test_file_facts_counts_nulls_zero_volume_and_steps(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, "X.csv")
            write_csv(p, ["2024-01-02,10,10,10,10,10,100",              # Tuesday
                          "2024-01-03,null,null,null,null,null,null",    # Wednesday: null on a weekday
                          "2024-01-06,null,null,null,null,null,null",    # Saturday: null on a weekend
                          "2024-01-08,11,11,11,11,11,0",                 # zero volume
                          "2024-01-09,12,12,12,12,11,50"])               # Adj/Close steps from 1.0 to 0.9167
            f = DQ.file_facts("X", p)
            self.assertEqual((f["rows"], f["bars"], f["null_rows"]), (5, 3, 2))
            self.assertEqual((f["null_rows_on_weekdays"], f["null_rows_on_weekends"]), (1, 1))
            self.assertEqual(f["null_weekday_dates"], ["2024-01-03"])
            self.assertEqual(f["zero_volume_bars"], 1)
            self.assertEqual(f["adjustment_steps"], 1)
            self.assertFalse(f["adj_equals_close_everywhere"])
            self.assertEqual(f["largest_moves"][0][1], "2024-01-08")


class Skipping(unittest.TestCase):
    def run_require(self, d, names):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            try:
                C.require_files(d, names)
                code = None
            except SystemExit as e:
                code = e.code
        return code, out.getvalue()

    def test_none_present_skips_with_exit_0(self):
        with tempfile.TemporaryDirectory() as t:
            code, text = self.run_require(t, ["A.NS", "^IDX"])
            self.assertEqual(code, 0)
            self.assertIn("SKIPPED", text)
            self.assertIn("IDX.csv", text)      # '^' dropped from the file name

    def test_partly_present_fails_with_exit_1_naming_the_missing_file(self):
        with tempfile.TemporaryDirectory() as t:
            write_csv(os.path.join(t, "A.NS.csv"), ["2024-01-02,1,1,1,1,1,1"])
            code, text = self.run_require(t, ["A.NS", "B.NS"])
            self.assertEqual(code, 1)
            self.assertIn("B.NS.csv", text)

    def test_all_present_returns_the_paths(self):
        with tempfile.TemporaryDirectory() as t:
            write_csv(os.path.join(t, "A.NS.csv"), ["2024-01-02,1,1,1,1,1,1"])
            write_csv(os.path.join(t, "IDX.csv"), ["2024-01-02,1,1,1,1,1,1"])
            paths = C.require_files(t, ["A.NS", "^IDX"])
            self.assertEqual(sorted(paths), ["A.NS", "^IDX"])


class KnownBad(unittest.TestCase):
    def test_known_bad_bars_file_is_well_formed(self):
        kb = C.known_bad_bars()
        self.assertEqual(sorted(kb), ["NIFTYBEES.NS"])
        self.assertEqual(kb["NIFTYBEES.NS"], [dt.date(2010, 2, 5), dt.date(2010, 10, 6), dt.date(2019, 12, 19), dt.date(2019, 12, 20)])


if __name__ == "__main__":
    unittest.main()
