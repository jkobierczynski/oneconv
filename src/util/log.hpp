// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Minimal logging: warnings are counted so the CLI can report a summary.
#pragma once

#include <atomic>
#include <iostream>
#include <sstream>
#include <string>

namespace oneconv {

enum class LogLevel { Quiet = 0, Normal = 1, Verbose = 2, Debug = 3 };

class Log {
public:
    static LogLevel& level() {
        static LogLevel l = LogLevel::Normal;
        return l;
    }
    static std::atomic<int>& warning_count() {
        static std::atomic<int> c{0};
        return c;
    }
    static void warn(const std::string& msg) {
        ++warning_count();
        if (level() >= LogLevel::Normal) std::cerr << "warning: " << msg << "\n";
    }
    static void info(const std::string& msg) {
        if (level() >= LogLevel::Normal) std::cerr << msg << "\n";
    }
    static void verbose(const std::string& msg) {
        if (level() >= LogLevel::Verbose) std::cerr << msg << "\n";
    }
    static void debug(const std::string& msg) {
        if (level() >= LogLevel::Debug) std::cerr << "debug: " << msg << "\n";
    }
    static void error(const std::string& msg) { std::cerr << "error: " << msg << "\n"; }
};

template <typename... Args>
std::string cat(Args&&... args) {
    std::ostringstream os;
    (os << ... << args);
    return os.str();
}

}  // namespace oneconv
