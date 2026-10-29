#pragma once
#include "content/content_registry.hpp"
#include "core/world_snapshot.hpp"
#include <algorithm>
#include <bit>

namespace pixel {
// The recovered 15-bit generator. Stream seeding/scheduling is project-owned;
// no global RNG or additional unsaved state is introduced.
class CombustionRandom final {
    std::uint32_t state_;
public:
    explicit CombustionRandom(std::uint32_t seed):state_(seed){}
    std::uint32_t next(){state_=state_*214013u+2531011u;return (state_>>16)&0x7fffu;}
    std::uint32_t percent(){return next()%101u;}
    int neighbor(){next();return static_cast<int>((state_>>16)&3u);}
};
inline int combustion_intensity(const CellState& s){return std::bit_cast<std::int8_t>(s.fire_intensity);}
inline void force_combustion(const MaterialDefinition& d,CellState& s){
    if(!d.source_burning || !d.burnable || (d.explosion.radius && !s.fuel))return;
    s.burning=true;s.fire_intensity=static_cast<std::uint8_t>(d.burn_heat);s.ignition_pending=true;
}
inline void extinguish_combustion(CellState& s){s.burning=false;s.ignition_pending=false;}
inline std::uint32_t char_color(std::uint32_t color,int intensity,CombustionRandom& random){
    const int red=(color>>16)&255,green=(color>>8)&255,blue=color&255;
    if(std::max({red,green,blue})<19)return color;
    const double factor=intensity*(0.0045+0.001*random.next()/32767.0);
    std::uint32_t result=color&0xff000000u;
    for(int shift:{16,8,0})result|=static_cast<std::uint32_t>(std::clamp(static_cast<int>(((color>>shift)&255)*(1-factor)),19,255))<<shift;
    return result;
}
inline std::uint32_t fade_burning_color(std::uint32_t color){
    std::uint32_t result=color&0xff000000u;for(int shift:{16,8,0}){const auto channel=(color>>shift)&255;result|=(channel?channel-1:0)<<shift;}return result;
}
}
