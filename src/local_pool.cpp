// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "local_pool.h"

#include "garbage.h"
#include "unitext.h"
#include "winutil.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <cstdlib>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace live {

namespace {

// The server's similar-line rule (Makine-WEB src/live/text.ts; docs/MIMARI.md §4.3)
constexpr size_t kFuzzyMinKeyLen = 12;
constexpr double kFuzzyMinSimilarity = 0.9;

double nowSeconds()
{
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

std::vector<std::u32string> digitRuns(const std::u32string& s)   // re.findall(r"\d+", s)
{
    std::vector<std::u32string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && !text::isDecimal(s[i])) ++i;
        const size_t b = i;
        while (i < s.size() && text::isDecimal(s[i])) ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

double num(const json& j) { return j.is_number() ? j.get<double>() : 0.0; }

bool truthy(const json& j)
{
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<double>() != 0;
    if (j.is_string() || j.is_array() || j.is_object()) return !j.empty();
    return false;
}

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

json readJson(const std::wstring& path)
{
    const auto text = win::readFile(path);
    if (!text) return nullptr;
    json j = json::parse(*text, nullptr, false);
    return j.is_discarded() ? json(nullptr) : j;
}

std::string str(const json& j)
{
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number_integer()) return std::to_string(j.get<int64_t>());
    if (j.is_number()) return j.dump();
    return {};
}

} // namespace

int boundedLevenshtein(const std::u32string& a0, const std::u32string& b0, int limit)
{
    const std::u32string* a = &a0;
    const std::u32string* b = &b0;
    if (std::abs(static_cast<int>(a->size()) - static_cast<int>(b->size())) > limit) return limit + 1;
    if (a->size() > b->size()) std::swap(a, b);
    std::vector<int> prev(a->size() + 1), cur(a->size() + 1);
    for (size_t j = 0; j <= a->size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= b->size(); ++i) {
        cur[0] = static_cast<int>(i);
        int best = cur[0];
        for (size_t j = 1; j <= a->size(); ++j) {
            const int v = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + ((*a)[j - 1] != (*b)[i - 1])});
            cur[j] = v;
            best = std::min(best, v);
        }
        if (best > limit) return limit + 1;
        std::swap(prev, cur);
    }
    return prev.back() <= limit ? prev.back() : limit + 1;
}

bool keysSimilar(const std::u32string& a, const std::u32string& b)
{
    if (a == b) return true;
    const size_t la = a.size(), lb = b.size();
    if (std::min(la, lb) < kFuzzyMinKeyLen) return false;
    const int limit = static_cast<int>(static_cast<double>(std::max(la, lb)) * (1 - kFuzzyMinSimilarity) + 1e-9);
    if (std::abs(static_cast<int>(la) - static_cast<int>(lb)) > limit) return false;
    if (digitRuns(a) != digitRuns(b)) return false;   // "floor 3" / "floor 4" never match
    return boundedLevenshtein(a, b, limit) <= limit;
}

bool keysSameLine(const std::u32string& a, const std::u32string& b)
{
    if (!keysSimilar(a, b)) return false;
    const size_t n = 6;
    const auto head = [&](const std::u32string& s) { return s.substr(0, n); };
    const auto tail = [&](const std::u32string& s) { return s.size() <= n ? s : s.substr(s.size() - n); };
    return boundedLevenshtein(head(a), head(b), 1) <= 1 && boundedLevenshtein(tail(a), tail(b), 1) <= 1;
}

