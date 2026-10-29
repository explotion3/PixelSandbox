#pragma once
#include "core/coordinates.hpp"
#include <cmath>

namespace pixel {
// Direct-world geometry only. Callers supply accessibility, material reads and
// staged writes; threaded request consumption is not inferred here.
template<class Read,class Write>
void source_reaction_blob(CellPos center,int radius,bool restricted,int input,int output,Read&& read,Write&& write){
    if(!restricted){
        for(int y=-radius;y<=radius;++y)for(int x=-radius;x<=radius;++x){
            const CellPos p{center.x+x,center.y+y};if(x*x+y*y<radius*radius && read(p))write(p);
        }
        return;
    }
    const auto origin=read(center);if(input==output || !origin || *origin!=input)return;
    write(center);if(!radius)return;
    constexpr float two_pi=6.2831854820251465f;
    const int rays=static_cast<int>(radius*two_pi+0.5f);const float step=two_pi/rays;float angle=0;
    for(int ray=0;ray<rays;++ray){
        const float dx=static_cast<float>(std::cos(static_cast<double>(angle))),dy=static_cast<float>(std::sin(static_cast<double>(angle)));angle+=step;
        float x=static_cast<float>(center.x)+dx,y=static_cast<float>(center.y)+dy;
        for(int distance=0;distance<radius;++distance){
            const CellPos p{static_cast<int>(x),static_cast<int>(y)};const auto value=read(p);if(!value)break;
            if(*value==input)write(p);else if(*value!=output)break;
            x+=dx;y+=dy;
        }
    }
}
}
