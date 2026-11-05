#include "pjt/tracker.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace pjt {

namespace {

std::size_t type_index(AlertType t) { return static_cast<std::size_t>(t); }

std::string stuck_alert_id(const std::string& payment_id, Stage s) {
    return payment_id + ":stuck:" + std::string(to_string(s));
}

// Order used when choosing which held event to try next: earliest stage first, Failed last.
int sort_key(Stage s) { return s == Stage::Failed ? kStageCount : path_position(s); }

}  // namespace

Tracker::Tracker(EngineConfig config, Sink& sink)
    : config_(std::move(config)), sink_(sink), slowdown_(config_.slowdown) {}

void Tracker::remember(const std::string& event_id) {
    seen_.insert(event_id);
    seen_order_.push_back(event_id);
    while (seen_order_.size() > config_.dedupe_capacity) {
        seen_.erase(seen_order_.front());
        seen_order_.pop_front();
    }
}

void Tracker::process(const Event& e) {
    ++stats_.events;
    if (e.timestamp_ms > now_ms_) advance_to(e.timestamp_ms);

    // 1. Idempotency: an event we've already handled changes nothing.
    if (seen_before(e.event_id)) {
        ++stats_.duplicates;
        sink_.on_event(e, EventStatus::Duplicate, now_ms_);
        auto it = payments_.find(e.payment_id);
        PaymentState* p = it == payments_.end() ? nullptr : &it->second;
        if (!p || !p->duplicate_alerted) {
            if (p) p->duplicate_alerted = true;
            raise(AlertType::Duplicate, e.payment_id + ":duplicate", e.payment_id, p, e.stage,
                  "event " + e.event_id + " arrived more than once; counted once");
        }
        return;
    }
    remember(e.event_id);

    auto& p = payments_[e.payment_id];
    if (p.finished) {  // nothing moves out of Settled or Failed
        ++stats_.stale;
        sink_.on_event(e, EventStatus::Stale, now_ms_);
        return;
    }
    const bool fits = p.started ? can_move(p.stage, e.stage) : e.stage == Stage::Received;
    if (fits) {
        apply(e.payment_id, p, e);
        drain_pending(e.payment_id, p);
    } else if (!p.started || path_position(e.stage) > path_position(p.stage)) {
        hold(e.payment_id, p, e);  // 2. too early: wait briefly for the missing event
    } else {
        ++stats_.stale;
        sink_.on_event(e, EventStatus::Stale, now_ms_);
    }
}

void Tracker::hold(const std::string& id, PaymentState& p, const Event& e) {
    ++stats_.held;
    sink_.on_event(e, EventStatus::Held, now_ms_);
    if (p.pending.empty()) {
        p.pending_since_ms = now_ms_;
        ++p.reorder_gen;
        timers_.push({now_ms_ + config_.reorder_wait_ms, id, TimerKind::Reorder, p.reorder_gen});
    }
    p.pending.push_back(e);
}

