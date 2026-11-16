#include "bt/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <utility>

#include "bt/fill.hpp"

namespace bt {

// ============================== Context ==============================

Date Context::date() const {
  if (phase_ == Phase::Start) throw std::logic_error("Context::date: no current day during on_start");
  return data_->date(t_);
}

std::size_t Context::day_index() const {
  if (phase_ == Phase::Start) throw std::logic_error("Context::day_index: no current day during on_start");
  return t_;
}

HistoryView Context::history(std::size_t s) const {
  if (s >= data_->num_symbols()) throw std::out_of_range("Context::history: bad symbol index");
  if (phase_ == Phase::Start) return HistoryView(&data_->bars(s), 0);
  return HistoryView(&data_->bars(s), data_->num_visible(s, t_));
}

bool Context::traded_today(std::size_t s) const {
  if (phase_ == Phase::Start) return false;
  return data_->bar(s, t_) != nullptr;
}

OrderId Context::submit(std::size_t s, Side side, OrderType type, std::int64_t qty, double price,
                        TimeInForce tif) {
  if (phase_ != Phase::Bar) throw std::logic_error("orders can only be submitted from on_bar");
  if (s >= data_->num_symbols()) throw std::invalid_argument("order: bad symbol index");
  if (qty <= 0) throw std::invalid_argument("order: qty must be > 0");
  Paise p = 0;
  if (type != OrderType::Market) {
    if (!std::isfinite(price) || price <= 0.0) throw std::invalid_argument("order: price must be a positive number");
    p = to_paise(price);
    if (p <= 0) throw std::invalid_argument("order: price rounds to zero paise");
  }
  Order o;
  o.id = (*next_id_)++;
  o.symbol = s;
  o.side = side;
  o.type = type;
  o.qty = qty;
  o.remaining = qty;
  o.price = p;
  o.tif = tif;
  o.submitted = data_->date(t_);
  staged_->push_back(o);
  return o.id;
}

OrderId Context::market(std::size_t s, Side side, std::int64_t qty, TimeInForce tif) {
  return submit(s, side, OrderType::Market, qty, 0.0, tif);
}
OrderId Context::limit(std::size_t s, Side side, std::int64_t qty, double price, TimeInForce tif) {
  return submit(s, side, OrderType::Limit, qty, price, tif);
}
OrderId Context::stop(std::size_t s, Side side, std::int64_t qty, double price, TimeInForce tif) {
  return submit(s, side, OrderType::Stop, qty, price, tif);
}

bool Context::cancel(OrderId id) {
  if (phase_ != Phase::Bar) throw std::logic_error("orders can only be cancelled from on_bar");
  for (auto* list : {staged_, pending_}) {
    const auto it = std::find_if(list->begin(), list->end(), [id](const Order& o) { return o.id == id; });
    if (it != list->end()) {
      events_->push_back({it->id, data_->date(t_), it->symbol, OrderEventKind::Cancelled, RejectReason::InsufficientCash});
      list->erase(it);
      return true;
    }
  }
  return false;
}

std::size_t Context::cancel_all(std::size_t s) {
  std::vector<OrderId> ids;
  for (const Order& o : open_orders(s)) ids.push_back(o.id);
  for (OrderId id : ids) cancel(id);
  return ids.size();
}

std::vector<Order> Context::open_orders(std::size_t s) const {
  std::vector<Order> out;
  for (const Order& o : *pending_)
    if (o.symbol == s) out.push_back(o);
  for (const Order& o : *staged_)
    if (o.symbol == s) out.push_back(o);
  return out;
}

// ============================== Engine ==============================

namespace {

// Round-trip bookkeeping for one symbol.
struct OpenTrade {
  bool active = false;
  Side direction = Side::Buy;
  Date entry_date = 0;
  std::size_t entry_day = 0;
  std::int64_t opened_qty = 0;
  std::int64_t peak = 0;
  Paise entry_value = 0, exit_value = 0, cash_flow = 0;
  FeeBreakdown fees;
  int fills = 0;
};

int rank_of(OrderType t) {
  switch (t) {
    case OrderType::Market: return 0;
    case OrderType::Stop: return 1;
    case OrderType::Limit: return 2;
  }
  return 3;
}

}  // namespace

EngineResult Engine::run(Strategy& strategy) const {
  const std::size_t n_sym = data_.num_symbols();
  const std::size_t n_days = data_.num_days();

  // Up-front data check. Prices become integer paise inside the engine; a bar whose price rounds to 0 paise
  // (e.g. 0.004) could only produce a zero-price fill, which the ledger would reject with a logic_error in the
  // middle of a run. Fail early, naming the symbol and date, instead.
  for (std::size_t s = 0; s < n_sym; ++s)
    for (const Bar& b : data_.bars(s))
      if (to_paise(b.open) < 1 || to_paise(b.high) < 1 || to_paise(b.low) < 1 || to_paise(b.close) < 1)
        throw std::invalid_argument("Engine: " + data_.symbol(s) + " " + format_date(b.date) +
                                    ": a price rounds to 0 paise (prices below Rs 0.005 are not tradable here)");

  Portfolio pf(n_sym, cfg_.initial_cash);
  std::vector<Order> pending, staged;
  std::vector<Paise> mark(n_sym, 0);  // last known close per symbol, paise
  std::vector<OpenTrade> open_trades(n_sym);
  OrderId next_id = 1;

  EngineResult res;
  res.initial_cash = cfg_.initial_cash;
  for (std::size_t s = 0; s < n_sym; ++s) res.symbols.push_back(data_.symbol(s));
  res.equity_curve.reserve(n_days);

  Context ctx;
  ctx.data_ = &data_;
  ctx.pf_ = &pf;
  ctx.next_id_ = &next_id;
  ctx.pending_ = &pending;
  ctx.staged_ = &staged;
  ctx.events_ = &res.events;

  ctx.phase_ = Phase::Start;
  ctx.equity_ = cfg_.initial_cash;
  strategy.on_start(ctx);

  std::vector<OrderEvent> warmup_events;  // scratch sink: cancellations made during warm-up are not recorded

  for (std::size_t t = 0; t < n_days; ++t) {
    const Date today = data_.date(t);

    // ---- WARM-UP: before eval_start nothing counts (see EngineConfig::eval_start) ----
    if (cfg_.eval_start && today < *cfg_.eval_start) {
      for (std::size_t s = 0; s < n_sym; ++s)
        if (const Bar* b = data_.bar(s, t)) mark[s] = to_paise(b->close);
      std::vector<OrderEvent>* real_events = ctx.events_;
      ctx.events_ = &warmup_events;
      ctx.warmup_ = true;
      ctx.phase_ = Phase::Bar;
      ctx.t_ = t;
      ctx.equity_ = cfg_.initial_cash;
      strategy.on_bar(ctx);
      ctx.warmup_ = false;
      ctx.events_ = real_events;
      res.warmup_orders_discarded += staged.size();
      staged.clear();
      warmup_events.clear();
      next_id = 1;  // order ids restart with the evaluation window
      ++res.warmup_days;
      continue;
    }

    // ---- 1. FILL ----
    if (!pending.empty()) {
      std::vector<std::size_t> order(pending.size());
      std::iota(order.begin(), order.end(), std::size_t{0});
      std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return rank_of(pending[a].type) < rank_of(pending[b].type);  // stable => ascending id within a rank
      });
      std::vector<char> dead(pending.size(), 0);

      for (std::size_t idx : order) {
        Order& o = pending[idx];
        const Bar* bar = data_.bar(o.symbol, t);
        if (bar == nullptr) continue;  // symbol did not trade today: cannot fill
        const std::optional<FillQuote> q = try_fill(o, to_paise_bar(*bar), cfg_.slippage, cfg_.max_volume_participation);
        if (!q) continue;

        const Paise notional = q->price * q->qty;
        const FeeBreakdown fees = compute_fees(cfg_.costs, o.side, notional);
        const std::int64_t held = pf.qty(o.symbol);

        // Broker-side checks, done at fill time against the then-current account.
        std::optional<RejectReason> reject;
        if (o.side == Side::Sell) {
          if (held > 0 && q->qty > held) reject = cfg_.allow_short ? RejectReason::CrossesZero : RejectReason::ShortingDisabled;
          else if (held <= 0 && !cfg_.allow_short) reject = RejectReason::ShortingDisabled;
        } else {
          if (held < 0 && q->qty > -held) reject = RejectReason::CrossesZero;
        }
        if (!reject && pf.cash_after(o.side, notional, fees.total()) < 0) reject = RejectReason::InsufficientCash;

        if (reject) {
          res.events.push_back({o.id, today, o.symbol, OrderEventKind::Rejected, *reject});
          dead[idx] = 1;
          continue;
        }

        pf.apply_fill(o.symbol, o.side, q->qty, q->price, fees.total());
        const std::int64_t after = pf.qty(o.symbol);

        Fill f;
        f.order_id = o.id;
        f.date = today;
        f.day_index = t;
        f.symbol = o.symbol;
        f.side = o.side;
        f.qty = q->qty;
        f.price = q->price;
        f.notional = notional;
        f.fees = fees;
        f.cash_after = pf.cash();
        f.position_after = after;
        res.fills.push_back(f);
        res.fees += fees;

        // Round-trip tracking.
        OpenTrade& ot = open_trades[o.symbol];
        if (held == 0) {
          ot = OpenTrade{};
          ot.active = true;
          ot.direction = o.side;
          ot.entry_date = today;
          ot.entry_day = t;
        }
        if (o.side == ot.direction) {
          ot.entry_value += notional;
          ot.opened_qty += q->qty;
        } else {
          ot.exit_value += notional;
        }
        ot.cash_flow += o.side == Side::Sell ? notional : -notional;
        ot.fees += fees;
        ot.peak = std::max<std::int64_t>(ot.peak, std::llabs(after));
        ++ot.fills;
        if (after == 0) {
          Trade tr;
          tr.symbol = o.symbol;
          tr.direction = ot.direction;
          tr.entry_date = ot.entry_date;
          tr.exit_date = today;
          tr.entry_day = ot.entry_day;
          tr.exit_day = t;
          tr.qty = ot.opened_qty;
          tr.entry_value = ot.entry_value;
          tr.exit_value = ot.exit_value;
          tr.gross_pnl = ot.cash_flow;
          tr.fees = ot.fees;
          tr.net_pnl = ot.cash_flow - ot.fees.total();
          tr.num_fills = ot.fills;
          res.trades.push_back(tr);
          ot = OpenTrade{};
        }

        o.remaining -= q->qty;
        if (o.remaining == 0) dead[idx] = 1;
      }

      // Every order still pending was submitted on an earlier day, so Day orders end here.
      std::vector<Order> keep;
      keep.reserve(pending.size());
      for (std::size_t i = 0; i < pending.size(); ++i) {
        if (dead[i]) continue;
        if (pending[i].tif == TimeInForce::Day) {
          res.events.push_back({pending[i].id, today, pending[i].symbol, OrderEventKind::Expired, RejectReason::InsufficientCash});
          continue;
        }
        keep.push_back(pending[i]);
      }
      pending = std::move(keep);
    }