// /(?<![\p{L}\p{N}])(?:not|no|never|nothing|nobody|none|nor|cannot|nope|(?:do|does|…|need)n['’]?t)(?![\p{L}\p{N}])/
// over the normalized text, in token steps
int negationCount(std::string_view textUtf8)
{
    static const std::set<std::u32string> words{U"not", U"no", U"never", U"nothing", U"nobody", U"none", U"nor", U"cannot", U"nope"};
    static const std::set<std::u32string> aux{U"do", U"does", U"did", U"is", U"are", U"was", U"were", U"ca", U"can", U"could",
                                              U"wo", U"would", U"should", U"have", U"has", U"had", U"ai", U"must", U"need"};
    const std::u32string t = text::normalizeDialogue(text::fromUtf8(textUtf8));
    const auto alnum = [&](size_t i) { return i < t.size() && (text::isAlpha(t[i]) || text::isNumber(t[i])); };
    int count = 0;
    for (size_t i = 0; i < t.size();) {
        if (!alnum(i)) {
            ++i;
            continue;
        }
        size_t e = i;
        while (alnum(e)) ++e;
        const std::u32string run = t.substr(i, e - i);
        if (words.count(run)) {
            ++count;
        } else if (run.size() > 2 && run.compare(run.size() - 2, 2, U"nt") == 0 && aux.count(run.substr(0, run.size() - 2))) {
            ++count;   // "dont", "cant"
        } else if (run.size() > 1 && run.back() == U'n' && aux.count(run.substr(0, run.size() - 1)) && e + 1 < t.size()
                   && (t[e] == U'\'' || t[e] == U'’') && t[e + 1] == U't' && !alnum(e + 2)) {
            ++count;   // "don't", "can't"
            e += 2;
        }
        i = e;
    }
    return count;
}

// ── GamePool ────────────────────────────────────────────────────────────────

GamePool::GamePool(std::string slug_, std::string name_, std::string rev_, const json& items, double syncedAt_,
                   const json& variants, const json& relations)
    : slug(std::move(slug_)), name(std::move(name_)), rev(std::move(rev_)), syncedAt(syncedAt_)
{
    m_withVariants = variants.is_array();
    if (variants.is_array())
        for (const json& v : variants) {   // [id, signal, translation]; a ctx: variant is never picked for anyone
            if (!v.is_array() || v.size() < 3) continue;
            const std::string signal = str(v[1]), translation = str(v[2]);
            int64_t id = 0;
            if (v[0].is_number_integer()) id = v[0].get<int64_t>();
            else continue;
            if (signal.empty() || translation.empty() || signal.rfind("ctx:", 0) == 0) continue;
            m_variants[id][signal] = translation;
        }
    if (relations.is_array())
        for (const json& r : relations) {
            if (!r.is_array() || r.size() < 3) continue;
            const std::string form = str(r[2]);
            if (form == "sen" || form == "siz") m_relations.push_back({str(r[0]), str(r[1]), form});
        }
    if (!items.is_array()) return;
    for (const json& it : items) {
        if (!it.is_array() || it.size() < 4) continue;
        int64_t id = 0;
        if (it[0].is_number_integer()) {
            id = it[0].get<int64_t>();
        } else if (it[0].is_string()) {
            char* end = nullptr;
            const std::string s = it[0].get<std::string>();
            id = std::strtoll(s.c_str(), &end, 10);
            if (s.empty() || *end) continue;
        } else {
            continue;
        }
        const std::string key = str(it[1]), source = str(it[2]), translation = str(it[3]);
        if (key.empty() || translation.empty()) continue;
        const std::u32string k = text::fromUtf8(key);
        m_items[id] = {k, source, translation};
        m_byKey.emplace(k, id);
        m_byLen[k.size()].push_back(id);
    }
}