void Tracker::apply(const std::string& id, PaymentState& p, const Event& e) {
    const bool first = !p.started;
    const Stage previous = p.stage;
    const std::int64_t previous_ms = p.last_update_ms;

    p.started = true;
    if (first) p.first_amount_minor = e.amount_minor;
    p.stage = e.stage;
    p.amount_minor = e.amount_minor;
    p.currency = e.currency;
    p.last_update_ms = e.timestamp_ms;
    p.history.push_back({e.event_id, e.stage, e.timestamp_ms, e.amount_minor});
    ++stats_.applied;
    sink_.on_event(e, EventStatus::Applied, now_ms_);

    if (p.stuck_open) {  // it moved again, so the stuck alert is no longer true
        p.stuck_open = false;
        sink_.on_alert_cleared(stuck_alert_id(id, previous), now_ms_);
    }
    if (!first && e.amount_minor != p.first_amount_minor && !p.amount_alerted) {
        p.amount_alerted = true;
        raise(AlertType::AmountMismatch, id + ":amount_mismatch", id, &p, e.stage,
              "amount changed from " + std::to_string(p.first_amount_minor) + " to " +
                  std::to_string(e.amount_minor) + " " + e.currency + " (minor units)");
    }
    // Learn how long the previous stage normally takes (only from clean, in-order moves).
    if (!first && !p.invalid && e.stage != Stage::Failed && path_position(e.stage) == path_position(previous) + 1) {
        const auto change = slowdown_.record(previous, e.timestamp_ms - previous_ms);
        const auto& s = slowdown_.stats(previous);
        const std::string alert_id = "stage:" + std::string(to_string(previous)) + ":slowdown:" + std::to_string(s.episodes);
        if (change == SlowdownDetector::Change::Started) {
            raise(AlertType::Slowdown, alert_id, "", nullptr, previous,
                  std::string(to_string(previous)) + " now takes about " + std::to_string(static_cast<long long>(s.recent_ms)) +
                      " ms, normally " + std::to_string(static_cast<long long>(s.normal_ms)) + " ms");
        } else if (change == SlowdownDetector::Change::Cleared) {
            sink_.on_alert_cleared(alert_id, now_ms_);
        }
    }
    if (e.stage == Stage::Failed) {
        raise(AlertType::Failed, id + ":failed", id, &p, previous, "payment failed after " + std::string(to_string(previous)));
    }

    if (is_final(p.stage)) {
        p.finished = true;
        p.finished_at_ms = now_ms_;
        ++p.stuck_gen;  // cancel the stuck timer
        timers_.push({now_ms_ + config_.retention_ms, id, TimerKind::Retention, 0});
    } else {
        schedule_stuck(id, p);
    }
    sink_.on_state({id, p.stage, p.amount_minor, p.currency, p.last_update_ms, p.invalid, p.finished,
                    static_cast<int>(p.history.size())});
}

void Tracker::schedule_stuck(const std::string& id, PaymentState& p) {
    ++p.stuck_gen;
    const auto limit = config_.stuck_after_ms[static_cast<std::size_t>(index_of(p.stage))];
    if (limit > 0) timers_.push({p.last_update_ms + limit, id, TimerKind::Stuck, p.stuck_gen});
}

void Tracker::drain_pending(const std::string& id, PaymentState& p) {
    bool progressed = true;
    while (progressed && !p.pending.empty()) {
        progressed = false;
        std::stable_sort(p.pending.begin(), p.pending.end(),
                         [](const Event& a, const Event& b) { return sort_key(a.stage) < sort_key(b.stage); });
        for (std::size_t i = 0; i < p.pending.size(); ++i) {
            const Event& e = p.pending[i];
            if (p.finished || (p.started && path_position(e.stage) <= path_position(p.stage) && e.stage != Stage::Failed)) {
                // Overtaken by events: it can never apply now.
                ++stats_.stale;
                sink_.on_event(e, EventStatus::Stale, now_ms_);
                p.pending.erase(p.pending.begin() + static_cast<std::ptrdiff_t>(i));
                progressed = true;
                break;
            }
            const bool fits = p.started ? can_move(p.stage, e.stage) : e.stage == Stage::Received;
            if (fits) {
                const Event ready = e;
                p.pending.erase(p.pending.begin() + static_cast<std::ptrdiff_t>(i));
                apply(id, p, ready);
                if (!p.out_of_order_alerted && !p.invalid) {  // after a skipped stage it's not a reorder
                    p.out_of_order_alerted = true;
                    raise(AlertType::OutOfOrder, id + ":out_of_order", id, &p, ready.stage,
                          "event " + ready.event_id + " arrived early; held and applied in order");
                }
                progressed = true;
                break;
            }
        }
    }
    if (p.pending.empty()) ++p.reorder_gen;  // nothing waiting: cancel the timer
}

void Tracker::advance_to(std::int64_t now_ms) {
    while (!timers_.empty() && timers_.top().at <= now_ms) {
        const Timer t = timers_.top();
        timers_.pop();
        if (t.at > now_ms_) now_ms_ = t.at;
        on_timer(t);
    }
    if (now_ms > now_ms_) now_ms_ = now_ms;
}

