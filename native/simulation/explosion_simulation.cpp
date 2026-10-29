#include "simulation/explosion_simulation.hpp"
#include "simulation/material_simulation.hpp"
#include <algorithm>
#include <stdexcept>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include "simulation/rigid_simulation.hpp"
#include "simulation/material_interactions.hpp"
#include "simulation/source_blast.hpp"

namespace pixel {
namespace {
// Slab intersection against a real square/AABB; t is along the finite ray.
std::optional<double> intersect(double x,double y,double dx,double dy,double left,double top,double right,double bottom){
    double enter=0,exit=1;
    auto axis=[&](double origin,double direction,double low,double high){
        if(std::abs(direction)<1e-12)return origin>=low && origin<=high;
        auto a=(low-origin)/direction,b=(high-origin)/direction;if(a>b)std::swap(a,b);
        enter=std::max(enter,a);exit=std::min(exit,b);return enter<=exit+1e-12;
    };
    if(!axis(x,dx,left,right) || !axis(y,dy,top,bottom) || exit<0 || enter>1)return {};
    return std::max(0.0,enter);
}
}
ExplosionResult ExplosionSimulation::apply(WorldState& w,const ContentRegistry& content,CellPos center,const ExplosionDefinition& blast){return commit(w,content,center,blast,{},Material::air);}
ExplosionResult ExplosionSimulation::commit(WorldState& w,const ContentRegistry& content,CellPos center,const ExplosionDefinition& blast,std::span<const CellPos> fuel,Material residue){
    if(!w.bounds_.contains(center) || blast.radius<1 || blast.radius>48 || blast.ray_energy<1)
        throw std::invalid_argument("Explosion requires an in-bounds center, radius 1..48 and positive ray energy.");
    std::vector<const MaterialDefinition*> defs{nullptr};for(const auto& id:w.material_ids_)defs.push_back(content.find_material(id));
    auto definition=[&](Material m){return defs[static_cast<std::uint16_t>(m)];};
    const int shock=blast.radius+std::max(1,blast.radius/4),edge=shock*2+1;
    const auto area=static_cast<std::size_t>(edge)*edge;
    auto index=[&](int x,int y){return static_cast<std::size_t>(y+shock)*edge+static_cast<std::size_t>(x+shock);};
    auto global=[&](CellPos p){return static_cast<std::size_t>(p.y-w.bounds_.origin.y)*w.bounds_.width+static_cast<std::size_t>(p.x-w.bounds_.origin.x);};
    std::unordered_set<std::size_t> consumed;for(const auto p:fuel)consumed.insert(global(p));
    auto initial=[&](Material m){auto s=m==Material::air?CellState{}:MaterialSimulation::initial_state(*definition(m));if(m!=Material::air)s.created_tick=w.tick_;return s;};
    auto grid_material=[&](CellPos p)->std::optional<Material>{if(!w.bounds_.contains(p))return {};return consumed.contains(global(p))?residue:*w.material_at(p);};
    auto residue_state=[&](CellPos p){auto state=initial(residue);if(residue==w.material_at(p)){state.fuel=0;state.burning=false;state.detonation_ticks=0;}return state;};
    auto grid_state=[&](CellPos p){return consumed.contains(global(p))?residue_state(p):*w.cell_state(p);};
    std::vector<std::int64_t> exposure(area,0);std::vector<unsigned> visited(area,0);
    std::vector<std::vector<std::int64_t>> body_power;std::vector<std::size_t> nearby;
    for(std::size_t i=0;i<w.bodies_.size();++i){body_power.emplace_back(w.bodies_[i].pixels.size(),0);const auto& box=w.body_bounds_[i];if(box[2]>=center.x-shock && box[0]<=center.x+shock+1 && box[3]>=center.y-shock && box[1]<=center.y+shock+1)nearby.push_back(i);}
    struct Hit {double time;CellPos grid;bool body=false;std::size_t owner=0,pixel=0;};
    std::vector<Hit> hits;hits.reserve(static_cast<std::size_t>(edge)*3);unsigned ray=0;
    // Collect terrain and oriented body-square intersections before consuming energy.
    // Every ray sees the same pre-blast shape, including fuel-to-residue overrides.
    for(int end_y=-shock;end_y<=shock;++end_y)for(int end_x=-shock;end_x<=shock;++end_x){
        if(end_x*end_x+end_y*end_y>shock*shock)continue;
        const int ax=std::abs(end_x),ay=std::abs(end_y);
        if((ax+1)*(ax+1)+ay*ay<=shock*shock && ax*ax+(ay+1)*(ay+1)<=shock*shock)continue;
        hits.clear();++ray;
        auto add_grid=[&](int x,int y,double t){const auto at=index(x,y);if(visited[at]!=ray){visited[at]=ray;hits.push_back({t,{center.x+x,center.y+y}});}};
        int x=0,y=0;add_grid(0,0,0);
        const int sx=end_x<0?-1:end_x>0?1:0,sy=end_y<0?-1:end_y>0?1:0;
        const double infinity=std::numeric_limits<double>::infinity();
        const double step_x=ax?1.0/ax:infinity,step_y=ay?1.0/ay:infinity;double tx=step_x/2,ty=step_y/2;
        while(x!=end_x || y!=end_y){
            if(std::abs(tx-ty)<1e-12){add_grid(x+sx,y,tx);add_grid(x,y+sy,ty);x+=sx;y+=sy;add_grid(x,y,tx);tx+=step_x;ty+=step_y;}
            else if(tx<ty){x+=sx;add_grid(x,y,tx);tx+=step_x;}else{y+=sy;add_grid(x,y,ty);ty+=step_y;}
        }
        for(const auto i:nearby){
            const auto& body=w.bodies_[i];const auto& box=w.body_bounds_[i];
            if(!intersect(center.x+0.5,center.y+0.5,end_x,end_y,box[0],box[1],box[2],box[3]))continue;
            const auto c=std::cos(body.angle),sine=std::sin(body.angle),dx=center.x+0.5-body.x,dy=center.y+0.5-body.y;
            const auto ox=c*dx+sine*dy,oy=-sine*dx+c*dy,rx=c*end_x+sine*end_y,ry=-sine*end_x+c*end_y;
            for(std::size_t j=0;j<body.pixels.size();++j){const auto p=body.pixels[j].local;if(const auto t=intersect(ox,oy,rx,ry,p.x-0.5,p.y-0.5,p.x+0.5,p.y+0.5))hits.push_back({*t,{},true,i,j});}
        }
        std::sort(hits.begin(),hits.end(),[](const auto& a,const auto& b){if(a.time!=b.time)return a.time<b.time;if(a.body!=b.body)return !a.body;if(a.owner!=b.owner)return a.owner<b.owner;if(a.pixel!=b.pixel)return a.pixel<b.pixel;return a.grid.y!=b.grid.y?a.grid.y<b.grid.y:a.grid.x<b.grid.x;});
        std::int64_t energy=blast.ray_energy;
        for(const auto& hit:hits){
            Material m;double distance;
            if(hit.body){const auto& body=w.bodies_[hit.owner];const auto& pixel=body.pixels[hit.pixel];m=static_cast<Material>(pixel.material);
                const auto c=std::cos(body.angle),sine=std::sin(body.angle);distance=std::max(std::abs(body.x+c*pixel.local.x-sine*pixel.local.y-center.x-0.5),std::abs(body.y+sine*pixel.local.x+c*pixel.local.y-center.y-0.5));
            }else{const auto value=grid_material(hit.grid);if(!value)break;m=*value;distance=std::max(std::abs(hit.grid.x-center.x),std::abs(hit.grid.y-center.y));}
            const auto power=static_cast<std::int64_t>(energy*std::clamp((shock+1-distance)/(shock+1),0.0,1.0));
            auto& output=hit.body?body_power[hit.owner][hit.pixel]:exposure[index(hit.grid.x-center.x,hit.grid.y-center.y)];output=std::max(output,power);
            if(m!=Material::air){const auto* d=definition(m);if(d->durability>blast.max_durability)break;energy-=std::max(1,d->hit_points);}
            if(energy<=0)break;
        }
    }
    std::vector<int> source_damage(area,0);
    CombustionRandom source_random(static_cast<std::uint32_t>(w.seed_^w.tick_^static_cast<unsigned>(center.x*31+center.y)));
    if(blast.source_rules){
        std::array<SourceBlastProbe,360> probes;
        constexpr float two_pi=6.2831854820251465f;
        for(int i=0;i<360;++i){const float angle=static_cast<float>(i)/360*two_pi;
            probes[i]=source_blast_probe(static_cast<float>(center.x),static_cast<float>(center.y),center.x-blast.radius*std::cos(angle),center.y-blast.radius*std::sin(angle),blast.ray_energy,blast.max_durability,source_random,
                [&](CellPos q)->std::optional<SourceBlastObstacle>{const auto m=grid_material(q);if(!m || *m==Material::air)return {};const auto* d=definition(*m);
                    return SourceBlastObstacle{std::max(0,d->hit_points-static_cast<int>(grid_state(q).damage_taken)),d->durability,d->sand || d->cell_type=="solid"};});
        }
        std::fill(exposure.begin(),exposure.end(),0);
        for(int y=-blast.radius;y<=blast.radius;++y)for(int x=-blast.radius;x<=blast.radius;++x){
            const int distance=x*x+y*y;if(distance>=blast.radius*blast.radius)continue;
            // Angle-to-table rasterization is project-owned; original sine edge modulation is not recovered.
            const float angle=std::atan2(static_cast<float>(-y),static_cast<float>(-x));const int i=(static_cast<int>(angle*360/two_pi)+360)%360;
            if(distance<=probes[i].distance_squared){exposure[index(x,y)]=blast.ray_energy;source_damage[index(x,y)]=probes[i].max_spent;}
        }
    }
    struct GridWrite {CellPos p;Material m;CellState s;};std::vector<GridWrite> grid;
    std::unordered_map<std::size_t,std::size_t> locations;
    auto stage=[&](CellPos p,Material m,CellState s){const auto key=global(p);if(const auto found=locations.find(key);found!=locations.end())grid[found->second]={p,m,s};else{locations[key]=grid.size();grid.push_back({p,m,s});}};
    for(const auto p:fuel)stage(p,residue,residue_state(p));
    const auto fire=blast.fire_material=="air"?Material::air:w.material_index(blast.fire_material);
    std::uint64_t affected=0;std::uint32_t destroyed=0,mobilized=0;
    for(int y=-shock;y<=shock;++y)for(int x=-shock;x<=shock;++x){
        const auto power=exposure[index(x,y)];if(!power)continue;const CellPos p{center.x+x,center.y+y};const auto m=grid_material(p);if(!m)continue;
        const int distance=std::max(std::abs(x),std::abs(y));
        if(*m==Material::air){const int inner=std::max(1,blast.radius/3);const auto pattern=static_cast<std::uint64_t>(x*x+y*y)+w.tick_+w.seed_;
            if(!w.body_pixel_at(p) && fire!=Material::air && (blast.source_rules?static_cast<int>(source_random.percent())<blast.create_probability:(x*x+y*y<=inner*inner && ((!x&&!y) || pattern%5==0)))){stage(p,fire,initial(fire));++affected;}continue;}
        const auto* d=definition(*m);auto state=grid_state(p);const auto previous=state;
        if(blast.source_rules){
            if(d->explosion.radius && state.fuel && power>=d->explosion.trigger_energy){if(!state.detonation_ticks)state.detonation_ticks=static_cast<std::uint16_t>(d->explosion.delay_ticks);state.burning=true;}
            else if(blast.hole_enabled && d->durability<=blast.max_durability){
                const int damage=source_damage[index(x,y)];state.damage_taken=std::min(static_cast<std::uint32_t>(d->hit_points),state.damage_taken+static_cast<std::uint32_t>(damage));
                if(state.damage_taken>=static_cast<std::uint32_t>(d->hit_points) && (d->sand || blast.destroy_liquids)){
                    const auto replacement=fire!=Material::air && static_cast<int>(source_random.percent())<blast.create_probability?fire:Material::air;
                    stage(p,replacement,initial(replacement));++destroyed;++affected;continue;
                }
            }
            if(state!=previous){stage(p,*m,state);++affected;}continue;
        }

        const int heat=static_cast<int>(static_cast<std::int64_t>(blast.heat)*power/blast.ray_energy);state.temperature=std::min(10000,std::max(state.temperature,heat));
        if(d->explosion.radius && state.fuel && power>=d->explosion.trigger_energy){if(!state.detonation_ticks)state.detonation_ticks=static_cast<std::uint16_t>(d->explosion.delay_ticks);state.burning=true;}
        else{
            const bool eligible=d->durability<=blast.max_durability,fluid=!d->sand;
            if(eligible && blast.hole_enabled && (!fluid || blast.destroy_liquids) && x*x+y*y<=blast.radius*blast.radius && power>=d->hit_points){stage(p,Material::air,{});++destroyed;++affected;continue;}
            if(eligible && d->can_loosen && power>=std::max(1,d->hit_points/4)){if(!state.mobilized)++mobilized;state.mobilized=true;state.quiet_ticks=0;}
            if(d->motion(state.mobilized)!=Motion::fixed){const int impulse=blast.impulse_q8*(shock+1-distance)/(shock+1),length=std::max(1,distance);
                state.velocity_x=std::clamp(state.velocity_x+x*impulse/length,-8192,8192);state.velocity_y=std::clamp(state.velocity_y+y*impulse/length,-8192,8192);
                if(impulse>d->adhesion_strength_q8){state.adhesion_ticks=0;if(d->adhesion_strength_q8)state.adhesion_cooldown=16;}
            }
        }
        if(state!=previous){stage(p,*m,state);++affected;}
    }
    const auto before=RigidSimulation::shape_centers(w,content);auto bodies=w.bodies_;std::size_t fragments=0,new_ids=0;bool body_changed=false;
    for(std::size_t i=0;i<bodies.size();++i){auto& body=bodies[i];bool shape=false;
        for(std::size_t j=0;j<body.pixels.size();++j){const auto power=body_power[i][j];if(!power)continue;auto& p=body.pixels[j];const auto* d=defs[p.material];const auto old=p.state;
            const auto c=std::cos(body.angle),sine=std::sin(body.angle),dx=body.x+c*p.local.x-sine*p.local.y-center.x-0.5,dy=body.y+sine*p.local.x+c*p.local.y-center.y-0.5;
            // A body pixel is solid even when its material's liquid_sand flag is false.
            if(d->explosion.radius && p.state.fuel && power>=d->explosion.trigger_energy){if(!p.state.detonation_ticks)p.state.detonation_ticks=static_cast<std::uint16_t>(d->explosion.delay_ticks);p.state.burning=true;}
            else if(blast.hole_enabled && d->durability<=blast.max_durability && dx*dx+dy*dy<=blast.radius*blast.radius && power>=d->hit_points){p.material=0;p.state={};shape=true;body_changed=true;++destroyed;++affected;continue;}
            p.state.temperature=std::min(10000,std::max(p.state.temperature,static_cast<int>(static_cast<std::int64_t>(blast.heat)*power/blast.ray_energy)));
            if(p.state!=old || blast.impulse_q8){body_changed=true;++affected;}
        }
        if(shape)++body.shape_revision;const auto count=RigidSimulation::connected_parts(body.pixels).size();fragments+=count;if(count>1)new_ids+=count-1;
    }
    if(fragments>64 || new_ids>std::numeric_limits<std::uint32_t>::max()-w.next_body_id_){++w.simulation_stats_.deferred_reactions;return {false,0,"Rigid fragments exceed capacity; explosion and fuel were not committed.",{}};}
    // Commit only after geometry, state and capacity plans are complete.
    for(const auto& v:grid){const auto old=*w.material_at(v.p);if(old==Material::stone && v.m!=old)--w.initial_stone_;if(v.m==Material::stone && v.m!=old)++w.initial_stone_;w.set_cell(v.p,v.m,v.s);}
    for(std::size_t i=0;i<bodies.size();++i)for(std::size_t j=0;j<bodies[i].pixels.size();++j)if(!bodies[i].pixels[j].material){const auto& old=w.bodies_[i].pixels[j];const auto m=old.material;--w.counts_[m];if(old.state.electrified_ticks)--w.electrified_cells_;if(m==1){--w.world_stone_;--w.initial_stone_;}}
    std::unordered_map<std::uint32_t,std::size_t> parents;for(std::size_t i=0;i<bodies.size();++i)parents[bodies[i].id]=i;
    std::vector<std::array<int,65*65>> lookup(bodies.size());
    for(std::size_t i=0;i<bodies.size();++i){lookup[i].fill(-1);for(std::size_t j=0;j<bodies[i].pixels.size();++j){const auto p=bodies[i].pixels[j].local;lookup[i][(p.y+32)*65+p.x+32]=static_cast<int>(j);}}
    if(body_changed){w.bodies_=std::move(bodies);++w.body_revision_;}
    auto links=RigidSimulation::finish_material_changes(w,content,before);
    // Apply pressure to each surviving fragment independently, at actual exposed pixels.
    // Momentum per unit area is a project parameter; density and inertia resist it.
    if(blast.impulse_q8)for(auto& body:w.bodies_){
        const auto link=std::find_if(links.begin(),links.end(),[&](const auto& v){return v.child==body.id;});const auto parent=link==links.end()?body.id:link->parent;const auto owner=parents.at(parent);
        double mass=0,cx=0,cy=0;for(const auto& p:body.pixels){const auto density=defs[p.material]->density;mass+=density;cx+=p.local.x*density;cy+=p.local.y*density;}cx/=mass;cy/=mass;
        double jx=0,jy=0,torque=0,inertia=0;const auto c=std::cos(body.angle),sine=std::sin(body.angle);
        for(const auto& p:body.pixels){const auto density=defs[p.material]->density;const auto rx=c*(p.local.x-cx)-sine*(p.local.y-cy),ry=sine*(p.local.x-cx)+c*(p.local.y-cy);inertia+=density*(rx*rx+ry*ry+1.0/6);
            const auto at=lookup[owner][(p.local.y+32)*65+p.local.x+32];const auto power=body_power[owner][static_cast<std::size_t>(at)];if(!power)continue;
            const auto dx=body.x+c*p.local.x-sine*p.local.y-center.x-0.5,dy=body.y+sine*p.local.x+c*p.local.y-center.y-0.5,length=std::max(1.0,std::hypot(dx,dy));
            const auto pressure=static_cast<double>(blast.impulse_q8)*60.0/256*power/blast.ray_energy,px=dx*pressure/length,py=dy*pressure/length;jx+=px;jy+=py;torque+=rx*py-ry*px;
        }
        if(jx || jy || torque){body.vx=std::clamp(body.vx+jx/mass,-512.0,512.0);body.vy=std::clamp(body.vy+jy/mass,-512.0,512.0);body.angular_velocity=std::clamp(body.angular_velocity+torque/inertia,-20.0,20.0);body.sleeping=false;++w.body_revision_;}
    }
    w.simulation_stats_.blast_destroyed+=destroyed;w.simulation_stats_.blast_mobilized+=mobilized;++w.simulation_stats_.explosions;
    return {true,affected,{},std::move(links)};
}

void ExplosionSimulation::advance(WorldState& w,const ContentRegistry& content){
    bool present=false;std::vector<const MaterialDefinition*> defs{nullptr};
    for(const auto& id:w.material_ids_){const auto* d=content.find_material(id);defs.push_back(d);if(d->explosion.radius && w.material_count(w.material_index(id)))present=true;}
    if(!present)return;
    struct ReadyBody {std::uint32_t root;CellPos local;};std::vector<ReadyBody> ready_bodies;
    std::unordered_map<std::uint32_t,std::uint32_t> roots;
    for(auto& body:w.bodies_){roots[body.id]=body.id;for(auto& pixel:body.pixels){
        if(!pixel.state.detonation_ticks)continue;
        if(pixel.state.detonation_ticks==1)ready_bodies.push_back({body.id,pixel.local});else{--pixel.state.detonation_ticks;++w.body_revision_;}
        w.wake({static_cast<int>(std::floor(body.x+std::cos(body.angle)*pixel.local.x-std::sin(body.angle)*pixel.local.y)),static_cast<int>(std::floor(body.y+std::sin(body.angle)*pixel.local.x+std::cos(body.angle)*pixel.local.y))});
    }}
    std::vector<CellPos> ready;
    for(auto& chunk:w.chunks_){
        if(!chunk.awake)continue;
        for(int y=0;y<chunk_size;++y)for(int x=0;x<chunk_size;++x){
            const CellPos p{chunk.position.x*chunk_size+x,chunk.position.y*chunk_size+y};if(!w.bounds_.contains(p))continue;
            const auto at=static_cast<std::size_t>(y)*chunk_size+x;auto state=chunk.states[at];
            if(!state.detonation_ticks)continue;
            if(state.detonation_ticks==1)ready.push_back(p);else{--state.detonation_ticks;w.set_cell(p,chunk.cells[at],state);}w.wake(p);
        }
    }
    auto aggregate=[](ExplosionDefinition blast,std::size_t size){int growth=0;while(growth<12 && static_cast<std::size_t>((growth+1)*(growth+1))<=size)++growth;
        blast.radius=std::min(48,blast.radius+std::max(0,growth-1));blast.ray_energy=static_cast<int>(std::min<std::int64_t>(1000000000,static_cast<std::int64_t>(blast.ray_energy)*std::min<std::size_t>(16,size)));return blast;};
    auto remember=[&](const auto& links){for(const auto& link:links)roots[link.child]=roots.at(link.parent);};
    const auto& bounds=w.bounds_;std::vector<unsigned char> collected(static_cast<std::size_t>(bounds.width)*bounds.height,0);
    auto index=[&](CellPos p){return static_cast<std::size_t>(p.y-bounds.origin.y)*bounds.width+static_cast<std::size_t>(p.x-bounds.origin.x);};
    int attempts=0;
    for(const auto origin:ready){
        if(attempts==64)break;if(collected[index(origin)])continue;
        const auto material=*w.material_at(origin);if(material==Material::air)continue;const auto* d=defs[static_cast<std::uint16_t>(material)];
        if(!d->explosion.radius || !w.cell_state(origin)->detonation_ticks)continue;
        std::vector<CellPos> cluster{origin};collected[index(origin)]=1;
        for(std::size_t i=0;i<cluster.size();++i)for(const auto delta:{CellPos{0,-1},CellPos{-1,0},CellPos{1,0},CellPos{0,1}}){
            const CellPos p{cluster[i].x+delta.x,cluster[i].y+delta.y};if(!bounds.contains(p) || collected[index(p)] || w.material_at(p)!=material)continue;collected[index(p)]=1;cluster.push_back(p);
        }
        const auto residue=d->explosion.output=="air"?Material::air:w.material_index(d->explosion.output);
        const auto result=commit(w,content,origin,aggregate(d->explosion,cluster.size()),cluster,residue);++attempts;
        if(result.ok)remember(result.fragments);
    }
    // Eligibility is captured before any explosions. Parent/local identities survive
    // fracture, so newly primed pixels cannot recursively detonate in this phase.
    std::unordered_map<std::uint32_t,std::vector<CellPos>> processed;
    for(const auto entry:ready_bodies){
        if(attempts==64)break;auto& handled=processed[entry.root];if(std::find(handled.begin(),handled.end(),entry.local)!=handled.end())continue;
        auto body=w.bodies_.end();std::size_t pixel=0;
        for(auto it=w.bodies_.begin();it!=w.bodies_.end();++it){if(roots.at(it->id)!=entry.root)continue;const auto found=std::find_if(it->pixels.begin(),it->pixels.end(),[&](const auto& p){return p.local==entry.local;});if(found!=it->pixels.end()){body=it;pixel=static_cast<std::size_t>(found-it->pixels.begin());break;}}
        if(body==w.bodies_.end())continue;const auto& origin=body->pixels[pixel];const auto* d=defs[origin.material];
        if(!origin.state.detonation_ticks || !origin.state.fuel || !d->explosion.radius)continue;
        auto selected=body->pixels;for(auto& p:selected)if(p.material!=origin.material)p.material=0;
        auto parts=RigidSimulation::connected_parts(selected);const auto part=std::find_if(parts.begin(),parts.end(),[&](const auto& group){return std::any_of(group.begin(),group.end(),[&](const auto& p){return p.local==entry.local;});});
        std::vector<CellPos> cluster;for(const auto& p:*part){cluster.push_back(p.local);handled.push_back(p.local);}
        CellPos center{static_cast<int>(std::floor(body->x+std::cos(body->angle)*origin.local.x-std::sin(body->angle)*origin.local.y)),static_cast<int>(std::floor(body->y+std::sin(body->angle)*origin.local.x+std::cos(body->angle)*origin.local.y))};
        center.x=std::clamp(center.x,bounds.origin.x,bounds.origin.x+bounds.width-1);center.y=std::clamp(center.y,bounds.origin.y,bounds.origin.y+bounds.height-1);
        // Rare body detonations use an owned candidate, preserving both residue
        // placement and the whole blast if either preflight fails. No per-frame copy.
        WorldState candidate=w;const auto conversion=MaterialInteractions::consume_body_fuel(candidate,content,body->id,cluster,d->explosion.output);++attempts;
        if(!conversion.ok){++w.simulation_stats_.deferred_reactions;continue;}
        const auto result=apply(candidate,content,center,aggregate(d->explosion,cluster.size()));
        if(!result.ok){++w.simulation_stats_.deferred_reactions;continue;}
        w=std::move(candidate);remember(conversion.fragments);remember(result.fragments);
    }
}
}
