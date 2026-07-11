#include "report/html_report.hpp"

#include "metrics/metrics_store.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace tftp_test_harness::report {

using metrics::MetricRecord;
using metrics::Outcome;
using metrics::Severity;

std::string html_escape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

namespace {

std::string format_double(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::string format_bytes_per_second(double value) {
    if (value >= 1024.0 * 1024.0) {
        return format_double(value / (1024.0 * 1024.0), 2) + " MiB/s";
    }
    if (value >= 1024.0) {
        return format_double(value / 1024.0, 1) + " KiB/s";
    }
    return format_double(value, 0) + " B/s";
}

const char* outcome_badge_class(Outcome outcome) {
    switch (outcome) {
        case Outcome::Pass: return "badge pass";
        case Outcome::Fail: return "badge fail";
        case Outcome::Skipped: return "badge skip";
    }
    return "badge";
}

std::string counter_or_dash(const MetricRecord& record, const char* name) {
    auto it = record.counters.find(name);
    if (it == record.counters.end()) return "—";
    if (std::floor(it->second) == it->second) {
        return std::to_string(static_cast<long long>(it->second));
    }
    return format_double(it->second, 1);
}

} // namespace

std::string shared_report_css() {
    // Inline, theme-aware. Palette values are the validated dataviz defaults
    // (blue primary; green/amber/red status), stepped for each surface.
    return R"CSS(
:root{
  --surface:#f4f4f1; --card:#ffffff; --ink:#0b0b0b; --ink2:#52514e; --muted:#8a8a85;
  --series:#2a78d6; --good:#008300; --warn:#b07400; --bad:#d13a39;
  --border:#e0e0da; --grid:#ececE6; --axis:#b8b8b1;
  --good-bg:#e7f3e7; --warn-bg:#faf1dd; --bad-bg:#fbe7e7;
}
@media (prefers-color-scheme: dark){
  :root{
    --surface:#141413; --card:#1f1f1d; --ink:#f4f4f0; --ink2:#c3c2b7; --muted:#8f8e86;
    --series:#3987e5; --good:#4fae4f; --warn:#e0a52a; --bad:#e66767;
    --border:#33332f; --grid:#2a2a27; --axis:#4a4a45;
    --good-bg:#16241a; --warn-bg:#2a2416; --bad-bg:#2c1a1a;
  }
}
:root[data-theme=light]{
  --surface:#f4f4f1; --card:#ffffff; --ink:#0b0b0b; --ink2:#52514e; --muted:#8a8a85;
  --series:#2a78d6; --good:#008300; --warn:#b07400; --bad:#d13a39;
  --border:#e0e0da; --grid:#ececE6; --axis:#b8b8b1;
  --good-bg:#e7f3e7; --warn-bg:#faf1dd; --bad-bg:#fbe7e7;
}
:root[data-theme=dark]{
  --surface:#141413; --card:#1f1f1d; --ink:#f4f4f0; --ink2:#c3c2b7; --muted:#8f8e86;
  --series:#3987e5; --good:#4fae4f; --warn:#e0a52a; --bad:#e66767;
  --border:#33332f; --grid:#2a2a27; --axis:#4a4a45;
  --good-bg:#16241a; --warn-bg:#2a2416; --bad-bg:#2c1a1a;
}
*{box-sizing:border-box;}
body{margin:0;background:var(--surface);color:var(--ink);
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  line-height:1.5;}
.wrap{max-width:1080px;margin:0 auto;padding:32px 20px 80px;}
h1{font-size:26px;margin:0 0 4px;letter-spacing:-0.01em;}
h2{font-size:19px;margin:40px 0 14px;padding-bottom:6px;border-bottom:1px solid var(--border);}
h3{font-size:15px;margin:24px 0 8px;color:var(--ink2);}
.sub{color:var(--ink2);font-size:14px;margin:0 0 4px;}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(165px,1fr));gap:14px;margin-top:20px;}
.tile{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px 18px;}
.tile .label{font-size:12px;text-transform:uppercase;letter-spacing:0.04em;color:var(--muted);}
.tile .value{font-size:30px;font-weight:650;margin-top:6px;letter-spacing:-0.02em;}
.tile .value.small{font-size:20px;}
.tile .foot{font-size:12px;color:var(--ink2);margin-top:4px;}
.value.good{color:var(--good);} .value.warn{color:var(--warn);} .value.bad{color:var(--bad);}
table{width:100%;border-collapse:collapse;font-size:13.5px;margin-top:8px;}
th,td{text-align:left;padding:8px 10px;border-bottom:1px solid var(--border);vertical-align:top;}
th{color:var(--muted);font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:0.03em;}
.tablewrap{overflow-x:auto;background:var(--card);border:1px solid var(--border);border-radius:12px;}
.badge{display:inline-block;padding:2px 9px;border-radius:999px;font-size:12px;font-weight:600;}
.badge.pass{background:var(--good-bg);color:var(--good);}
.badge.fail{background:var(--bad-bg);color:var(--bad);}
.badge.skip{background:var(--grid);color:var(--muted);}
.callout{border-radius:12px;padding:14px 18px;margin-top:16px;font-size:14px;}
.callout.bad{background:var(--bad-bg);border:1px solid var(--bad);}
.callout.good{background:var(--good-bg);border:1px solid var(--good);}
.callout ul{margin:8px 0 0;padding-left:20px;}
.charts{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;margin-top:16px;}
.chartcard{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px 16px 8px;}
.chartcard .ct{font-size:14px;font-weight:600;margin-bottom:2px;}
.chartcard .cs{font-size:12px;color:var(--muted);margin-bottom:6px;}
svg{max-width:100%;height:auto;display:block;}
.svg-grid{stroke:var(--grid);stroke-width:1;}
.svg-axis{stroke:var(--axis);stroke-width:1;}
.svg-line{stroke:var(--series);stroke-width:2;fill:none;stroke-linejoin:round;stroke-linecap:round;}
.svg-dot{fill:var(--series);}
.svg-text{fill:var(--ink2);font-size:10px;font-family:inherit;}
.svg-area{fill:var(--series);opacity:0.10;}
.caprow{display:flex;align-items:center;gap:8px;}
.dot{width:9px;height:9px;border-radius:50%;display:inline-block;}
.dot.yes{background:var(--good);} .dot.no{background:var(--muted);}
.narrative{color:var(--ink2);}
.foot-note{margin-top:40px;color:var(--muted);font-size:12px;}
code{background:var(--grid);padding:1px 5px;border-radius:5px;font-size:12.5px;}
)CSS";
}