std::optional<PoolHit> GamePool::lookup(const std::string& textUtf8, bool fuzzy, const std::vector<std::string>& signals) const
{
    const std::u32string key = text::compactPoolKey(text::fromUtf8(textUtf8));
    if (key.empty()) return std::nullopt;
    if (const auto it = m_byKey.find(key); it != m_byKey.end())
        return withVariant(PoolHit{it->second, m_items.at(it->second).translation, true, {}}, signals);
    const size_t n = key.size();
    if (!fuzzy || n < kFuzzyMinKeyLen) return std::nullopt;
    const int negations = negationCount(textUtf8);
    {
        std::lock_guard lock(m_fuzzyMutex);
        if (const auto it = m_fuzzy.find(key); it != m_fuzzy.end()) {
            if (!it->second) return std::nullopt;
            return withVariant(PoolHit{*it->second, m_items.at(*it->second).translation, false, {}}, signals);
        }
    }
    const int d = static_cast<int>(static_cast<double>(n) * (1 - kFuzzyMinSimilarity));
    std::optional<int64_t> best;
    int bestDist = 0;
    for (int length = static_cast<int>(n) - d - 1; length < static_cast<int>(n) + d + 2; ++length) {
        if (length < 0) continue;
        const auto bucket = m_byLen.find(static_cast<size_t>(length));
        if (bucket == m_byLen.end()) continue;
        for (const int64_t cand : bucket->second) {
            const std::u32string& k = m_items.at(cand).key;
            // the server's sameLine: held to both ends, and as many negations ("NOT Resuscitate" ≠ "Resuscitate")
            if (!keysSameLine(key, k) || negationCount(m_items.at(cand).source) != negations) continue;
            const int dist = boundedLevenshtein(key, k, static_cast<int>(std::max(key.size(), k.size())));
            if (!best || dist < bestDist) {
                best = cand;
                bestDist = dist;
            }
        }
    }
    {
        std::lock_guard lock(m_fuzzyMutex);
        if (m_fuzzy.size() >= 5000) m_fuzzy.clear();
        m_fuzzy[key] = best;
    }
    if (!best) return std::nullopt;
    return withVariant(PoolHit{*best, m_items.at(*best).translation, false, {}}, signals);
}

PoolHit GamePool::withVariant(PoolHit hit, const std::vector<std::string>& signals) const
{
    // (key, variant) before the plain key; only stable signals (line: / scene: / f:), never ctx:
    const auto v = m_variants.find(hit.id);
    if (v == m_variants.end()) return hit;
    for (const std::string& signal : signals) {
        if (signal.rfind("ctx:", 0) == 0) continue;
        if (const auto t = v->second.find(signal); t != v->second.end()) {
            hit.translation = t->second;
            hit.variant = signal;
            break;
        }
    }
    return hit;
}

std::string GamePool::ruleForm(const std::string& speaker, const std::string& listener) const
{
    return scene::ruleForm(m_relations, speaker, listener);
}

std::optional<PoolHit> GamePool::lookupExact(const std::string& textUtf8, const std::vector<std::string>& signals) const
{
    const std::u32string key = text::compactPoolKey(text::fromUtf8(textUtf8));
    if (key.empty()) return std::nullopt;
    const auto it = m_byKey.find(key);
    if (it == m_byKey.end()) return std::nullopt;
    return withVariant(PoolHit{it->second, m_items.at(it->second).translation, true, {}}, signals);
}

json GamePool::toJson() const
{
    json items = json::array();
    for (const auto& [id, it] : m_items) items.push_back({id, text::toUtf8(it.key), it.source, it.translation});
    json out = {{"game", slug}, {"name", name}, {"rev", rev}, {"synced_at", syncedAt}, {"items", items}};
    if (serverCount >= 0) out["count"] = serverCount;
    if (m_withVariants) {
        json variants = json::array(), relations = json::array();
        for (const auto& [id, bySignal] : m_variants)
            for (const auto& [signal, translation] : bySignal) variants.push_back({id, signal, translation});
        for (const scene::Relation& r : m_relations) relations.push_back({r.speakerKey, r.listenerKey, r.form});
        out["variants"] = variants;
        out["relations"] = relations;
    }
    return out;
}

// ── LocalPool ───────────────────────────────────────────────────────────────

LocalPool::LocalPool(std::wstring dataDir) : m_dir(std::move(dataDir) + L"\\pool")
{
    m_games = readJson(m_dir + L"\\games.json");
    if (!m_games.is_object() || !m_games.contains("confirmed") || !m_games["confirmed"].is_object())
        m_games = {{"confirmed", json::object()}};
}

std::wstring LocalPool::file(const std::string& slug) const
{
    std::string safe;
    for (const char c : slug)
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') safe += c;
    return m_dir + L"\\" + win::utf8ToWide(safe) + L".json";
}