void Tracker::on_timer(const Timer& t) {
    auto it = payments_.find(t.payment_id);
    if (it == payments_.end()) return;
    PaymentState& p = it->second;
    switch (t.kind) {
        case TimerKind::Retention:
            if (p.finished) payments_.erase(it);  // finished long enough: free the memory
            return;
        case TimerKind::Stuck:
            if (t.gen != p.stuck_gen || p.finished || p.stuck_open) return;
            p.stuck_open = true;
            raise(AlertType::Stuck, stuck_alert_id(t.payment_id, p.stage), t.payment_id, &p, p.stage,
                  "no new event for " + std::to_string(now_ms_ - p.last_update_ms) + " ms in " +
                      std::string(to_string(p.stage)));
            return;
        case TimerKind::Reorder: {
            if (t.gen != p.reorder_gen || p.pending.empty()) return;
            // The missing event never came. Report it, then carry on with what we have.
            const Stage missing = p.started ? next_on_path(p.stage) : Stage::Received;
            p.invalid = true;
            if (!p.invalid_alerted) {
                p.invalid_alerted = true;
                raise(AlertType::InvalidJourney, t.payment_id + ":invalid_journey", t.payment_id, &p, missing,
                      "missing " + std::string(to_string(missing)) + " event after waiting " +
                          std::to_string(config_.reorder_wait_ms) + " ms");
            }
            std::stable_sort(p.pending.begin(), p.pending.end(),
                             [](const Event& a, const Event& b) { return sort_key(a.stage) < sort_key(b.stage); });
            const Event next = p.pending.front();
            p.pending.erase(p.pending.begin());
            if (!p.pending.empty()) {  // anything still waiting gets a fresh wait
                p.pending_since_ms = now_ms_;
                ++p.reorder_gen;
                timers_.push({now_ms_ + config_.reorder_wait_ms, t.payment_id, TimerKind::Reorder, p.reorder_gen});
            }
            apply(t.payment_id, p, next);
            drain_pending(t.payment_id, p);
            return;
        }
    }
}

void Tracker::drain() {
    std::int64_t longest = 0;
    for (auto v : config_.stuck_after_ms) longest = std::max(longest, v);
    // Each round can apply one held event, which can start a new timer, so repeat.
    for (int i = 0; i < kStageCount + 1; ++i) advance_to(now_ms_ + longest + config_.reorder_wait_ms + 1);
}

void Tracker::raise(AlertType type, const std::string& alert_id, const std::string& payment_id,
                    const PaymentState* p, std::optional<Stage> stage, std::string detail) {
    ++stats_.alerts[type_index(type)];
    Alert a;
    a.alert_id = alert_id;
    a.type = type;
    a.payment_id = payment_id;
    a.stage = stage;
    a.raised_at_ms = now_ms_;
    a.detail = std::move(detail);
    if (p) a.history = p->history;
    sink_.on_alert(a);
}

// ---------------------------------------------------------------------------------------
// Snapshots

namespace {

nlohmann::json event_to_json(const Event& e) {
    return {e.event_id, e.payment_id, index_of(e.stage), e.timestamp_ms, e.amount_minor, e.currency};
}

Event event_from_json(const nlohmann::json& j) {
    return {j.at(0).get<std::string>(), j.at(1).get<std::string>(), static_cast<Stage>(j.at(2).get<int>()),
            j.at(3).get<std::int64_t>(), j.at(4).get<std::int64_t>(), j.at(5).get<std::string>()};
}

}  // namespace

std::string Tracker::snapshot_json(const std::string& position) const {
    nlohmann::json j;
    j["version"] = 1;
    j["position"] = position;
    j["now_ms"] = now_ms_;
    auto& pays = j["payments"] = nlohmann::json::object();
    for (const auto& [id, p] : payments_) {
        nlohmann::json h = nlohmann::json::array();
        for (const auto& e : p.history) h.push_back({e.event_id, index_of(e.stage), e.timestamp_ms, e.amount_minor});
        nlohmann::json pend = nlohmann::json::array();
        for (const auto& e : p.pending) pend.push_back(event_to_json(e));
        pays[id] = {{"started", p.started},
                    {"stage", index_of(p.stage)},
                    {"amount", p.amount_minor},
                    {"first_amount", p.first_amount_minor},
                    {"currency", p.currency},
                    {"last_update", p.last_update_ms},
                    {"invalid", p.invalid},
                    {"finished", p.finished},
                    {"finished_at", p.finished_at_ms},
                    {"history", std::move(h)},
                    {"pending", std::move(pend)},
                    {"pending_since", p.pending_since_ms},
                    {"flags", {p.stuck_open, p.amount_alerted, p.duplicate_alerted, p.out_of_order_alerted, p.invalid_alerted}}};
    }
    j["seen"] = seen_order_;
    nlohmann::json slow = nlohmann::json::array();
    for (int i = 0; i < kStageCount; ++i) {
        const auto& s = slowdown_.stats(static_cast<Stage>(i));
        slow.push_back({s.recent_ms, s.normal_ms, s.samples, s.warning, s.episodes});
    }
    j["slowdown"] = std::move(slow);
    j["stats"] = {stats_.events, stats_.applied, stats_.duplicates, stats_.held, stats_.stale, stats_.alerts};
    return j.dump();
}