std::string render_resilience_svg(const ResilienceCurve& curve) {
    // Single-series line chart: success (y, 0..1) vs impairment intensity (x).
    const double width = 320, height = 190;
    const double left = 38, right = 12, top = 12, bottom = 30;
    const double plot_w = width - left - right;
    const double plot_h = height - top - bottom;

    double min_x = 0.0, max_x = 1.0;
    if (!curve.points.empty()) {
        min_x = curve.points.front().intensity;
        max_x = curve.points.back().intensity;
        if (max_x <= min_x) max_x = min_x + 1.0;
    }
    auto sx = [&](double x) {
        return left + plot_w * (x - min_x) / (max_x - min_x);
    };
    auto sy = [&](double y) { return top + plot_h * (1.0 - y); };

    std::ostringstream svg;
    svg << "<svg viewBox=\"0 0 " << width << " " << height
        << "\" role=\"img\" aria-label=\"" << html_escape(curve.title)
        << " success rate versus intensity\">";

    // Horizontal gridlines + y labels at 0, 0.5, 1.0.
    for (double gy : {0.0, 0.5, 1.0}) {
        const double py = sy(gy);
        svg << "<line class=\"svg-grid\" x1=\"" << left << "\" y1=\"" << py
            << "\" x2=\"" << (width - right) << "\" y2=\"" << py << "\"/>";
        svg << "<text class=\"svg-text\" x=\"" << (left - 6) << "\" y=\""
            << (py + 3) << "\" text-anchor=\"end\">"
            << format_double(gy * 100.0, 0) << "%</text>";
    }
    // Axes.
    svg << "<line class=\"svg-axis\" x1=\"" << left << "\" y1=\"" << sy(0.0)
        << "\" x2=\"" << (width - right) << "\" y2=\"" << sy(0.0) << "\"/>";
    svg << "<line class=\"svg-axis\" x1=\"" << left << "\" y1=\"" << top
        << "\" x2=\"" << left << "\" y2=\"" << sy(0.0) << "\"/>";

    if (!curve.points.empty()) {
        // Filled area under the curve for magnitude read.
        std::ostringstream area;
        area << "M" << sx(curve.points.front().intensity) << "," << sy(0.0);
        for (const auto& p : curve.points) {
            area << " L" << sx(p.intensity) << "," << sy(p.success);
        }
        area << " L" << sx(curve.points.back().intensity) << "," << sy(0.0)
             << " Z";
        svg << "<path class=\"svg-area\" d=\"" << area.str() << "\"/>";

        // The line.
        std::ostringstream line;
        for (std::size_t i = 0; i < curve.points.size(); ++i) {
            line << (i == 0 ? "M" : " L") << sx(curve.points[i].intensity)
                 << "," << sy(curve.points[i].success);
        }
        svg << "<path class=\"svg-line\" d=\"" << line.str() << "\"/>";

        // Points + x labels.
        for (const auto& p : curve.points) {
            svg << "<circle class=\"svg-dot\" cx=\"" << sx(p.intensity)
                << "\" cy=\"" << sy(p.success) << "\" r=\"3\"/>";
            svg << "<text class=\"svg-text\" x=\"" << sx(p.intensity)
                << "\" y=\"" << (height - bottom + 14)
                << "\" text-anchor=\"middle\">" << format_double(p.intensity, 2)
                << "</text>";
        }
    }
    svg << "<text class=\"svg-text\" x=\"" << (left + plot_w / 2) << "\" y=\""
        << (height - 3) << "\" text-anchor=\"middle\">impairment intensity"
        << "</text>";
    svg << "</svg>";
    return svg.str();
}

