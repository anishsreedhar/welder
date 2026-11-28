// Shared Save As transaction: a saved destination is never an obsolete autosave.
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace strata::savefile {
namespace fs = std::filesystem;
// Write a file so that a crash (or a full disk) part-way through never leaves it half written:
// into a temporary file next to it, then swapped in. If the swap can't be done (a file locked
// on a network share), the file is written in place as before.
inline bool writeAtomic(const std::string& path, const std::string& text, std::string* err) {
    const std::string tmp = path + ".saving";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (f) {
            f << text;
            f.flush();
        }
        if (!f) {
            std::error_code ec;
            fs::remove(tmp, ec);
            std::ofstream g(path, std::ios::binary);  // (a folder that can't take a new file)
            g << text;
            g.flush();
            if (!g && err) *err = "cannot write " + path;
            return (bool)g;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {  // (on some systems a rename won't replace a file)
        std::error_code ec2;
        fs::remove(path, ec2);
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        std::ofstream g(path, std::ios::binary);
        g << text;
        g.flush();
        if (!g && err) *err = "cannot write " + path + ": " + ec.message();
        return (bool)g;
    }
    return true;
}

inline bool samePath(const std::string& a, const std::string& b) {
    std::error_code ec;
    if (fs::equivalent(a, b, ec) && !ec) return true;
    auto key = [](const std::string& path) {
        std::error_code error;
        auto p = fs::weakly_canonical(fs::absolute(path, error), error);
        if (error) p = fs::path(path).lexically_normal();
        auto text = p.generic_string();
#ifdef _WIN32
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
#endif
        return text;
    };
    return key(a) == key(b);
}
// The caller must first join any outstanding autosave writer.
inline bool saveComp(const std::string& destination, const std::string& text,
                     const std::vector<std::string>& obsoleteAutosaves, std::string* err) {
    if (!writeAtomic(destination, text, err)) return false;
    for (const auto& autosave : obsoleteAutosaves) {
        if (autosave.empty() || samePath(autosave, destination)) continue;
        std::error_code ec;
        fs::remove(autosave, ec);
    }
    return true;
}
} // namespace strata::savefile