void LocalPool::writeGames() const
{
    std::lock_guard write(m_writeMutex);
    std::string data;
    {
        std::lock_guard lock(m_mutex);
        data = dump(m_games);
    }
    win::writeFileAtomic(m_dir + L"\\games.json", data);
}

bool LocalPool::isConfirmed(const std::string& slug) const
{
    std::lock_guard lock(m_mutex);
    return m_games["confirmed"].contains(slug);
}

void LocalPool::confirm(const std::string& slug, const std::string& name)
{
    {
        std::lock_guard lock(m_mutex);
        m_games["confirmed"][slug] = {{"name", name}, {"t", nowSeconds()}};
    }
    writeGames();
}

void LocalPool::forget(const std::string& slug)
{
    {
        std::lock_guard lock(m_mutex);
        m_games["confirmed"].erase(slug);
        m_pools.erase(slug);
    }
    writeGames();
    DeleteFileW(file(slug).c_str());
}

std::shared_ptr<GamePool> LocalPool::get(const std::string& slug)
{
    if (slug.empty() || !isConfirmed(slug)) return nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_pools.find(slug); it != m_pools.end()) return it->second;
    }
    json d = readJson(file(slug));
    if (!d.is_object()) d = json::object();   // not downloaded yet: an empty pool, filled by the sync
    auto p = std::make_shared<GamePool>(slug, str(d.value("name", json())), str(d.value("rev", json())),
                                        d.value("items", json()), num(d.value("synced_at", json())),
                                        d.value("variants", json()), d.value("relations", json()));
    if (d.contains("count") && d["count"].is_number()) p->serverCount = d["count"].get<int>();
    std::lock_guard lock(m_mutex);
    // an "unchanged" sync since the file was written keeps the newer count in games.json only
    const json& confirmed = m_games["confirmed"];
    if (const auto g = confirmed.find(slug); g != confirmed.end() && g->is_object() && g->contains("count")
                                             && (*g)["count"].is_number()
                                             && str(g->value("rev", json())) == (p->hasVariants() ? p->rev : ""))
        p->serverCount = (*g)["count"].get<int>();
    return m_pools.emplace(slug, std::move(p)).first->second;
}

bool LocalPool::noteSummaryLocked(const std::string& slug, const GamePool& pool)
{
    json& games = m_games["confirmed"];
    if (!games.contains(slug) || !games[slug].is_object()) return false;
    json& g = games[slug];
    const json before = g;
    g["rev"] = pool.hasVariants() ? pool.rev : std::string();
    g["n"] = static_cast<int>(pool.size());
    g["count"] = pool.serverCount;
    if (g.value("name", std::string()).empty() && !pool.name.empty()) g["name"] = pool.name;
    return g != before;
}

std::vector<PoolSummary> LocalPool::summaries()
{
    std::vector<PoolSummary> out;
    std::vector<size_t> unread;   // pools saved before the summary was kept in games.json
    {
        std::lock_guard lock(m_mutex);
        for (const auto& [slug, g] : m_games["confirmed"].items()) {
            PoolSummary s;
            s.slug = slug;
            s.name = g.is_object() ? str(g.value("name", json())) : std::string();
            if (const auto it = m_pools.find(slug); it != m_pools.end()) {
                const GamePool& p = *it->second;
                s.rev = p.hasVariants() ? p.rev : std::string();
                s.local = static_cast<int>(p.size());
                s.serverCount = p.serverCount;
                if (s.name.empty()) s.name = p.name;
            } else if (g.is_object() && g.contains("n") && g["n"].is_number()) {
                s.rev = str(g.value("rev", json()));
                s.local = g["n"].get<int>();
                s.serverCount = g.contains("count") && g["count"].is_number() ? g["count"].get<int>() : -1;
            } else {
                unread.push_back(out.size());
            }
            out.push_back(std::move(s));
        }
    }
    bool write = false;
    for (const size_t i : unread) {
        PoolSummary& s = out[i];
        const json d = readJson(file(s.slug));
        if (!d.is_object()) continue;   // confirmed, nothing downloaded yet
        // read for its summary only, never kept: the lines of a game not being played stay on disk
        GamePool p(s.slug, str(d.value("name", json())), str(d.value("rev", json())), d.value("items", json()),
                   num(d.value("synced_at", json())), d.value("variants", json()), d.value("relations", json()));
        if (d.contains("count") && d["count"].is_number()) p.serverCount = d["count"].get<int>();
        s.rev = p.hasVariants() ? p.rev : std::string();
        s.local = static_cast<int>(p.size());
        s.serverCount = p.serverCount;
        if (s.name.empty()) s.name = p.name;
        std::lock_guard lock(m_mutex);
        write = noteSummaryLocked(s.slug, p) || write;
    }
    if (write) writeGames();
    return out;
}