std::string Tracker::restore_json(const std::string& text) {
    const auto j = nlohmann::json::parse(text);
    if (j.at("version").get<int>() != 1) throw std::runtime_error("snapshot: unsupported version");
    payments_.clear();
    seen_.clear();
    seen_order_.clear();
    timers_ = {};
    now_ms_ = j.at("now_ms").get<std::int64_t>();
    for (const auto& [id, v] : j.at("payments").items()) {
        PaymentState p;
        p.started = v.at("started").get<bool>();
        p.stage = static_cast<Stage>(v.at("stage").get<int>());
        p.amount_minor = v.at("amount").get<std::int64_t>();
        p.first_amount_minor = v.at("first_amount").get<std::int64_t>();
        p.currency = v.at("currency").get<std::string>();
        p.last_update_ms = v.at("last_update").get<std::int64_t>();
        p.invalid = v.at("invalid").get<bool>();
        p.finished = v.at("finished").get<bool>();
        p.finished_at_ms = v.at("finished_at").get<std::int64_t>();
        for (const auto& h : v.at("history")) {
            p.history.push_back({h.at(0).get<std::string>(), static_cast<Stage>(h.at(1).get<int>()),
                                 h.at(2).get<std::int64_t>(), h.at(3).get<std::int64_t>()});
        }
        for (const auto& e : v.at("pending")) p.pending.push_back(event_from_json(e));
        p.pending_since_ms = v.at("pending_since").get<std::int64_t>();
        const auto& f = v.at("flags");
        p.stuck_open = f.at(0).get<bool>();
        p.amount_alerted = f.at(1).get<bool>();
        p.duplicate_alerted = f.at(2).get<bool>();
        p.out_of_order_alerted = f.at(3).get<bool>();
        p.invalid_alerted = f.at(4).get<bool>();
        payments_.emplace(id, std::move(p));
    }
    for (const auto& id : j.at("seen")) remember(id.get<std::string>());
    const auto& slow = j.at("slowdown");
    for (int i = 0; i < kStageCount; ++i) {
        auto& s = slowdown_.mutable_stats(static_cast<Stage>(i));
        const auto& v = slow.at(static_cast<std::size_t>(i));
        s.recent_ms = v.at(0).get<double>();
        s.normal_ms = v.at(1).get<double>();
        s.samples = v.at(2).get<std::int64_t>();
        s.warning = v.at(3).get<bool>();
        s.episodes = v.at(4).get<int>();
    }
    const auto& st = j.at("stats");
    stats_.events = st.at(0).get<std::int64_t>();
    stats_.applied = st.at(1).get<std::int64_t>();
    stats_.duplicates = st.at(2).get<std::int64_t>();
    stats_.held = st.at(3).get<std::int64_t>();
    stats_.stale = st.at(4).get<std::int64_t>();
    stats_.alerts = st.at(5).get<std::array<std::int64_t, 7>>();
    rebuild_timers();
    return j.at("position").get<std::string>();
}

void Tracker::rebuild_timers() {
    for (auto& [id, p] : payments_) {
        if (p.finished) {
            timers_.push({p.finished_at_ms + config_.retention_ms, id, TimerKind::Retention, 0});
            continue;
        }
        if (p.started && !p.stuck_open) schedule_stuck(id, p);
        if (!p.pending.empty()) {
            ++p.reorder_gen;
            timers_.push({p.pending_since_ms + config_.reorder_wait_ms, id, TimerKind::Reorder, p.reorder_gen});
        }
    }
}

}  // namespace pjt
