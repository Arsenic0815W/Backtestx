"""Tests for tools/ref_metrics.py: the Python reference is checked against HAND-COMPUTED values (the same cases as the C++
analytics tests, derived on paper), so it is validated independently of the C++ code it is used to cross-check. Also tests the
comparator itself (it must reject tampering) and the "no scenarios" failure."""
import copy
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # needed because tests run with python -I
import ref_metrics as rm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIFF = os.path.join(ROOT, "build", "diff")
CFG0 = {"risk_free_annual": 0.0, "trading_days_per_year": 252, "calendar_days_per_year": 365.25}


def write_scenario(folder, equity, trades=(), fills=(), bench=None, cfg=CFG0):
    """equity: [(iso_date, equity_paise, positions_value_paise)];  trades: [(entry, exit, entry_day, exit_day, net)];
    fills: [(symbol, notional, fees, position_after)];  bench: [(iso_date, value)]."""
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "equity.csv"), "w") as f:
        f.write("date,cash_paise,positions_value_paise,equity_paise\n")
        for dte, e, p in equity:
            f.write(f"{dte},{e - p},{p},{e}\n")
    with open(os.path.join(folder, "trades.csv"), "w") as f:
        f.write("symbol,direction,entry_date,exit_date,entry_day,exit_day,qty,entry_value_paise,exit_value_paise,"
                "gross_pnl_paise,fees_paise,net_pnl_paise,num_fills\n")
        for en, ex, ed, xd, net in trades:
            f.write(f"X,long,{en},{ex},{ed},{xd},1,0,0,{net},0,{net},2\n")
    with open(os.path.join(folder, "fills.csv"), "w") as f:
        f.write("order_id,date,day_index,symbol,side,qty,price_paise,notional_paise,brokerage_paise,stt_paise,exchange_paise,"
                "sebi_paise,stamp_paise,gst_paise,fees_paise,cash_after_paise,position_after\n")
        for i, (sym, notional, fees, pos) in enumerate(fills, 1):
            f.write(f"{i},2024-01-01,0,{sym},buy,1,{notional},{notional},0,0,0,0,0,0,{fees},0,{pos}\n")
    if bench is not None:
        with open(os.path.join(folder, "benchmark.csv"), "w") as f:
            f.write("date,value\n")
            for dte, v in bench:
                f.write(f"{dte},{v!r}\n")
    with open(os.path.join(folder, "summary.json"), "w") as f:
        json.dump({"config": cfg}, f)


CURVE_A = [("2024-01-01", 10000, 0), ("2024-01-02", 11000, 5000), ("2024-01-03", 9900, 4000),
           ("2024-01-04", 10890, 0), ("2024-12-31", 11979, 0)]  # last date = first + 365 days


