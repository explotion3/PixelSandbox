#include "simulation/electrical_simulation.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <stdexcept>
#include "simulation/combustion.hpp"

namespace pixel {
namespace {
void normalize_previous(ElectricPulse& p){
    if(!p.source_rules && p.body==p.previous_body && std::abs(p.position.x-p.previous.x)+std::abs(p.position.y-p.previous.y)>1){p.previous=p.position;p.previous_body=p.body;}
}
}
CommandResult ElectricalSimulation::inject(WorldState& w,const ContentRegistry& content,const WorldCommand& command){
    ElectricAddress seed{};bool source_rules=false;CellPos wake=command.target;double best=command.radius*command.radius+1.0;
    for(int y=-command.radius;y<=command.radius;++y)for(int x=-command.radius;x<=command.radius;++x){
        const int distance=x*x+y*y;if(distance>=best || distance>command.radius*command.radius)continue;
        const CellPos p{command.target.x+x,command.target.y+y};const auto m=w.material_at(p);
        if(!m || *m==Material::air || w.body_pixel_at(p))continue;
        const auto* d=content.find_material(w.material_ids_[static_cast<std::uint16_t>(*m)-1]);
        if(d->electrical_conductivity){seed={p,0};wake=p;best=distance;source_rules=d->source_electricity;}
    }
    for(const auto& body:w.bodies_)for(const auto& pixel:body.pixels){
        const auto* d=content.find_material(w.material_ids_[pixel.material-1]);if(!d->electrical_conductivity)continue;
        const auto c=std::cos(body.angle),s=std::sin(body.angle),x=body.x+c*pixel.local.x-s*pixel.local.y,y=body.y+s*pixel.local.x+c*pixel.local.y;
        const auto dx=x-command.target.x-0.5,dy=y-command.target.y-0.5,distance=dx*dx+dy*dy;
        if(distance>=best || distance>command.radius*command.radius)continue;
        source_rules=d->source_electricity;seed={pixel.local,body.id};wake={static_cast<int>(std::floor(x)),static_cast<int>(std::floor(y))};best=distance;
    }
    if(best>command.radius*command.radius)return {command.kind,false,0,"No conductive material inside the electrical brush."};
    auto existing=std::find_if(w.electric_pulses_.begin(),w.electric_pulses_.end(),[&](const auto& p){return p.position==seed.position && p.body==seed.body;});
    std::uint32_t added=1000;
    if(existing!=w.electric_pulses_.end()){added=std::min(added,10000-existing->energy);existing->energy+=added;}
    else{ElectricPulse p;p.position=p.previous=seed.position;p.body=p.previous_body=seed.body;
        p.source_rules=source_rules;
        if(p.source_rules){CombustionRandom random(static_cast<std::uint32_t>(w.seed_^w.tick_^static_cast<unsigned>(wake.x*31+wake.y)));
            const float angle=static_cast<float>(random.next())/32767.0f*6.2831854820251465f;p.avg_x=std::cos(angle);p.avg_y=std::sin(angle);
            p.splittings_left=2;p.splitting_energy=500;p.glow_ticks=10; // Explicit project brush parameters, not original defaults.
        }
        w.electric_pulses_.push_back(p);}
    w.electric_energy_+=added;w.wake(wake);
    return {command.kind,added!=0,added,"Injected "+std::to_string(added)+" electrical energy into a conductor."};
}
void ElectricalSimulation::transfer(WorldState& w,ElectricAddress from,ElectricAddress to){
    for(auto& p:w.electric_pulses_){
        if(p.position==from.position && p.body==from.body){p.position=to.position;p.body=to.body;}
        if(p.previous==from.position && p.previous_body==from.body){p.previous=to.position;p.previous_body=to.body;}
        normalize_previous(p);
    }
}
void ElectricalSimulation::transfer_batch(WorldState& w,std::span<const ElectricRelocation> changes){
    if(w.electric_pulses_.empty() || changes.empty())return;
    using Key=std::tuple<std::uint32_t,int,int>;
    auto key=[](ElectricAddress at){return Key{at.body,at.position.x,at.position.y};};
    std::map<Key,ElectricAddress> destinations;
    for(const auto& change:changes)if(!destinations.emplace(key(change.from),change.to).second)
        throw std::logic_error("Duplicate electrical source in material relocation batch.");
    for(auto& pulse:w.electric_pulses_){
        // Read both old addresses before applying the permutation. Sequential
        // transfers could move a head twice or erase a still-adjacent history.
        const auto current=destinations.find(key({pulse.position,pulse.body}));
        const auto previous=destinations.find(key({pulse.previous,pulse.previous_body}));
        if(current!=destinations.end()){pulse.position=current->second.position;pulse.body=current->second.body;}
        if(previous!=destinations.end()){pulse.previous=previous->second.position;pulse.previous_body=previous->second.body;}
        normalize_previous(pulse);
    }
}
void ElectricalSimulation::reconcile(WorldState& w,std::span<const RigidSimulation::FragmentLink> links){
    auto exists=[&](std::uint32_t id,CellPos local){const auto b=std::find_if(w.bodies_.begin(),w.bodies_.end(),[&](const auto& body){return body.id==id;});return b!=w.bodies_.end() && std::any_of(b->pixels.begin(),b->pixels.end(),[&](const auto& p){return p.material && p.local==local;});};
    auto remap=[&](CellPos position,std::uint32_t& id){
        if(!id)return w.bounds_.contains(position);
        if(exists(id,position))return true;
        for(const auto& link:links)if(link.parent==id && exists(link.child,position)){id=link.child;return true;}
        return false;
    };
    std::uint64_t lost=0;
    w.electric_pulses_.erase(std::remove_if(w.electric_pulses_.begin(),w.electric_pulses_.end(),[&](auto& p){
        if(!remap(p.position,p.body)){lost+=p.energy;return true;}
        if(!remap(p.previous,p.previous_body)){p.previous=p.position;p.previous_body=p.body;}
        normalize_previous(p);return false;
    }),w.electric_pulses_.end());
    w.electric_energy_-=lost;w.simulation_stats_.electric_spent+=lost;
}
}
