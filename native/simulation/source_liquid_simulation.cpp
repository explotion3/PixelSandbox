#include "simulation/source_liquid_simulation.hpp"
#include "simulation/combustion.hpp"
#include "simulation/electrical_simulation.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace pixel {
namespace {
std::uint32_t cell_random(std::uint64_t seed, std::uint64_t tick, CellPos p) {
    auto n=seed ^ (tick*0x9e3779b97f4a7c15ULL) ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x))<<32) ^ static_cast<std::uint32_t>(p.y);
    n=(n^(n>>30))*0xbf58476d1ce4e5b9ULL;n=(n^(n>>27))*0x94d049bb133111ebULL;n^=n>>31;
    return static_cast<std::uint32_t>(n^(n>>32));
}
}
void SourceLiquidSimulation::step(WorldState& w, const ContentRegistry& content,
    std::span<const CellPos> active, std::span<const std::uint8_t> attached,
    std::span<std::uint8_t> updated, std::span<std::uint8_t> transported) {
    std::vector<const MaterialDefinition*> defs{nullptr};
    bool present=false;
    for(const auto& id:w.material_ids_){const auto* d=content.find_material(id);defs.push_back(d);present=present || (d->source_liquid && w.counts_[defs.size()-1]);}
    if(!present)return;
    const auto& bounds=w.bounds_;
    auto index=[&](CellPos p){return static_cast<std::size_t>(p.y-bounds.origin.y)*bounds.width+p.x-bounds.origin.x;};
    for(auto p:active){
        const auto material=*w.material_at(p);
        if(material==Material::air || updated[index(p)] || attached[index(p)] || w.body_pixel_at(p))continue;
        const auto* d=defs[static_cast<std::uint16_t>(material)];
        if(!d->source_liquid)continue;
        auto state=*w.cell_state(p);
        // Project scheduling: deterministic per-cell stream and canonical traversal.
        // The original world/worker RNG scheduling is not yet recovered.
        CombustionRandom random(cell_random(w.seed_,w.tick_,p));
        bool pending=false,moved=false;
        auto enter=[&](CellPos q){
            if(!bounds.contains(q) || w.body_pixel_at(q) || attached[index(q)])return false;
            const auto other=*w.material_at(q);
            // A stationary parcel may already have had its turn this tick.
            // It must still be exchangeable by a denser parcel above it.
            // Only defer destinations that were actually transported.
            if(transported[index(q)]){
                if(other==Material::air)pending=true;
                else {const auto* target=defs[static_cast<std::uint16_t>(other)];const auto motion=target->motion(w.cell_state(q)->mobilized);
                    pending=pending || motion==Motion::gas || motion==Motion::fire || (motion!=Motion::fixed && motion!=Motion::rigid && d->density>target->density);}
                return false;
            }
            if(other==Material::air)return true;
            const auto* target=defs[static_cast<std::uint16_t>(other)];
            const auto motion=target->motion(w.cell_state(q)->mobilized);
            if(motion==Motion::gas || motion==Motion::fire)return true;
            // Keep fixed terrain a boundary even though Noita's class hierarchy
            // also represents much terrain as CLiquidCell.
            if(motion==Motion::fixed || motion==Motion::rigid)return false;
            const bool denser=target->cell_type=="liquid" && d->density>target->density;
            pending=pending || denser; // A failed random gate must get another chance.
            if(random.next()%101<75)return false;
            return denser;
        };
        auto swap=[&](CellPos q){
            const auto other=*w.material_at(q);
            auto displaced=*w.cell_state(q);
            state.source_attachment=0;state.adhesion_ticks=0;
            if(other!=Material::air){
                displaced.source_attachment=0;displaced.adhesion_ticks=0;
                const auto* target=defs[static_cast<std::uint16_t>(other)];
                if(target->source_gas && target->lifetime)++displaced.gas_counter;
            }
            w.set_cell(p,other,displaced);w.set_cell(q,material,state);
            if(!w.electric_pulses_.empty()){
                const std::array<ElectricRelocation,2> changes{{{{p,0},{q,0}},{{q,0},{p,0}}}};
                ElectricalSimulation::transfer_batch(w,changes);
            }
            updated[index(p)]=other==Material::air?0:1;updated[index(q)]=1;
            transported[index(p)]=other==Material::air?0:1;transported[index(q)]=1;
            p=q;moved=true;++w.simulation_stats_.moved_cells;
        };
        auto attempt=[&](CellPos q){if(!enter(q))return false;swap(q);return true;};
        // Explicit project bridge for existing impulse commands and older saves.
        // These physical velocity fields are consumed once, never run through
        // the pressure solver or legacy row-outlet transport for this material.
        state.liquid_lateral+=static_cast<float>((state.velocity_x+state.momentum_remainder_x/256.0)/256.0);
        const auto external_y=(state.velocity_y+state.momentum_remainder_y/256.0)/256.0;
        if(external_y<0){
            for(int n=0;n<static_cast<int>(std::ceil(-external_y));++n)if(!attempt({p.x,p.y-1}))break;
        }else state.liquid_fall+=static_cast<float>(external_y);
        state.velocity_x=state.velocity_y=0;state.momentum_remainder_x=state.momentum_remainder_y=0;
        state.vertical_fraction=state.horizontal_fraction=0;state.liquid_fraction_x=state.liquid_fraction_y=0;
        bool fell=false;
        if(external_y>=0 && attempt({p.x,p.y+1})){
            fell=true;state.liquid_direction=1;
            state.liquid_fall+=d->liquid_gravity*0.25f;
            float remaining=state.liquid_fall-1;
            // Each successful move checks the intervening cell: no tunnelling.
            // Lateral choice here is a project adaptation of the recovered
            // stochastic extra-fall branch, not the original full RNG sequence.
            while(remaining>0){
                const int dx=state.liquid_lateral>=1?1:state.liquid_lateral<=-1?-1:static_cast<int>(random.next()%3)-1;
                if(!attempt({p.x+dx,p.y+1})){
                    if(dx==0 || !attempt({p.x,p.y+1})){state.liquid_fall=0;break;}
                }
                remaining-=2;
            }
        }else if(external_y>=0)state.liquid_fall=0;
        if(!fell && external_y>=0){
            const bool right=enter({p.x+1,p.y}),left=enter({p.x-1,p.y});
            bool lateral=false;
            if(right && !left)state.liquid_lateral+=d->liquid_flow_speed;
            else if(left && !right)state.liquid_lateral-=d->liquid_flow_speed;
            else if(left && right && std::abs(state.liquid_lateral)<1){
                // Stable per-cell random choice; the original caller ordering
                // remains unknown. Do not add a global lower-outlet search.
                state.liquid_lateral+=(random.next()&1?1.0f:-1.0f)*d->liquid_flow_speed;
            }
            // The one-sided branch already sampled the destination predicate.
            // Commit that candidate without imposing a second density gate.
            if((right && !left && state.liquid_lateral>=1) ||
               (left && !right && state.liquid_lateral<=-1)){
                swap({p.x+(right?1:-1),p.y});
                state.liquid_lateral*=d->liquid_damping;lateral=true;state.liquid_direction=2;
            }
            while(std::abs(state.liquid_lateral)>=1){
                const int dx=state.liquid_lateral>0?1:-1;
                if(!attempt({p.x+dx,p.y})){state.liquid_lateral=0;break;}
                state.liquid_lateral*=d->liquid_damping;lateral=true;state.liquid_direction=2;
            }
            if(!lateral){
                const int side=random.next()&1?1:-1;
                if(attempt({p.x+side,p.y+1}) || attempt({p.x-side,p.y+1}))state.liquid_direction=1;
                else if(!left && !right){state.liquid_direction=3;state.liquid_lateral=0;}
                else pending=true; // Fractional lateral credit must accumulate.
            }
        }
        w.set_cell(p,material,state);updated[index(p)]=1;
        if(pending || moved)w.wake(p);
    }
}
}
