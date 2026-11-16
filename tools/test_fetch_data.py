"""Offline self-test for fetch_data.parse_chart using a hand-made Yahoo-style payload (no network)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # needed because tests run with python -I
import fetch_data  # noqa: E402

# NSE session opens 09:15 IST = 03:45 UTC. gmtoffset 19800 s = +05:30.
#   1704080700 = 2024-01-01 03:45:00 UTC -> local 2024-01-01 09:15
D1 = 1704080700
DAY = 86400


def payload(with_adj=True):
    quote = {
        "open": [100.123456789, None, 103.0, 104.0],
        "high": [105.0, None, 106.0, 107.0],
        "low": [99.0, None, 102.0, 103.0],
        "close": [104.0, None, 105.0, 106.0],
        "volume": [1000, None, 2000, 3000],
    }
    ind = {"quote": [quote]}
    if with_adj:
        ind["adjclose"] = [{"adjclose": [103.5, None, 104.5, 105.5]}]
    # Timestamps: Jan 1, Jan 2 (null row), Jan 3, and Jan 3 AGAIN (duplicate; later one must win).
    return {"chart": {"result": [{"meta": {"gmtoffset": 19800},
                                  "timestamp": [D1, D1 + DAY, D1 + 2 * DAY, D1 + 2 * DAY + 3600],
                                  "indicators": ind}], "error": None}}


class ParseChart(unittest.TestCase):
    def test_rows_dates_nulls_and_dedup(self):
        rows = fetch_data.parse_chart(payload(), "T")
        self.assertEqual(rows, [
            "2024-01-01,100.123457,105.0,99.0,104.0,103.5,1000",
            "2024-01-02,null,null,null,null,null,null",
            "2024-01-03,104.0,107.0,103.0,106.0,105.5,3000",
        ])

    def test_missing_adjclose_falls_back_to_close_with_warning(self):
        warnings = []
        rows = fetch_data.parse_chart(payload(with_adj=False), "T", warn=warnings.append)
        self.assertEqual(rows[0], "2024-01-01,100.123457,105.0,99.0,104.0,104.0,1000")
        self.assertEqual(len(warnings), 1)

    def test_timezone_offset_changes_the_date(self):
        # 2024-01-01 20:00 UTC is already 2024-01-02 01:30 in IST.
        p = payload()
        p["chart"]["result"][0]["timestamp"] = [D1 + 16 * 3600 + 900]
        for k in ("open", "high", "low", "close", "volume"):
            p["chart"]["result"][0]["indicators"]["quote"][0][k] = [1]
        p["chart"]["result"][0]["indicators"]["adjclose"][0]["adjclose"] = [1]
        self.assertTrue(fetch_data.parse_chart(p, "T")[0].startswith("2024-01-02,"))

    def test_error_payloads_raise(self):
        with self.assertRaises(fetch_data.FetchError):
            fetch_data.parse_chart({"chart": {"result": None, "error": {"code": "Not Found"}}}, "T")
        with self.assertRaises(fetch_data.FetchError):
            fetch_data.parse_chart({"chart": {"result": [], "error": None}}, "T")

    def test_partial_bar_rule(self):
        import datetime as dt
        rows = ["2026-10-27,1,1,1,1,1,1", "2026-10-28,2,2,2,2,2,2"]
        # 13:18 IST on 2026-10-28 == 07:48 UTC -> before close: today's row is dropped.
        kept, dropped = fetch_data.drop_partial_today(rows, dt.datetime(2026, 10, 28, 7, 48))
        self.assertEqual((kept, dropped), (rows[:1], "2026-10-28"))
        # 15:29 IST (09:59 UTC) still drops; 15:30 IST (10:00 UTC) keeps.
        self.assertEqual(fetch_data.drop_partial_today(rows, dt.datetime(2026, 10, 28, 9, 59))[1], "2026-10-28")
        self.assertEqual(fetch_data.drop_partial_today(rows, dt.datetime(2026, 10, 28, 10, 0)), (rows, None))
        # The IST date matters, not the UTC date: 20:00 UTC on the 27th is 01:30 IST on the 28th.
        kept, dropped = fetch_data.drop_partial_today(rows, dt.datetime(2026, 10, 27, 20, 0))
        self.assertEqual((kept, dropped), (rows[:1], "2026-10-28"))
        # Earlier in the day than the last row's date: nothing dated "today" -> nothing dropped.
        self.assertEqual(fetch_data.drop_partial_today(rows[:1], dt.datetime(2026, 10, 28, 7, 48)), (rows[:1], None))
        # Same rule applies on any day: at 13:18 IST on the 27th, the row dated the 27th is partial.
        self.assertEqual(fetch_data.drop_partial_today(rows[:1], dt.datetime(2026, 10, 27, 7, 48))[1], "2026-10-27")

    def test_default_symbols_include_the_benchmark_etf_and_the_index(self):
        self.assertIn("NIFTYBEES.NS", fetch_data.DEFAULT_SYMBOLS)
        self.assertIn("^NSEI", fetch_data.DEFAULT_SYMBOLS)
        self.assertEqual(len(fetch_data.DEFAULT_SYMBOLS), len(set(fetch_data.DEFAULT_SYMBOLS)))
        self.assertEqual(fetch_data.file_name("NIFTYBEES.NS"), "NIFTYBEES.NS.csv")

    def test_file_names(self):
        self.assertEqual(fetch_data.file_name("^NSEI"), "NSEI.csv")
        self.assertEqual(fetch_data.file_name("TCS.NS"), "TCS.NS.csv")


if __name__ == "__main__":
    unittest.main()