    // ---- 2. MARK TO MARKET ----
    for (std::size_t s = 0; s < n_sym; ++s)
      if (const Bar* b = data_.bar(s, t)) mark[s] = to_paise(b->close);
    Paise pos_value = 0;
    for (std::size_t s = 0; s < n_sym; ++s)
      if (pf.qty(s) != 0) pos_value += pf.qty(s) * mark[s];
    DailyRecord rec;
    rec.date = today;
    rec.cash = pf.cash();
    rec.positions_value = pos_value;
    rec.equity = pf.cash() + pos_value;
    res.equity_curve.push_back(rec);

    // ---- 3. DECIDE ----
    ctx.phase_ = Phase::Bar;
    ctx.t_ = t;
    ctx.equity_ = rec.equity;
    strategy.on_bar(ctx);

    // ---- 4. COLLECT ----
    for (Order& o : staged) pending.push_back(std::move(o));
    staged.clear();
  }

  ctx.phase_ = Phase::End;
  if (n_days > 0) ctx.t_ = n_days - 1;
  ctx.equity_ = res.equity_curve.empty() ? cfg_.initial_cash : res.equity_curve.back().equity;
  strategy.on_end(ctx);

  res.final_cash = pf.cash();
  res.final_equity = ctx.equity_;
  res.realized_gross = pf.realized_gross();
  res.orders_pending_at_end = pending.size();
  for (std::size_t s = 0; s < n_sym; ++s) {
    if (pf.qty(s) == 0) continue;
    OpenPosition op;
    op.symbol = s;
    op.qty = pf.qty(s);
    op.cost = pf.position(s).cost;
    op.mark = mark[s];
    op.unrealized_gross = pf.unrealized_gross(s, mark[s]);
    res.open_positions.push_back(op);
  }
  return res;
}

}  // namespace bt