namespace {

std::string render_tiles(const Scorecard& card) {
    std::ostringstream out;
    out << "<div class=\"tiles\">";

    // Conformance score.
    const char* conf_class = card.conformance_score >= 90   ? "good"
                             : card.conformance_score >= 70 ? "warn"
                                                            : "bad";
    out << "<div class=\"tile\"><div class=\"label\">Conformance score</div>"
        << "<div class=\"value " << conf_class << "\">"
        << format_double(card.conformance_score, 1) << "</div>"
        << "<div class=\"foot\">weighted over A-E, G (CRITICAL &gt; MAJOR &gt; "
           "MINOR)</div></div>";

    // Resilience index.
    out << "<div class=\"tile\"><div class=\"label\">Resilience index</div>"
        << "<div class=\"value\">" << format_double(card.resilience_index, 3)
        << "</div><div class=\"foot\">mean area under F success curves "
           "(0-1)</div></div>";

    // Integrity — the most damning axis.
    const bool clean = card.integrity_violations.empty();
    out << "<div class=\"tile\"><div class=\"label\">Integrity violations</div>"
        << "<div class=\"value " << (clean ? "good" : "bad") << "\">"
        << card.integrity_violations.size() << "</div>"
        << "<div class=\"foot\">reported success but delivered wrong "
           "bytes</div></div>";

    // Graceful degradation.
    out << "<div class=\"tile\"><div class=\"label\">Graceful degradation</div>"
        << "<div class=\"value\">"
        << format_double(card.graceful_degradation_rate * 100.0, 0)
        << "%</div><div class=\"foot\">of failures ended in a clean "
           "error, not a hang</div></div>";

    // Efficiency.
    out << "<div class=\"tile\"><div class=\"label\">Avg throughput</div>"
        << "<div class=\"value small\">"
        << format_bytes_per_second(card.average_throughput_bytes_per_second)
        << "</div><div class=\"foot\">across passing transfers; "
        << format_double(card.total_retransmissions, 0)
        << " retransmissions total</div></div>";

    // Results tally.
    out << "<div class=\"tile\"><div class=\"label\">Results</div>"
        << "<div class=\"value small\"><span class=\"badge pass\">"
        << card.total_passed << " pass</span> <span class=\"badge fail\">"
        << card.total_failed << " fail</span></div>"
        << "<div class=\"foot\">" << card.total_skipped
        << " skipped (unsupported)</div></div>";

    out << "</div>";
    return out.str();
}

std::string render_section_table(const Scorecard& card) {
    std::ostringstream out;
    out << "<div class=\"tablewrap\"><table><thead><tr><th>Section</th>"
           "<th>Pass</th><th>Fail</th><th>Skipped</th><th>Weighted "
           "score</th></tr></thead><tbody>";
    for (const auto& section : card.sections) {
        out << "<tr><td class=\"mono\">" << html_escape(section.section)
            << "</td><td>" << section.passed << "</td><td>" << section.failed
            << "</td><td>" << section.skipped << "</td><td>"
            << format_double(section.weighted_score, 1) << "</td></tr>";
    }
    out << "</tbody></table></div>";
    return out.str();
}

std::string render_must_failures(const Scorecard& card) {
    std::ostringstream out;
    if (card.must_failure_ids.empty()) {
        out << "<div class=\"callout good\"><strong>No MUST-level (CRITICAL) "
               "failures.</strong> Every CRITICAL conformance check passed.</div>";
    } else {
        out << "<div class=\"callout bad\"><strong>"
            << card.must_failure_ids.size()
            << " MUST-level (CRITICAL) failure(s)</strong> — shown separately, "
               "never averaged into the score:<ul>";
        for (const auto& id : card.must_failure_ids) {
            out << "<li class=\"mono\">" << html_escape(id) << "</li>";
        }
        out << "</ul></div>";
    }
    return out.str();
}

std::string render_capability_matrix(const Scorecard& card) {
    std::ostringstream out;
    out << "<div class=\"tablewrap\"><table><thead><tr><th>Capability</th>"
           "<th>Supported</th></tr></thead><tbody>";
    for (const auto& entry : card.capability_supported) {
        out << "<tr><td class=\"mono\">" << html_escape(entry.first)
            << "</td><td><span class=\"caprow\"><span class=\"dot "
            << (entry.second ? "yes" : "no") << "\"></span>"
            << (entry.second ? "yes" : "no (tests SKIPPED)")
            << "</span></td></tr>";
    }
    out << "</tbody></table></div>";
    return out.str();
}

std::string render_curves(const Scorecard& card) {
    if (card.curves.empty()) return "<p class=\"sub\">No resilience sweeps in "
                                    "this run.</p>";
    std::ostringstream out;
    out << "<div class=\"charts\">";
    for (const auto& curve : card.curves) {
        out << "<div class=\"chartcard\"><div class=\"ct mono\">"
            << html_escape(curve.impairment_id) << "</div><div class=\"cs\">"
            << html_escape(curve.title) << " — area "
            << format_double(curve.area, 3) << "</div>"
            << render_resilience_svg(curve) << "</div>";
    }
    out << "</div>";
    return out.str();
}

std::string render_integrity_ledger(const Scorecard& card) {
    std::ostringstream out;
    if (card.integrity_violations.empty()) {
        out << "<div class=\"callout good\"><strong>Integrity ledger is "
               "clean.</strong> No transfer reported success while delivering "
               "non-matching bytes.</div>";
        return out.str();
    }
    out << "<div class=\"callout bad\"><strong>"
        << card.integrity_violations.size()
        << " silent data-integrity failure(s).</strong> A transfer reported "
           "success but delivered corrupted bytes — the most severe outcome in "
           "the matrix.</div>";
    out << "<div class=\"tablewrap\"><table><thead><tr><th>Test</th>"
           "<th>Condition</th><th>Detail</th><th>Seed</th></tr></thead><tbody>";
    for (const auto& entry : card.integrity_violations) {
        out << "<tr><td class=\"mono\">" << html_escape(entry.test_id)
            << "</td><td>" << html_escape(entry.intensity_label) << "</td><td>"
            << html_escape(entry.narrative) << "</td><td class=\"mono\">"
            << entry.seed << "</td></tr>";
    }
    out << "</tbody></table></div>";
    return out.str();
}

std::string render_appendix(const Scorecard& card) {
    std::ostringstream out;
    out << "<div class=\"tablewrap\"><table><thead><tr><th>ID</th>"
           "<th>Behavior</th><th>Outcome</th><th>RFC</th><th>Retx</th>"
           "<th>Drop</th><th>Narrative</th><th>Seed</th></tr></thead><tbody>";
    for (const auto& record : card.records) {
        out << "<tr><td class=\"mono\">" << html_escape(record.test_id);
        if (!record.intensity_label.empty()) {
            out << "<br><span class=\"sub mono\">"
                << html_escape(record.intensity_label) << "</span>";
        }
        out << "</td><td>" << html_escape(record.title) << "</td><td><span class=\""
            << outcome_badge_class(record.outcome) << "\">"
            << metrics::outcome_name(record.outcome) << "</span>";
        if (record.severity == Severity::Critical &&
            record.outcome == Outcome::Fail) {
            out << " <span class=\"badge fail\">CRITICAL</span>";
        }
        out << "</td><td class=\"sub\">" << html_escape(record.rfc_clause)
            << "</td><td>" << counter_or_dash(record, "retransmissions")
            << "</td><td>" << counter_or_dash(record, "datagrams_dropped")
            << "</td><td class=\"narrative\">" << html_escape(record.narrative)
            << "</td><td class=\"mono\">" << record.seed << "</td></tr>";
    }
    out << "</tbody></table></div>";
    return out.str();
}

} // namespace

