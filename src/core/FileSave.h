// Shared Save As transaction: a saved destination is never an obsolete autosave.
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>
#include "nlohmann/json.hpp"
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
struct RecoveryFile {
    std::string path;
    std::string owner;
};

inline bool matchesOwner(const nlohmann::json& doc, const std::string& owner) {
    const auto it = doc.find("autosave_of");
    if (it == doc.end() || !it->is_string()) return false;
    const auto& recorded = it->get_ref<const std::string&>();
    return recorded == owner || (!owner.empty() && !recorded.empty() && samePath(recorded, owner));
}

inline bool ownedRecovery(const RecoveryFile& recovery) {
    if (recovery.path.empty() || (!recovery.owner.empty() && samePath(recovery.path, recovery.owner))) return false;
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(recovery.path, ec))) return false;
    std::ifstream file(recovery.path, std::ios::binary);
    return file && matchesOwner(nlohmann::json::parse(file, nullptr, false), recovery.owner);
}

// Prefer an existing recovery, including a fallback used on the previous run.
// Occupied names (including broken symlinks and unreadable files) belong to their users.
inline std::string recoveryPath(const std::string& preferred, const std::string& fallback,
                                const std::string& owner) {
    std::string vacant;
    for (const auto& path : {preferred, fallback}) {
        if (!owner.empty() && samePath(path, owner)) continue;
        std::error_code ec;
        if (fs::symlink_status(path, ec).type() == fs::file_type::not_found) {
            if (vacant.empty()) vacant = path;
        } else if (ownedRecovery({path, owner})) {
            return path;
        }
    }
    return vacant;
}

inline void removeRecovery(const RecoveryFile& recovery) {
    if (ownedRecovery(recovery)) {
        std::error_code ec;
        fs::remove(recovery.path, ec);
    }
}

// Claim a new name exclusively. Another editor or an artist may have saved there
// since recoveryPath selected it. Only an existing recovery of this comp is replaceable.
inline bool writeRecovery(const RecoveryFile& recovery, const std::string& text, std::string* err) {
#ifdef _WIN32
    auto* file = _wfopen(fs::path(recovery.path).c_str(), L"wbx");
#else
    auto* file = std::fopen(recovery.path.c_str(), "wbx");
#endif
    if (file) {
        const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
        const bool closed = std::fclose(file) == 0;
        if (written && closed) return true;
    } else if (ownedRecovery(recovery)) {
        return writeAtomic(recovery.path, text, err);
    }
    if (err) *err = "cannot write recovery file " + recovery.path;
    return false;
}

// The caller must first join any outstanding autosave writer.
inline bool saveComp(const std::string& destination, const std::string& text,
                     const RecoveryFile& before, const RecoveryFile& after, std::string* err) {
    if (!writeAtomic(destination, text, err)) return false;
    for (const auto& autosave : {before, after}) {
        if (!autosave.path.empty() && !samePath(autosave.path, destination) &&
            (before.owner.empty() || !samePath(autosave.path, before.owner)))
            removeRecovery(autosave);
    }
    return true;
}
} // namespace strata::savefile
