#include "debug_observer.hpp"
#include "ir_serializer.hpp"
#include <format>
#include <cstring>
#include <cctype>

namespace aletheia::debug {

DebugObserver::DebugObserver(DebugOptions opts, std::ostream& output)
    : opts_(std::move(opts)), out_(output) {
    // Resolve debug_all convenience
    if (opts_.debug_all) {
        opts_.stage_metrics = true;
        opts_.diff_stages = true;
        opts_.check_invariants = true;
    }
    if (opts_.stage_metrics_json) {
        opts_.stage_metrics = true;
    }
    if (!opts_.check_invariants_after.empty()) {
        invariant_selector_ = parse_stage_selector(opts_.check_invariants_after);
    }
}

DebugObserver::StageSelector DebugObserver::parse_stage_selector(const std::string& raw) {
    StageSelector selector;
    selector.stage_name = raw;
    selector.ordinal = 0;
    selector.valid = true;

    const std::size_t hash_pos = raw.find('#');
    if (hash_pos == std::string::npos) {
        return selector;
    }

    selector.stage_name = raw.substr(0, hash_pos);
    const std::string ordinal_text = raw.substr(hash_pos + 1);
    if (selector.stage_name.empty() || ordinal_text.empty()) {
        selector.valid = false;
        return selector;
    }

    std::size_t parsed = 0;
    for (char c : ordinal_text) {
        if (c < '0' || c > '9') {
            selector.valid = false;
            return selector;
        }
        parsed = parsed * 10 + static_cast<std::size_t>(c - '0');
    }

    if (parsed == 0) {
        selector.valid = false;
        return selector;
    }
    selector.ordinal = parsed;
    return selector;
}

std::string DebugObserver::normalize_for_fingerprint(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool prev_space = false;
    for (char c : text) {
        const bool is_space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (is_space) {
            if (!prev_space) {
                normalized.push_back(' ');
            }
            prev_space = true;
            continue;
        }
        normalized.push_back(c);
        prev_space = false;
    }
    if (!normalized.empty() && normalized.front() == ' ') {
        normalized.erase(normalized.begin());
    }
    if (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

std::string DebugObserver::violation_fingerprint(const InvariantViolation& violation) {
    return std::format("{}|{}|{}",
                       violation.invariant_name,
                       normalize_for_fingerprint(violation.description),
                       normalize_for_fingerprint(violation.context));
}

bool DebugObserver::should_run_invariants_for_stage(const std::string& stage_name,
                                                    std::size_t name_ordinal) const {
    if (opts_.check_invariants_after.empty()) {
        return opts_.check_invariants;
    }
    if (!invariant_selector_.valid) {
        return false;
    }
    if (stage_name != invariant_selector_.stage_name) {
        return false;
    }
    if (invariant_selector_.ordinal == 0) {
        return true;
    }
    return name_ordinal == invariant_selector_.ordinal;
}

std::string DebugObserver::format_unknown_selector_diagnostic() const {
    if (opts_.check_invariants_after.empty() || selector_match_count_ > 0) {
        return "";
    }

    std::string out;
    if (!invariant_selector_.valid) {
        out += std::format("debug: --check-invariants-after selector '{}' is invalid; expected Name or Name#N\n",
                           opts_.check_invariants_after);
    } else {
        out += std::format("debug: --check-invariants-after selector '{}' was not observed during execution\n",
                           opts_.check_invariants_after);
    }
    out += "debug: observed stages: ";
    if (stage_name_order_.empty()) {
        out += "<none>\n";
        return out;
    }

    for (std::size_t i = 0; i < stage_name_order_.size(); ++i) {
        const auto& stage = stage_name_order_[i];
        const auto total_it = stage_name_total_observed_.find(stage);
        const std::size_t count = (total_it == stage_name_total_observed_.end()) ? 0 : total_it->second;
        out += stage;
        if (count > 1) {
            out += std::format(" (x{})", count);
        }
        if (i + 1 < stage_name_order_.size()) {
            out += ", ";
        }
    }
    out += "\n";
    return out;
}

void DebugObserver::update_phase(const char* stage_name, bool after) {
    if (!after) return;

    if (std::strcmp(stage_name, "SsaConstructor") == 0) {
        current_phase_ = PipelinePhase::SSA;
    } else if (std::strcmp(stage_name, "SsaDestructor") == 0) {
        current_phase_ = PipelinePhase::PostSSA;
    } else if (std::strcmp(stage_name, "PatternIndependentRestructuring") == 0) {
        current_phase_ = PipelinePhase::PostStructuring;
    }
}

void DebugObserver::operator()(const char* stage_name, bool before_stage,
                                 DecompilerTask& task) {
    const std::string stage_name_str = stage_name ? stage_name : "";

    if (before_stage) {
        ++stage_ordinal_;
        const std::size_t name_ordinal = ++stage_name_occurrence_[stage_name_str];
        const auto [_, inserted] = stage_name_total_observed_.emplace(stage_name_str, 1);
        if (!inserted) {
            stage_name_total_observed_[stage_name_str] += 1;
        } else {
            stage_name_order_.push_back(stage_name_str);
        }

        if (opts_.stage_metrics) {
            metrics_collector_.before_stage(stage_name, task);
        }

        if (opts_.diff_stages) {
            // Capture "before" snapshot for diff
            bool want_diff = true;
            if (!opts_.diff_stage_name.empty() &&
                opts_.diff_stage_name != stage_name) {
                want_diff = false;
            }
            if (want_diff) {
                last_snapshot_ = capture_snapshot(task);
            }
        }

        if (!opts_.trace_variable.empty()) {
            provenance_tracker_.before_stage(stage_name, stage_ordinal_, task);
        }

        (void)name_ordinal;
    } else {
        // After stage
        if (opts_.stage_metrics) {
            metrics_collector_.after_stage(stage_name, task);
        }

        if (opts_.diff_stages) {
            bool want_diff = true;
            if (!opts_.diff_stage_name.empty() &&
                opts_.diff_stage_name != stage_name) {
                want_diff = false;
            }
            if (want_diff) {
                auto after_snapshot = capture_snapshot(task);
                auto diff = diff_snapshots(last_snapshot_, after_snapshot);
                if (diff.has_changes()) {
                    out_ << format_diff(diff, stage_name, stage_ordinal_,
                                        task.function_name(),
                                        task.function_address());
                }
            }
        }

        const auto name_ordinal_it = stage_name_occurrence_.find(stage_name_str);
        const std::size_t name_ordinal =
            (name_ordinal_it == stage_name_occurrence_.end()) ? 0 : name_ordinal_it->second;
        if (should_run_invariants_for_stage(stage_name_str, name_ordinal)) {
            selector_match_count_ += 1;
            auto violations = invariant_checker_.check_all(task.cfg(), current_phase_);
            if (!violations.empty()) {
                std::vector<InvariantViolation> new_violations;
                std::unordered_set<std::string> current_fingerprints;
                std::size_t repeated_suppressed = 0;

                for (const auto& violation : violations) {
                    const std::string fingerprint = violation_fingerprint(violation);
                    if (current_fingerprints.contains(fingerprint)) {
                        continue;
                    }
                    current_fingerprints.insert(fingerprint);
                    if (active_violation_fingerprints_.contains(fingerprint)) {
                        repeated_suppressed += 1;
                    } else {
                        new_violations.push_back(violation);
                    }
                }

                out_ << "=== AFTER " << stage_name << " (#" << stage_ordinal_ << ") INVARIANTS ===\n";
                out_ << "summary: new=" << new_violations.size()
                     << " repeated_suppressed=" << repeated_suppressed
                     << " total_active=" << current_fingerprints.size() << "\n";
                if (!new_violations.empty()) {
                    out_ << IrInvariantChecker::format_violations(new_violations);
                }
                active_violation_fingerprints_ = std::move(current_fingerprints);
            } else {
                active_violation_fingerprints_.clear();
            }
        }

        if (!opts_.trace_variable.empty()) {
            provenance_tracker_.after_stage(stage_name, stage_ordinal_, task);
        }

        if (opts_.dump_ir) {
            out_ << "=== AFTER " << stage_name << " (#" << stage_ordinal_ << ") ===\n";
            out_ << cfg_to_string(task.cfg());
            out_ << "=============================\n";
        }

        update_phase(stage_name, true);
    }
}

std::string DebugObserver::format_summary() const {
    std::string result;
    if (!selector_diagnostic_emitted_) {
        result += format_unknown_selector_diagnostic();
        selector_diagnostic_emitted_ = true;
    }
    if (opts_.stage_metrics) {
        if (opts_.stage_metrics_json) {
            result += metrics_collector_.format_json();
        } else {
            result += metrics_collector_.format_table();
        }
    }
    return result;
}

} // namespace aletheia::debug
