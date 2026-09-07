#pragma once

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

struct ScreenRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int width() const {
        return right - left;
    }

    int height() const {
        return bottom - top;
    }

    bool isValid() const {
        return width() > 0 && height() > 0;
    }
};

inline ScreenRect makeScreenRect(POINT first, POINT second) {
    return {
        std::min(first.x, second.x),
        std::min(first.y, second.y),
        std::max(first.x, second.x),
        std::max(first.y, second.y),
    };
}

struct ScreenLayout {
    ScreenRect board;
    ScreenRect nextQueue;

    bool isValid() const {
        return board.isValid() && nextQueue.isValid();
    }
};

inline std::filesystem::path executableDirectory() {
    wchar_t executablePath[32768];
    const DWORD length = GetModuleFileNameW(nullptr, executablePath, 32768);
    if (length == 0 || length == 32768) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(executablePath).parent_path();
}

inline std::filesystem::path defaultScreenLayoutPath() {
    return executableDirectory() / "screen_layout.cfg";
}

inline bool saveScreenLayout(
    const std::filesystem::path& path,
    const ScreenLayout& layout,
    std::string& error
) {
    if (!layout.isValid()) {
        error = "The selected rectangles are invalid.";
        return false;
    }

    std::ofstream output(path);
    if (!output) {
        error = "Could not open the calibration file for writing: " + path.string();
        return false;
    }

    output << "board "
           << layout.board.left << ' ' << layout.board.top << ' '
           << layout.board.right << ' ' << layout.board.bottom << '\n';
    output << "next_queue "
           << layout.nextQueue.left << ' ' << layout.nextQueue.top << ' '
           << layout.nextQueue.right << ' ' << layout.nextQueue.bottom << '\n';

    if (!output) {
        error = "Could not write the calibration file: " + path.string();
        return false;
    }
    return true;
}

inline bool loadScreenLayout(
    const std::filesystem::path& path,
    ScreenLayout& layout,
    std::string& error
) {
    std::ifstream input(path);
    if (!input) {
        error = "Calibration file not found: " + path.string();
        return false;
    }

    std::string boardLabel;
    std::string queueLabel;
    ScreenLayout loaded;
    if (!(input >> boardLabel
                >> loaded.board.left >> loaded.board.top
                >> loaded.board.right >> loaded.board.bottom
                >> queueLabel
                >> loaded.nextQueue.left >> loaded.nextQueue.top
                >> loaded.nextQueue.right >> loaded.nextQueue.bottom)
        || boardLabel != "board"
        || queueLabel != "next_queue"
        || !loaded.isValid()) {
        error = "Calibration file is malformed: " + path.string();
        return false;
    }

    layout = loaded;
    return true;
}
