#include "simulation/rigid_simulation.hpp"
#include "simulation/material_simulation.hpp"
#include "simulation/electrical_simulation.hpp"
#include "simulation/combustion.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace pixel {
namespace {
std::vector<std::vector<RigidPixel>> components(const std::vector<RigidPixel>& pixels){
    std::vector<bool> seen(pixels.size(),false);std::vector<std::vector<RigidPixel>> result;
    for(std::size_t start=0;start<pixels.size();++start){
        if(seen[start] || !pixels[start].material)continue;
        std::vector<std::size_t> queue{start};seen[start]=true;std::vector<RigidPixel> group;
        for(std::size_t k=0;k<queue.size();++k){
            const auto& p=pixels[queue[k]];group.push_back(p);
            for(std::size_t j=0;j<pixels.size();++j)if(!seen[j] && pixels[j].material && std::abs(p.local.x-pixels[j].local.x)+std::abs(p.local.y-pixels[j].local.y)==1){seen[j]=true;queue.push_back(j);}
        }
        result.push_back(std::move(group));
    }
    return result;
}
std::pair<double,double> centroid(const std::vector<RigidPixel>& pixels,const std::vector<const MaterialDefinition*>& defs){
    double x=0,y=0,mass=0;for(const auto& p:pixels){if(!p.material)continue;const auto weight=defs[p.material]->density;x+=p.local.x*weight;y+=p.local.y*weight;mass+=weight;}return mass?std::pair{x/mass,y/mass}:std::pair{0.0,0.0};
}
}
std::vector<std::vector<RigidPixel>> RigidSimulation::connected_parts(const std::vector<RigidPixel>& pixels){return components(pixels);}
std::vector<RigidSimulation::ShapeBefore> RigidSimulation::shape_centers(const WorldState& w,const ContentRegistry& content){
    std::vector<const MaterialDefinition*> defs{nullptr};for(const auto& id:w.material_ids_)defs.push_back(content.find_material(id));
    std::vector<ShapeBefore> result;result.reserve(w.bodies_.size());
    for(const auto& b:w.bodies_){const auto [x,y]=centroid(b.pixels,defs);result.push_back({x,y,b.shape_revision});}return result;
}
std::vector<RigidSimulation::FragmentLink> RigidSimulation::finish_material_changes(WorldState& w,const ContentRegistry& content,const std::vector<ShapeBefore>& before){
    bool changed=false;for(std::size_t i=0;i<before.size();++i)changed|=before[i].revision!=w.bodies_[i].shape_revision;
    if(!changed)return {};
    std::vector<const MaterialDefinition*> defs{nullptr};for(const auto& id:w.material_ids_)defs.push_back(content.find_material(id));
    auto planned=w.bodies_;auto next=w.next_body_id_;std::vector<FragmentLink> links;
    for(std::size_t i=0;i<before.size();++i){
        auto& b=planned[i];if(before[i].revision==b.shape_revision)continue;
        b.pixels.erase(std::remove_if(b.pixels.begin(),b.pixels.end(),[](const auto& p){return !p.material;}),b.pixels.end());
        if(b.pixels.empty())continue;
        const auto [x,y]=centroid(b.pixels,defs);const auto c=std::cos(b.angle),s=std::sin(b.angle);
        const auto dx=c*(x-before[i].x)-s*(y-before[i].y),dy=s*(x-before[i].x)+c*(y-before[i].y);
        b.vx=std::clamp(b.vx-b.angular_velocity*dy,-512.0,512.0);b.vy=std::clamp(b.vy+b.angular_velocity*dx,-512.0,512.0);b.sleeping=false;
        auto groups=components(b.pixels);if(groups.size()==1)continue;
        const auto parent=b;
        for(std::size_t j=0;j<groups.size();++j){
            auto child=parent;child.pixels=std::move(groups[j]);const auto [cx,cy]=centroid(child.pixels,defs);
            const auto rx=c*(cx-x)-s*(cy-y),ry=s*(cx-x)+c*(cy-y);
            child.vx=std::clamp(parent.vx-parent.angular_velocity*ry,-512.0,512.0);child.vy=std::clamp(parent.vy+parent.angular_velocity*rx,-512.0,512.0);
            if(j){child.id=next++;links.push_back({child.id,parent.id});planned.push_back(std::move(child));}else planned[i]=std::move(child);
        }
    }
    planned.erase(std::remove_if(planned.begin(),planned.end(),[](const auto& b){return b.pixels.empty();}),planned.end());
    if(planned.size()>64 || next<w.next_body_id_)throw std::logic_error("Material fragments were not preflighted.");
    w.bodies_=std::move(planned);w.next_body_id_=next;++w.body_revision_;w.refresh_body_bounds();
    ElectricalSimulation::reconcile(w,links);
    return links;
}
CommandResult RigidSimulation::create(WorldState& w,const ContentRegistry& content,const WorldCommand& c,bool detach){
    auto fail=[&](const std::string& message){return CommandResult{c.kind,false,0,message};};
    if(!w.material_lab_ || c.radius<1 || c.radius>8 || !w.bounds_.contains(c.target))return fail("Rigid tools require the lab, an in-bounds target and radius 1..8.");
    const auto* selected=content.find_material(c.material_id);
    if(!detach && (!selected || (selected->motion()!=Motion::fixed && selected->motion()!=Motion::rigid)))return fail("Rigid pixels require a solid material.");
    if(!detach && selected->cell_type!="solid" && !selected->sand)return fail("Static liquids cannot form rigid bodies.");
    std::vector<RigidPixel> pixels;std::vector<CellPos> removed;
    for(int y=-c.radius;y<=c.radius;++y)for(int x=-c.radius;x<=c.radius;++x){
        if(x*x+y*y>c.radius*c.radius)continue;
        const CellPos at{c.target.x+x,c.target.y+y};
        if(!w.bounds_.contains(at))return fail("Rigid shape would leave the finite world.");
        if(w.body_pixel_at(at))return fail("Rigid shape overlaps another body.");
        auto material=detach?*w.material_at(at):w.material_index(c.material_id);
        if(detach && material==Material::air)continue;
        const auto* d=detach?content.find_material(w.material_ids_[static_cast<std::uint16_t>(material)-1]):selected;
        if(detach && (d->motion(w.cell_state(at)->mobilized)!=Motion::fixed || !d->sand))continue;
        if(!detach && w.material_at(at)!=Material::air)return fail("Rigid shape space is occupied; no source material was added.");
        auto state=detach?*w.cell_state(at):MaterialSimulation::initial_state(*d);
        if(state.adhesion_ticks || state.adhesion_cooldown || state.source_attachment)return fail("Detach cannot transfer adhesion state in this revision.");
        state.liquid_fall=state.liquid_lateral=0;state.liquid_direction=0;
        state.velocity_x=state.velocity_y=0;state.horizontal_fraction=state.vertical_fraction=0;
        state.momentum_remainder_x=state.momentum_remainder_y=0;
        state.created_tick=detach?state.created_tick:w.tick_;
        pixels.push_back({{x,y},static_cast<std::uint16_t>(material),state});if(detach)removed.push_back(at);
    }
    if(pixels.empty())return fail("No fixed solid pixels in the selected area.");
    auto groups=components(pixels);
    if(w.bodies_.size()+groups.size()>64 || w.next_body_id_>std::numeric_limits<std::uint32_t>::max()-groups.size())return fail("Rigid body capacity is 64; operation was not committed.");
    std::vector<RigidBodyState> additions;additions.reserve(groups.size());
    auto next=w.next_body_id_;
    for(auto& group:groups){RigidBodyState body;body.id=next++;body.x=c.target.x+0.5;body.y=c.target.y+0.5;body.pixels=std::move(group);additions.push_back(std::move(body));}
    w.bodies_.reserve(w.bodies_.size()+additions.size());
    for(const auto at:removed)w.set_cell(at,Material::air,{});
    for(auto& body:additions){
        for(const auto& p:body.pixels){++w.counts_[p.material];if(p.state.electrified_ticks)++w.electrified_cells_;if(p.material==1){++w.world_stone_;if(!detach)++w.initial_stone_;}
            if(detach)ElectricalSimulation::transfer(w,{{c.target.x+p.local.x,c.target.y+p.local.y},0},{p.local,body.id});}
        w.bodies_.push_back(std::move(body));
    }
    w.next_body_id_=next;++w.body_revision_;w.refresh_body_bounds();
    return {c.kind,true,pixels.size(),detach?"Detached actual terrain pixels; no inventory reward.":"Created a pixel rigid body; brush source recorded."};
}
CommandResult RigidSimulation::edit(WorldState& w,const ContentRegistry& content,const WorldCommand& c){
    if(c.kind!=CommandKind::erase && c.kind!=CommandKind::push && c.kind!=CommandKind::heat && c.kind!=CommandKind::cool)return {c.kind,false,0,"Rigid edit supports erase, push, heat and cool."};
    if(c.radius<1 || c.radius>8 || !w.bounds_.contains(c.target))return {c.kind,false,0,"Invalid rigid brush bounds/radius."};
    auto planned=w.bodies_;auto next=w.next_body_id_;std::uint64_t changed=0;
    std::vector<const MaterialDefinition*> defs{nullptr};for(const auto& id:w.material_ids_)defs.push_back(content.find_material(id));
    std::vector<RigidPixel> removed;std::vector<FragmentLink> links;
    const auto original_count=planned.size();
    for(std::size_t i=0;i<original_count;++i){
        auto& b=planned[i];const auto cosine=std::cos(b.angle),sine=std::sin(b.angle);
        const auto original=b.pixels;
        if(c.kind==CommandKind::erase){
            b.pixels.erase(std::remove_if(b.pixels.begin(),b.pixels.end(),[&](const auto& p){
                const auto dx=b.x+cosine*p.local.x-sine*p.local.y-(c.target.x+0.5),dy=b.y+sine*p.local.x+cosine*p.local.y-(c.target.y+0.5);
                if(dx*dx+dy*dy>c.radius*c.radius)return false;removed.push_back(p);++changed;return true;
            }),b.pixels.end());
            if(b.pixels.size()!=original.size()){
                ++b.shape_revision;
                if(!b.pixels.empty()){
                    const auto [old_x,old_y]=centroid(original,defs);const auto [x,y]=centroid(b.pixels,defs);
                    const auto dx=cosine*(x-old_x)-sine*(y-old_y),dy=sine*(x-old_x)+cosine*(y-old_y);
                    b.vx=std::clamp(b.vx-b.angular_velocity*dy,-512.0,512.0);b.vy=std::clamp(b.vy+b.angular_velocity*dx,-512.0,512.0);b.sleeping=false;
                }
            }
        }else if(c.kind==CommandKind::heat || c.kind==CommandKind::cool){
            for(auto& p:b.pixels){const auto dx=b.x+cosine*p.local.x-sine*p.local.y-c.target.x-0.5,dy=b.y+sine*p.local.x+cosine*p.local.y-c.target.y-0.5;
                if(dx*dx+dy*dy>c.radius*c.radius)continue;
                p.state.temperature=c.kind==CommandKind::heat?120:-30;if(c.kind==CommandKind::cool)extinguish_combustion(p.state);else force_combustion(*content.find_material(w.material_ids_[p.material-1]),p.state);++changed;
            }
        }else if(c.kind==CommandKind::push){
            bool hit=false;for(const auto& p:b.pixels){const auto dx=b.x+cosine*p.local.x-sine*p.local.y-c.target.x-0.5,dy=b.y+sine*p.local.x+cosine*p.local.y-c.target.y-0.5;if(dx*dx+dy*dy<=c.radius*c.radius){hit=true;break;}}
            if(hit){b.vx=std::clamp(b.vx+120.0,-512.0,512.0);b.vy=std::clamp(b.vy-100.0,-512.0,512.0);b.angular_velocity=std::clamp(b.angular_velocity+3.0,-20.0,20.0);b.sleeping=false;++changed;}
        }
    }
    // Split after all brush decisions; components retain each surviving pixel exactly once.
    for(std::size_t i=0;i<original_count;++i){
        if(planned[i].pixels.empty())continue;
        auto groups=components(planned[i].pixels);if(groups.size()==1)continue;
        if(planned.size()+groups.size()-1>64 || next>std::numeric_limits<std::uint32_t>::max()-groups.size())return {c.kind,false,0,"Rigid fragments exceed capacity; brush was not committed."};
        const auto parent=planned[i];const auto [old_x,old_y]=centroid(parent.pixels,defs);
        for(std::size_t j=0;j<groups.size();++j){
            auto child=parent;child.pixels=std::move(groups[j]);const auto [x,y]=centroid(child.pixels,defs);
            const auto dx=std::cos(parent.angle)*(x-old_x)-std::sin(parent.angle)*(y-old_y),dy=std::sin(parent.angle)*(x-old_x)+std::cos(parent.angle)*(y-old_y);
            child.vx=std::clamp(parent.vx-parent.angular_velocity*dy,-512.0,512.0);child.vy=std::clamp(parent.vy+parent.angular_velocity*dx,-512.0,512.0);child.sleeping=false;
            if(j){child.id=next++;links.push_back({child.id,parent.id});planned.push_back(std::move(child));}else planned[i]=std::move(child);
        }
    }
    if(changed){
        planned.erase(std::remove_if(planned.begin(),planned.end(),[](const auto& b){return b.pixels.empty();}),planned.end());
        for(const auto& p:removed){--w.counts_[p.material];if(p.state.electrified_ticks)--w.electrified_cells_;if(p.material==1){--w.world_stone_;--w.initial_stone_;}}
        w.bodies_=std::move(planned);w.next_body_id_=next;++w.body_revision_;w.refresh_body_bounds();
        ElectricalSimulation::reconcile(w,links);
    }
    return {c.kind,true,changed,"Rigid brush affected "+std::to_string(changed)+" pixels/bodies; no inventory reward."};
}
bool RigidSimulation::displace_liquids(WorldState& w,const ContentRegistry& content){
    const auto& bounds=w.bounds_;const auto size=static_cast<std::size_t>(bounds.width)*bounds.height;
    auto index=[&](CellPos p){return static_cast<std::size_t>(p.y-bounds.origin.y)*bounds.width+p.x-bounds.origin.x;};
    std::vector<std::uint8_t> occupied(size,0);
    std::vector<CellPos> sources;
    std::vector<const MaterialDefinition*> defs{nullptr};for(const auto& id:w.material_ids_)defs.push_back(content.find_material(id));
    struct Parcel {Material material;CellState state;CellPos origin;};
    std::unordered_map<std::size_t,Parcel> pending;
    auto parcel=[&](CellPos p){const auto it=pending.find(index(p));return it==pending.end()?Parcel{*w.material_at(p),*w.cell_state(p),p}:it->second;};
    auto material=[&](CellPos p){const auto it=pending.find(index(p));return it==pending.end()?*w.material_at(p):it->second.material;};
    auto liquid=[&](CellPos p){const auto m=material(p);if(m==Material::air)return false;
        const auto it=pending.find(index(p));const auto& state=it==pending.end()?*w.cell_state(p):it->second.state;
        return defs[static_cast<std::uint16_t>(m)]->motion(state.mobilized)==Motion::liquid;
    };
    // Raster occupancy uses the same inverse-transform query as transport and
    // display. Multiple rotated pixels sharing a cell displace it only once.
    for(const auto& box:w.body_bounds_)
        for(int y=std::max(bounds.origin.y,static_cast<int>(std::floor(box[1])));y<std::min(bounds.origin.y+bounds.height,static_cast<int>(std::ceil(box[3])));++y)
            for(int x=std::max(bounds.origin.x,static_cast<int>(std::floor(box[0])));x<std::min(bounds.origin.x+bounds.width,static_cast<int>(std::ceil(box[2])));++x){
                const CellPos p{x,y};if(occupied[index(p)] || !w.body_pixel_at(p))continue;
                occupied[index(p)]=1;if(liquid(p))sources.push_back(p);
            }
    if(sources.empty())return true;
    std::vector<std::uint32_t> visited(size,0);std::vector<std::size_t> parent(size,0);std::uint32_t generation=0;
    auto position=[&](std::size_t i){return CellPos{bounds.origin.x+static_cast<int>(i%bounds.width),bounds.origin.y+static_cast<int>(i/bounds.width)};};
    constexpr CellPos directions[]{{0,-1},{-1,0},{1,0},{0,1}};
    for(const auto source:sources){
        ++generation;std::vector<CellPos> queue{source};visited[index(source)]=generation;
        std::optional<CellPos> destination;
        for(std::size_t i=0;i<queue.size() && !destination;++i)for(const auto delta:directions){
            const CellPos next{queue[i].x+delta.x,queue[i].y+delta.y};if(!bounds.contains(next))continue;
            const auto j=index(next);if(visited[j]==generation)continue;visited[j]=generation;parent[j]=index(queue[i]);
            const auto m=material(next);
            if(!occupied[j] && m==Material::air){destination=next;break;}
            // Earlier routes are visible as real virtual parcels, so several
            // displaced cells can share a column without reusing its last air.
            if(liquid(next) || (occupied[j] && m==Material::air))queue.push_back(next);
        }
        if(!destination)return false; // No world writes until every route succeeds.
        std::vector<CellPos> route;
        for(auto at=*destination;at!=source;at=position(parent[index(at)]))
            if(!occupied[index(at)])route.push_back(at);
        route.push_back(source);
        // Shift from the free end back toward the body. Body-interior raster
        // cells transmit displacement but are never filled by another parcel.
        for(std::size_t i=0;i+1<route.size();++i){
            pending[index(route[i])]=parcel(route[i+1]);
            pending[index(route[i+1])]={Material::air,{},route[i+1]};
        }
    }
    std::vector<std::size_t> changed;changed.reserve(pending.size());
    for(const auto& [at,value]:pending){static_cast<void>(value);changed.push_back(at);}
    std::sort(changed.begin(),changed.end());
    std::vector<ElectricRelocation> electrical;
    for(const auto at:changed){const auto& value=pending.at(at);const auto to=position(at);
        if(value.material!=Material::air && value.origin!=to)electrical.push_back({{value.origin,0},{to,0}});
    }
    for(const auto at:changed){auto& value=pending.at(at);if(value.origin!=position(at))value.state.source_attachment=0;w.set_cell(position(at),value.material,value.state);}
    ElectricalSimulation::transfer_batch(w,electrical);
    w.simulation_stats_.fluid_displaced+=static_cast<std::uint32_t>(sources.size());
    return true;
}
void RigidSimulation::advance(WorldState& w,const ContentRegistry& content,RigidSolver& solver,double seconds){
    if(w.bodies_.empty()){solver.reset();return;}
    std::vector<BodyMotion> before;before.reserve(w.bodies_.size());
    for(const auto& b:w.bodies_)before.push_back({b.id,b.x,b.y,b.angle,b.vx,b.vy,b.angular_velocity,b.sleeping});
    const auto motions=solver.step(w,content,seconds);
    if(motions.size()!=w.bodies_.size())throw std::logic_error("Rigid solver omitted a world-owned body.");
    for(const auto& m:motions){
        auto found=std::find_if(w.bodies_.begin(),w.bodies_.end(),[&](const auto& b){return b.id==m.id;});
        if(found==w.bodies_.end())throw std::logic_error("Rigid solver returned an unknown body ID.");
        auto& b=*found;
        if(b.x!=m.x || b.y!=m.y || b.angle!=m.angle || b.vx!=m.vx || b.vy!=m.vy || b.angular_velocity!=m.angular_velocity || b.sleeping!=m.sleeping){
            b.x=m.x;b.y=m.y;b.angle=m.angle;b.vx=m.vx;b.vy=m.vy;b.angular_velocity=m.angular_velocity;b.sleeping=m.sleeping;++w.body_revision_;
        }
    }
    w.refresh_body_bounds();
    if(!displace_liquids(w,content)){
        // Finite incompressible capacity: retain all material and all old poses.
        // The adapter sees the corrected poses/velocities on the next step.
        for(std::size_t i=0;i<before.size();++i){const auto& old=before[i];auto& b=w.bodies_[i];
            b.x=old.x;b.y=old.y;b.angle=old.angle;b.vx=b.vy=b.angular_velocity=0;b.sleeping=false;
        }
        ++w.body_revision_;++w.simulation_stats_.fluid_blocked;w.refresh_body_bounds();
    }
}
}
