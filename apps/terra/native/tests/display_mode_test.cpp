#include "display_mode.h"

#include <array>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    using terra::DisplayModeSpec;
    constexpr std::array modes{
        DisplayModeSpec{3840, 2160, 60}, DisplayModeSpec{3840, 2160, 120},
        DisplayModeSpec{2560, 1440, 120}, DisplayModeSpec{1920, 1080, 120},
        DisplayModeSpec{1920, 1080, 144}, DisplayModeSpec{1920, 1200, 120},
    };

    expect(terra::displayModeMatchesFrameRate(119, 60),
           "Rounded refresh rate did not match stream cadence.");
    expect(!terra::displayModeMatchesFrameRate(144, 60),
           "Non-divisible refresh rate matched stream cadence.");
    expect(!terra::displayModeMatchesFrameRate(61, 60),
           "Higher mismatched refresh rate was treated as a fractional alias.");
    expect(terra::selectOptimalDisplayMode(modes, {3840, 2160, 60}, 1920, 1080, 60) ==
               DisplayModeSpec{3840, 2160, 120},
           "Highest cadence-matched desktop mode was not selected.");
    expect(terra::selectOptimalDisplayMode(modes, {2560, 1600, 60}, 1920, 1080, 120) ==
               DisplayModeSpec{1920, 1080, 120},
           "Closest lower-resolution aspect match was not selected.");
    expect(terra::selectOptimalDisplayMode(modes, {3840, 2160, 60}, 1920, 1080, 100) ==
               DisplayModeSpec{3840, 2160, 60},
           "Desktop mode was not retained when no cadence match existed.");
}