std::string LocalPool::rev(const std::string& slug)
{
    const auto p = get(slug);
    // a pool saved before variants=1 has none: no revision, so the next sync brings everything
    return p && p->hasVariants() ? p->rev : std::string();
}

SyncStats LocalPool::applySync(const std::string& slug, const json& resp)
{
    const auto old = get(slug);
    SyncStats st;
    const json count = resp.is_object() ? resp.value("count", json()) : json();
    if (resp.is_object() && truthy(resp.value("unchanged", json())) && old) {
        old->syncedAt = nowSeconds();
        if (count.is_number()) old->serverCount = count.get<int>();
        st.total = static_cast<int>(old->size());
        st.unchanged = true;
        bool changed = false;
        {
            std::lock_guard lock(m_mutex);
            changed = noteSummaryLocked(slug, *old);
        }
        if (changed) writeGames();
        return st;
    }
    if (!resp.is_object()) return st;
    std::string name = str(resp.value("name", json()));
    if (name.empty() && old) name = old->name;
    auto fresh = std::make_shared<GamePool>(slug, name, str(resp.value("rev", json())), resp.value("items", json()),
                                            nowSeconds(), resp.value("variants", json::array()),
                                            resp.value("relations", json::array()));
    for (const auto& [id, item] : fresh->m_items) {
        if (!old || !old->m_items.count(id)) ++st.added;
        else if (old->m_items.at(id).translation != item.translation) ++st.changed;
    }
    if (old)
        for (const auto& [id, item] : old->m_items)
            if (!fresh->m_items.count(id)) ++st.removed;
    if (count.is_number()) fresh->serverCount = count.get<int>();
    st.total = static_cast<int>(fresh->size());
    const std::string data = dump(fresh->toJson());
    bool changed = false;
    {
        std::lock_guard lock(m_mutex);
        changed = noteSummaryLocked(slug, *fresh);
        m_pools[slug] = std::move(fresh);
    }
    win::writeFileAtomic(file(slug), data);
    if (changed) writeGames();
    return st;
}

std::optional<PoolHit> LocalPool::lookup(const std::string& slug, const std::string& text, bool fuzzy,
                                         const std::vector<std::string>& signals)
{
    const auto p = get(slug);
    if (!p) return std::nullopt;
    auto hit = p->lookup(text, fuzzy, signals);
    if (hit) {
        std::lock_guard lock(m_mutex);
        ++m_hits[slug][hit->id];
    }
    return hit;
}

std::optional<PoolHit> LocalPool::lookupExact(const std::string& slug, const std::string& text,
                                              const std::vector<std::string>& signals)
{
    const auto p = get(slug);
    return p ? p->lookupExact(text, signals) : std::nullopt;
}

std::string LocalPool::ruleForm(const std::string& slug, const std::string& speaker, const std::string& listener)
{
    const auto p = get(slug);
    return p ? p->ruleForm(speaker, listener) : std::string();
}

std::map<int64_t, int> LocalPool::takeHits(const std::string& slug)
{
    std::lock_guard lock(m_mutex);
    auto h = std::move(m_hits[slug]);
    m_hits.erase(slug);
    return h;
}

void LocalPool::returnHits(const std::string& slug, const std::map<int64_t, int>& hits)
{
    std::lock_guard lock(m_mutex);
    for (const auto& [id, n] : hits) m_hits[slug][id] += n;
}

} // namespace live
