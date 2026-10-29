#pragma once
#include "core/coordinates.hpp"
#include "simulation/combustion.hpp"
#include <bit>

namespace pixel {
// Local-build RVA 0x30ACC0. Arithmetic shifts and wrapping x86 multiplies
// matter at negative coordinates; this is independent of the world RNG.
inline std::uint8_t source_attachment_direction(CellPos p){
    auto bits=static_cast<std::uint32_t>(p.x)+0x67577458u+static_cast<std::uint32_t>(p.y)*91u;
    bits^=static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(bits)>>13);
    bits=bits*214013u+2531011u;
    bits^=static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(bits)>>((static_cast<std::uint32_t>(p.x)&15u)+1u));
    const auto choice=(bits&32767u)%3u;
    const auto direction=p.y%2==0?7+static_cast<int>(choice):5-static_cast<int>(choice);
    return static_cast<std::uint8_t>(direction==5?5:direction==7?7:6);
}
inline CellPos source_attachment_support(CellPos p,std::uint8_t direction){
    return direction==5?CellPos{p.x+1,p.y}:direction==7?CellPos{p.x-1,p.y}:CellPos{p.x,p.y-1};
}
// Recovered full-update branch at RVA 0x30C725..0x30C828. The caller owns
// update scheduling and supplies the resolved support/empty-below predicates.
inline bool source_attachment_releases(CombustionRandom& random,int ceiling,bool supported,bool empty_below){
    if(random.next()%101u>=20u || ceiling<=0)return false;
    if(!supported)return true;
    return static_cast<int>(random.next()%101u)<ceiling && empty_below;
}
}
