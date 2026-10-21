#pragma once

#include <toml++/toml.hpp>

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace tto {

class StrictTable {
public:
    StrictTable(const toml::node* n, std::string path) : t_(n ? n->as_table() : nullptr), path_(std::move(path)) {
        if (!t_) fail("", "must be a table");
    }

    const toml::table* table() const { return t_; }

    const toml::node* take(const char* key) {
        if (!t_) return nullptr;
        const toml::node* v = t_->get(key);
        if (!v) return nullptr;
        used_.insert(key);
        return v;
    }

    bool str(const char* key, std::string& out, size_t max_len, bool allow_empty = true) {
        const toml::node* v = take(key);
        if (!v) return false;
        if (!v->is_string()) return fail(key, "must be a string");
        std::string s(v->as_string()->get());
        if (s.size() > max_len) return fail(key, "is too long (at most " + std::to_string(max_len) + " characters)");
        if (!allow_empty && s.empty()) return fail(key, "must not be empty");
        for (unsigned char c : s) {
            if (c < 0x20 || c == 0x7f) return fail(key, "contains control characters");
        }
        out = std::move(s);
        return ok();
    }

    bool boolean(const char* key, bool& out) {
        const toml::node* v = take(key);
        if (!v) return false;
        if (!v->is_boolean()) return fail(key, "must be true or false");
        out = v->as_boolean()->get();
        return ok();
    }

    template <typename I>
    bool integer(const char* key, I& out, long long lo, long long hi) {
        const toml::node* v = take(key);
        if (!v) return false;
        long long n = 0;
        if (v->is_integer()) {
            n = v->as_integer()->get();
        } else if (v->is_floating_point()) {
            double d = v->as_floating_point()->get();
            if (!std::isfinite(d) || d != std::floor(d)) return fail(key, "must be a whole number");
            n = (long long)d;
        } else {
            return fail(key, "must be a number");
        }
        if (n < lo || n > hi) return fail(key, "must be between " + std::to_string(lo) + " and " + std::to_string(hi));
        out = I(n);
        return ok();
    }

    bool number(const char* key, double& out) {
        const toml::node* v = take(key);
        if (!v) return false;
        double d = 0;
        if (v->is_integer()) d = double(v->as_integer()->get());
        else if (v->is_floating_point()) d = v->as_floating_point()->get();
        else return fail(key, "must be a number");
        if (!std::isfinite(d)) return fail(key, "must be a number");
        out = d;
        return ok();
    }

    bool numbers(const char* key, std::vector<double>& out) {
        const toml::node* v = take(key);
        if (!v) return false;
        if (!v->is_array()) return fail(key, "must be a list of numbers");
        std::vector<double> c;
        for (auto&& e : *v->as_array()) {
            if (e.is_integer()) c.push_back(double(e.as_integer()->get()));
            else if (e.is_floating_point() && std::isfinite(e.as_floating_point()->get())) c.push_back(e.as_floating_point()->get());
            else return fail(key, "must be a list of numbers");
        }
        out = std::move(c);
        return ok();
    }

    bool strings(const char* key, std::vector<std::string>& out, size_t max_items, size_t max_len) {
        const toml::node* v = take(key);
        if (!v) return false;
        if (!v->is_array()) return fail(key, "must be a list of strings");
        const toml::array& a = *v->as_array();
        if (a.size() > max_items) return fail(key, "has too many entries (at most " + std::to_string(max_items) + ")");
        std::vector<std::string> items;
        for (auto&& e : a) {
            if (!e.is_string() || e.as_string()->get().empty() || e.as_string()->get().size() > max_len)
                return fail(key, "must be a list of non-empty strings");
            items.emplace_back(e.as_string()->get());
        }
        out = std::move(items);
        return ok();
    }

    bool check(const char* key, const std::string& problem) {
        if (problem.empty()) return ok();
        return fail(key, problem);
    }

    bool fail(const std::string& key, const std::string& what) {
        if (!error_.empty()) return false;
        std::string where = path_;
        if (!key.empty()) where += where.empty() ? key : "." + key;
        error_ = (where.empty() ? std::string("the file") : where) + ": " + what;
        return false;
    }

    void finish() {
        if (!error_.empty() || !t_) return;
        for (auto&& [k, v] : *t_) {
            (void)v;
            std::string key(k.str());
            if (!used_.count(key)) {
                fail(key, "unknown key");
                return;
            }
        }
    }

    void adopt(const std::string& e) {
        if (error_.empty()) error_ = e;
    }

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

private:
    const toml::table* t_;
    std::string path_;
    std::set<std::string> used_;
    std::string error_;
};

}