std::string render_single_report(const Scorecard& card) {
    std::ostringstream out;
    out << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        << "<meta name=\"viewport\" content=\"width=device-width,"
           "initial-scale=1\">"
        << "<title>TFTP conformance report — "
        << html_escape(card.implementation_name) << "</title><style>"
        << shared_report_css() << "</style></head><body><div class=\"wrap\">";

    out << "<h1>TFTP Conformance &amp; Robustness Report</h1>";
    out << "<p class=\"sub\">Implementation under test: <strong>"
        << html_escape(card.implementation_name) << "</strong></p>";
    out << "<p class=\"sub\">Every result traces to a stable test ID and the RFC "
           "clause it derives from. MUST-level failures are listed separately "
           "and are never averaged into the conformance score.</p>";

    out << "<h2>Executive scorecard</h2>";
    out << render_tiles(card);

    out << "<h2>MUST-level conformance</h2>";
    out << render_must_failures(card);

    out << "<h2>Per-section conformance</h2>";
    out << render_section_table(card);

    out << "<h2>Capability matrix</h2>";
    out << "<p class=\"sub\">Unsupported capabilities cause their tests to be "
           "recorded SKIPPED, never FAILED.</p>";
    out << render_capability_matrix(card);

    out << "<h2>Resilience curves (F-series)</h2>";
    out << "<p class=\"sub\">Byte-exact success rate as each impairment "
           "intensifies. The normalized area under a curve is the resilience "
           "index contribution.</p>";
    out << render_curves(card);

    out << "<h2>Integrity ledger</h2>";
    out << render_integrity_ledger(card);

    out << "<h2>Appendix — every result</h2>";
    out << render_appendix(card);

    out << "<p class=\"foot-note\">Generated by the TFTP conformance &amp; "
           "robustness test harness. Impairments applied by a local loopback "
           "UDP proxy; each result records the PRNG seed for exact "
           "reproduction.</p>";

    out << "</div></body></html>";
    return out.str();
}

bool write_text_file(const std::string& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream << contents;
    return static_cast<bool>(stream);
}

} // namespace tftp_test_harness::report
