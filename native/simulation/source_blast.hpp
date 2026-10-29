#pragma once
#include "core/coordinates.hpp"
#include "simulation/combustion.hpp"
#include <cmath>
#include <optional>

namespace pixel {
struct SourceBlastObstacle {int hp=0,durability=0;bool blocks=false;};
struct SourceBlastProbe {float distance_squared=0;int max_spent=0;};
// Probe reads current HP without changing cells. Destruction is a separate pass.
template<class Lookup>
SourceBlastProbe source_blast_probe(float start_x,float start_y,float end_x,float end_y,int energy,int hardness,CombustionRandom& random,Lookup&& lookup){
    SourceBlastProbe result;float dx=end_x-start_x,dy=end_y-start_y;
    const float squared=dx*dx+dy*dy;const int limit=std::min(4194304,static_cast<int>(squared+0.5f));if(limit<=0)return result;
    const float length=std::sqrt(squared);dx/=length;dy/=length;float x=start_x,y=start_y;CellPos previous{static_cast<int>(x),static_cast<int>(y)};
    for(int step=0;step<2048 && result.distance_squared<limit;++step){
        x+=dx;y+=dy;const CellPos q{static_cast<int>(x),static_cast<int>(y)};
        result.distance_squared=(x-start_x)*(x-start_x)+(y-start_y)*(y-start_y);
        if(q==previous)continue;previous=q;
        const auto cell=lookup(q);if(!cell || !cell->blocks)continue;
        const int spent=std::min(energy,std::max(0,cell->hp));energy-=spent;result.max_spent=std::max(result.max_spent,spent);
        if(cell->durability>hardness || !energy){if(random.next()%2==0)result.distance_squared+=1;return result;}
    }
    return result;
}
}
