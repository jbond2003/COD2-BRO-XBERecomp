#pragma once
#include <algorithm>
#include <cstdint>

namespace game {

// cg_fov: horizontal degrees on a 4:3 picture, 70 the game's own (src/game/fov.cpp)
inline constexpr uint32_t kDefaultFov = 70;
inline constexpr uint32_t kMinFov = 60;
inline constexpr uint32_t kMaxFov = 110;

inline uint32_t ClampFov(uint32_t degrees) { return std::clamp(degrees, kMinFov, kMaxFov); }

}  // namespace game