class HandComputed(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def near(self, a, b, rel=1e-9):
        self.assertIsNotNone(a)
        self.assertAlmostEqual(a, b, delta=rel * max(1.0, abs(b)))

    def test_equity_metrics_curve_a(self):
        write_scenario(self.tmp, CURVE_A)
        e = rm.compute(self.tmp)["equity"]
        self.assertEqual(e["n_records"], 5)
        self.near(e["total_return"], 0.1979)                    # 11979/10000 - 1
        self.near(e["years"], 365 / 365.25)
        self.near(e["cagr"], 0.19804816315622786)               # 1.1979 ** (365.25/365) - 1
        self.near(e["mean_daily_return"], 0.05)                 # (0.1 - 0.1 + 0.1 + 0.1)/4
        self.near(e["daily_return_sd"], 0.1)
        self.near(e["annualised_volatility"], 0.1 * 252 ** 0.5)
        self.near(e["sharpe"], 0.5 * 252 ** 0.5)                # 0.05/0.1 * sqrt(252)
        self.near(e["sortino"], 252 ** 0.5)                     # downside dev = sqrt(0.1^2/4) = 0.05
        self.near(e["max_drawdown"], -0.1)
        self.assertEqual((e["max_drawdown_peak_date"], e["max_drawdown_trough_date"], e["max_drawdown_recovery_date"]),
                         ("2024-01-02", "2024-01-03", "2024-12-31"))
        self.assertIs(e["max_drawdown_recovered"], True)
        self.assertEqual(e["max_drawdown_duration_days"], 364)
        self.assertEqual(e["longest_underwater_days"], 364)
        self.near(e["calmar"], 1.9804816315622786)
        self.assertEqual((e["best_day_date"], e["worst_day_date"]), ("2024-01-02", "2024-01-03"))  # ties: first wins
        self.near(e["time_in_market"], 0.4)
        self.near(e["avg_exposure"], 17 / 99)

    def test_risk_free_rate(self):
        cfg = dict(CFG0, risk_free_annual=0.065)
        write_scenario(self.tmp, CURVE_A, cfg=cfg)
        e = rm.compute(self.tmp)["equity"]
        self.near(e["sharpe"], 7.896307781951115)
        self.near(e["sortino"], 15.751985442720617)

    def test_nested_peaks_underwater_is_measured_against_the_running_max(self):
        # 100@0, 110@100, 95@101, 108@102, 109@120, 111@150 -> one spell: peak 110 (day 100) .. recovery 111 (day 150) = 50 days
        eq = [("2024-01-01", 100, 0), ("2024-04-10", 110, 0), ("2024-04-11", 95, 0), ("2024-04-12", 108, 0),
              ("2024-04-30", 109, 0), ("2024-05-30", 111, 0)]
        write_scenario(self.tmp, eq)
        e = rm.compute(self.tmp)["equity"]
        self.assertEqual(e["longest_underwater_days"], 50)
        self.assertEqual(e["max_drawdown_duration_days"], 50)

    def test_drawdown_cases(self):
        write_scenario(self.tmp, [("2024-01-01", 10000, 0), ("2024-01-11", 12000, 0), ("2024-01-21", 9000, 0), ("2024-01-31", 9600, 0)])
        e = rm.compute(self.tmp)["equity"]
        self.near(e["max_drawdown"], -0.25)
        self.assertIs(e["max_drawdown_recovered"], False)
        self.assertIsNone(e["max_drawdown_recovery_date"])
        self.assertEqual(e["max_drawdown_duration_days"], 20)   # peak (Jan 11) -> last date (Jan 31)
        # tie: two equal -10 % drawdowns, the first trough is reported; longest spell is the second (6 days)
        eq = [("2024-01-01", 100, 0), ("2024-01-02", 90, 0), ("2024-01-03", 100, 0), ("2024-01-04", 200, 0),
              ("2024-01-05", 180, 0), ("2024-01-10", 200, 0)]
        write_scenario(self.tmp, eq)
        e = rm.compute(self.tmp)["equity"]
        self.assertEqual(e["max_drawdown_trough_date"], "2024-01-02")
        self.assertEqual(e["longest_underwater_days"], 6)
        self.assertEqual(e["max_drawdown_duration_days"], 2)

    def test_edge_cases_are_none_not_nan(self):
        write_scenario(self.tmp, [("2024-01-01", 10000, 0), ("2024-01-02", 10000, 0), ("2024-01-03", 10000, 0)])
        e = rm.compute(self.tmp)["equity"]
        self.assertEqual(e["daily_return_sd"], 0.0)
        for k in ("sharpe", "sortino", "calmar", "max_drawdown_peak_date", "max_drawdown_recovered"):
            self.assertIsNone(e[k], k)
        write_scenario(self.tmp, [])
        e = rm.compute(self.tmp)["equity"]
        self.assertEqual(e["n_records"], 0)
        self.assertIsNone(e["total_return"])
        write_scenario(self.tmp, [("2024-01-01", 10000, 0), ("2024-01-02", 5000, 0), ("2024-01-03", 0, 0)])
        e = rm.compute(self.tmp)["equity"]
        self.near(e["total_return"], -1.0)
        for k in ("cagr", "mean_daily_return", "sharpe", "max_drawdown", "best_day"):
            self.assertIsNone(e[k], k)

    def test_trade_and_fill_metrics(self):
        # net P&L +300 -100 +200 -400 0 ; holding days 5 10 3 20 1 ; bars 3 7 2 14 1
        trades = [("2024-01-01", "2024-01-06", 10, 13, 300), ("2024-01-01", "2024-01-11", 0, 7, -100),
                  ("2024-01-01", "2024-01-04", 5, 7, 200), ("2024-01-01", "2024-01-21", 0, 14, -400),
                  ("2024-01-01", "2024-01-02", 3, 4, 0)]
        write_scenario(self.tmp, CURVE_A, trades=trades)
        t = rm.compute(self.tmp)["trades"]
        self.assertEqual((t["num_trades"], t["num_wins"], t["num_losses"], t["num_breakeven"]), (5, 2, 2, 1))
        self.near(t["win_rate"], 0.4)
        self.near(t["avg_win_paise"], 250.0)
        self.near(t["avg_loss_paise"], -250.0)
        self.near(t["profit_factor"], 1.0)
        self.assertEqual((t["best_trade_paise"], t["worst_trade_paise"]), (300, -400))
        self.near(t["avg_holding_days"], 7.8)
        self.near(t["avg_holding_bars"], 5.4)

    def test_profit_factor_and_empty_trades(self):
        write_scenario(self.tmp, CURVE_A, trades=[("2024-01-01", "2024-01-02", 0, 1, 100)])
        self.assertIsNone(rm.compute(self.tmp)["trades"]["profit_factor"])       # no losses -> infinite -> None
        write_scenario(self.tmp, CURVE_A, trades=[("2024-01-01", "2024-01-02", 0, 1, -100)])
        self.assertEqual(rm.compute(self.tmp)["trades"]["profit_factor"], 0.0)   # no wins
        write_scenario(self.tmp, CURVE_A)
        t = rm.compute(self.tmp)["trades"]
        self.assertEqual(t["num_trades"], 0)
        self.assertIsNone(t["win_rate"])

    def test_turnover_fees_open_positions(self):
        fills = [("X", 10000, 100, 100), ("X", 10800, 50, 0), ("Y", 0, 0, 7)]
        write_scenario(self.tmp, CURVE_A, fills=fills)
        t = rm.compute(self.tmp)["trades"]
        self.assertEqual(t["traded_value_paise"], 20800)
        self.assertEqual(t["total_fees_paise"], 150)
        self.near(t["average_equity_paise"], 10753.8)
        self.near(t["turnover"], 0.9671000018598077)
        self.near(t["turnover_annual"], 0.9677623991213555)
        self.assertEqual((t["net_pnl_paise"], t["gross_pnl_paise"]), (1979, 2129))
        self.near(t["fees_pct_gross_pnl"], 0.07045561296383279)
        self.near(t["fees_pct_initial_capital"], 0.015)
        self.assertEqual(t["num_open_positions"], 1)  # X ends flat, Y ends with 7

    def test_benchmark_pairing_across_a_gap(self):
        eq = [("2024-01-01", 10_000_000, 0), ("2024-01-02", 10_200_000, 0), ("2024-01-03", 10_300_000, 0),
              ("2024-01-04", 10_608_000, 0), ("2024-01-05", 10_395_840, 0), ("2024-01-06", 10_915_632, 0)]
        bench = [("2024-01-01", 100.0), ("2024-01-02", 101.0), ("2024-01-04", 103.02), ("2024-01-05", 101.9898),
                 ("2024-01-06", 104.539545)]   # no bar on Jan 3
        write_scenario(self.tmp, eq, bench=bench)
        b = rm.compute(self.tmp)["benchmark"]
        self.assertEqual((b["n_common_dates"], b["n_pairs"], b["n_curve_dates_without_benchmark"]), (5, 4, 1))
        self.near(b["beta"], 2.0)          # strategy return = exactly 2 x benchmark return on every common interval
        self.near(b["correlation"], 1.0)
        self.assertLess(abs(b["alpha_annual"]), 1e-9)
        self.near(b["strategy_return"], 0.0915632)
        self.near(b["benchmark_return"], 0.04539545)
        write_scenario(self.tmp, eq, bench=bench, cfg=dict(CFG0, risk_free_annual=0.065))
        self.near(rm.compute(self.tmp)["benchmark"]["alpha_annual"], 0.065)   # alpha = rf_d * 252 when ms = 2 mb

    def test_benchmark_undefined_cases(self):
        eq = [("2024-01-01", 100, 0), ("2024-01-02", 101, 0), ("2024-01-03", 103, 0), ("2024-01-04", 102, 0), ("2024-01-05", 104, 0)]
        write_scenario(self.tmp, eq, bench=[(d, 100.0) for d, _, _ in eq])          # zero-variance benchmark
        b = rm.compute(self.tmp)["benchmark"]
        self.assertEqual((b["beta"], b["alpha_annual"], b["correlation"]), (None, None, None))
        self.near(b["benchmark_return"], 0.0)
        write_scenario(self.tmp, eq, bench=[("2024-01-01", 100.0), ("2024-01-03", 102.0), ("2024-01-05", 101.0)])  # 2 pairs
        b = rm.compute(self.tmp)["benchmark"]
        self.assertEqual((b["n_pairs"], b["beta"]), (2, None))
        write_scenario(self.tmp, eq, bench=[("2030-01-01", 1.0), ("2030-01-02", 2.0)])   # no overlap
        b = rm.compute(self.tmp)["benchmark"]
        self.assertEqual((b["n_common_dates"], b["first_common_date"], b["strategy_return"]), (0, None, None))


class Comparator(unittest.TestCase):
    def test_values(self):
        ok = rm.compare_values
        self.assertTrue(ok(1.0, 1.0 + 1e-12)[0])
        self.assertTrue(ok(0.0, 5e-13)[0])            # within the absolute floor
        self.assertFalse(ok(0.0, 5e-11)[0])
        self.assertFalse(ok(1.0, 1.0 + 1e-6)[0])      # relative 1e-6 > 1e-9
        self.assertTrue(ok(1e9, 1e9 * (1 + 1e-10))[0])
        self.assertFalse(ok(7, 8)[0])                 # ints exact
        self.assertFalse(ok(7, 7.0)[0])               # int vs float: type drift is a failure
        self.assertFalse(ok("2024-01-01", "2024-01-02")[0])
        self.assertFalse(ok(True, 1)[0])
        self.assertTrue(ok(None, None)[0])
        self.assertFalse(ok(None, 0.0)[0])
        self.assertFalse(ok(0.0, None)[0])

    def test_key_sets_are_compared_both_ways(self):
        problems, stats = [], {"compared": 0, "max_rel": 0.0}
        rm.compare_section("equity", {"a": 1, "b": 2}, {"a": 1}, problems, stats)
        self.assertTrue(any("only in summary.json" in p for p in problems))
        problems = []
        rm.compare_section("equity", {"a": 1}, {"a": 1, "c": 3}, problems, stats)
        self.assertTrue(any("only in the Python reference" in p for p in problems))
        problems = []
        rm.compare_section("benchmark", None, {"a": 1}, problems, stats)
        self.assertTrue(problems)


class Driver(unittest.TestCase):
    def test_no_scenarios_is_a_failure(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(rm.main(["x", t]), 1)                       # empty folder
            os.makedirs(os.path.join(t, "not_a_scenario"))
            self.assertEqual(rm.main(["x", t]), 1)                       # subfolder without summary.json
        self.assertEqual(rm.main(["x", os.path.join(tempfile.gettempdir(), "no_such_dir_bt")]), 1)

    @unittest.skipUnless(os.path.isdir(DIFF) and any(os.path.isfile(os.path.join(DIFF, x, "summary.json")) for x in os.listdir(DIFF)),
                         "build/diff not present (run the C++ tests first)")
    def test_negative_controls_on_real_exports(self):
        """Take a real exported scenario and tamper with it in several ways: the checker must notice every one."""
        src = os.path.join(DIFF, sorted(x for x in os.listdir(DIFF) if x.startswith("02_"))[0])
        with tempfile.TemporaryDirectory() as t:
            dst = os.path.join(t, "s")
            shutil.copytree(src, dst)
            problems, _ = rm.check_folder(dst)
            self.assertEqual(problems, [], "the untouched export must pass")
            with open(os.path.join(dst, "summary.json")) as f:
                original = json.load(f)

            def tampered(mutate):
                doc = copy.deepcopy(original)
                mutate(doc)
                with open(os.path.join(dst, "summary.json"), "w") as f:
                    json.dump(doc, f)
                return rm.check_folder(dst)[0]

            self.assertTrue(tampered(lambda d: d["equity"].__setitem__("sharpe", d["equity"]["sharpe"] * (1 + 1e-6))))
            self.assertTrue(tampered(lambda d: d["trades"].__setitem__("num_trades", d["trades"]["num_trades"] + 1)))
            self.assertTrue(tampered(lambda d: d["equity"].__setitem__("end_date", "2001-01-01")))
            self.assertTrue(tampered(lambda d: d["equity"].__setitem__("max_drawdown", None)))
            self.assertTrue(tampered(lambda d: d["equity"].pop("calmar")))                     # key missing on the C++ side
            self.assertTrue(tampered(lambda d: d["equity"].__setitem__("extra_metric", 1.0)))  # extra key
            self.assertTrue(tampered(lambda d: d.__setitem__("benchmark", None)))
            # a tampered input file (not the summary) is caught too
            with open(os.path.join(dst, "summary.json"), "w") as f:
                json.dump(original, f)
            with open(os.path.join(dst, "equity.csv")) as f:
                lines = f.read().split("\n")
            fields = lines[5].split(",")
            fields[3] = str(int(fields[3]) + 100)
            lines[5] = ",".join(fields)
            with open(os.path.join(dst, "equity.csv"), "w") as f:
                f.write("\n".join(lines))
            self.assertTrue(rm.check_folder(dst)[0])


if __name__ == "__main__":
    unittest.main()
