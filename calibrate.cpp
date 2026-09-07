#include "screen_layout.h"

#include <cmath>
#include <iostream>
#include <optional>
#include <string>

namespace {

std::optional<POINT> waitForLeftClick(const std::string& instruction) {
    std::cout << instruction << '\n'
              << "Left-click to record the point, or press Escape to cancel." << std::endl;

    while (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        Sleep(10);
    }

    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            return std::nullopt;
        }
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
            POINT point{};
            if (!GetCursorPos(&point)) {
                std::cerr << "Could not read the mouse position." << std::endl;
                return std::nullopt;
            }
            while (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
                Sleep(10);
            }
            std::cout << "Recorded (" << point.x << ", " << point.y << ")\n\n";
            return point;
        }
        Sleep(10);
    }
}

} // namespace

int main() {
    SetProcessDPIAware();
    std::cout
        << "Tetris screen calibration\n"
        << "Keep the game visible and paused while selecting each point.\n"
        << "For the board, click the two OUTER grid corners.\n"
        << "For NEXT, select only the black preview interior; exclude its white header and border.\n\n";

    const auto boardTopLeft = waitForLeftClick("1/4: Select the board's top-left outer corner.");
    if (!boardTopLeft) return 1;
    const auto boardBottomRight = waitForLeftClick("2/4: Select the board's bottom-right outer corner.");
    if (!boardBottomRight) return 1;
    const auto queueTopLeft = waitForLeftClick("3/4: Select the top-left of the NEXT preview interior.");
    if (!queueTopLeft) return 1;
    const auto queueBottomRight = waitForLeftClick("4/4: Select the bottom-right of the NEXT preview interior.");
    if (!queueBottomRight) return 1;

    const ScreenLayout layout{
        makeScreenRect(*boardTopLeft, *boardBottomRight),
        makeScreenRect(*queueTopLeft, *queueBottomRight),
    };

    if (!layout.isValid()) {
        std::cerr << "One of the selected regions has no area. Please run calibration again.\n";
        return 1;
    }

    const double cellWidth = static_cast<double>(layout.board.width()) / 10.0;
    const double cellHeight = static_cast<double>(layout.board.height()) / 20.0;
    if (std::abs(cellWidth - cellHeight) > std::max(cellWidth, cellHeight) * 0.08) {
        std::cout
            << "Warning: the selected board gives cells of "
            << cellWidth << " x " << cellHeight
            << " pixels. Check that you selected the outer grid corners.\n";
    }

    std::string error;
    const auto path = defaultScreenLayoutPath();
    if (!saveScreenLayout(path, layout, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout
        << "Calibration saved to " << path.string() << '\n'
        << "Board cell size: " << cellWidth << " x " << cellHeight << " pixels\n"
        << "You can now run color.exe.\n";
    return 0;
}
