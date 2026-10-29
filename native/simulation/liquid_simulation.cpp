#include "simulation/liquid_simulation.hpp"
#include "simulation/electrical_simulation.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <limits>
#include <stdexcept>
#include <chrono>

namespace pixel {
namespace {
constexpr std::array<CellPos,4> directions{{{1,0},{0,1},{-1,0},{0,-1}}};
constexpr std::uint32_t volume_unit=1u<<24;
double vx(const CellState& state){return (state.velocity_x+state.momentum_remainder_x/256.0)/256;}
double vy(const CellState& state){return (state.velocity_y+state.momentum_remainder_y/256.0)/256;}
double kinetic(const CellState& state){return 0.5*(vx(state)*vx(state)+vy(state)*vy(state));}
void velocity_q16(double velocity,std::int32_t& coarse,std::int16_t& remainder){
    const double scaled=velocity*65536,nearest=std::round(scaled);
    // Keep exactly representable uniform translation; dissipate fractional
    // remnants toward zero so interpolation cannot round a tiny vortex back
    // to the same nonzero velocity forever.
    const auto fixed=static_cast<std::int32_t>(std::abs(scaled-nearest)<1e-8?nearest:scaled);
    coarse=fixed/256;remainder=static_cast<std::int16_t>(fixed%256);
}
struct Node {
    CellPos p;
    Material material;
    CellState state;
    const MaterialDefinition* definition;
    std::array<int,4> faces{{-1,-1,-1,-1}};
    double x=0,y=0;
    bool hydrostatic=false;
    bool ballistic=false;
    int component=0;
};
struct Face {int a,b,direction;double inverse_mass,velocity;CellPos outside;double predicted=0;int body=-1;double torque=0,rx=0,ry=0;};
struct Body {
    double mass=0,inertia=0,x=0,y=0,vx=0,vy=0,spin=0;
};
struct DragMatrix {
    std::array<std::array<double,3>,3> matrix{};std::array<double,3> rhs{};
    std::array<double,3> solve()const{
        std::array<std::array<double,3>,3> lower{};
        for(int i=0;i<3;++i)for(int j=0;j<=i;++j){double value=matrix[i][j];for(int k=0;k<j;++k)value-=lower[i][k]*lower[j][k];if(i==j && !(value>0))throw std::runtime_error("Liquid/rigid drag matrix lost positive definiteness.");lower[i][j]=i==j?std::sqrt(value):value/lower[j][j];}
        auto result=rhs;for(int i=0;i<3;++i){for(int j=0;j<i;++j)result[i]-=lower[i][j]*result[j];result[i]/=lower[i][i];}
        for(int i=2;i>=0;--i){for(int j=i+1;j<3;++j)result[i]-=lower[j][i]*result[j];result[i]/=lower[i][i];}return result;
    }
};
struct PressureRow {std::array<int,4> indices{};std::array<double,4> weights{};int count=0;};
template<class Matrix>
std::uint32_t solve_pressure(const std::vector<double>& rhs,std::vector<double>& pressure,const std::vector<double>& diagonal,const std::vector<Face>& faces,Matrix&& matrix){
    const auto n=rhs.size();std::vector<double> r(n),z(n),search(n),product(n);matrix(pressure,product);
    auto dot=[](const auto& a,const auto& b){return std::inner_product(a.begin(),a.end(),b.begin(),0.0);};
    double maximum=0,residual=0;for(std::size_t i=0;i<n;++i){maximum=std::max(maximum,std::abs(rhs[i]));r[i]=rhs[i]-product[i];residual=std::max(residual,std::abs(r[i]));}
    const double tolerance=1e-7*std::max(1.0,maximum);if(residual<=tolerance)return 0;
    // MIC(0) of the grid stencil. Body coupling remains in the exact matrix;
    // this SPD approximation only accelerates PCG, including sealed domains.
    struct Link {int index=-1;double value=0;};
    std::vector<std::array<Link,2>> lower(n),upper(n);std::vector<double> root(n),upper_sum(n,0);
    for(const auto& f:faces)if(f.b>=0){const int a=std::min(f.a,f.b),b=std::max(f.a,f.b);auto& entry=lower[b];entry[entry[0].index<0?0:1]={a,-f.inverse_mass};upper_sum[a]-=f.inverse_mass;}
    for(std::size_t i=0;i<n;++i){double sum=0;
        for(auto& link:lower[i])if(link.index>=0){const double raw=link.value,reciprocal=1/root[link.index];link.value*=reciprocal;
            sum+=link.value*link.value+0.97*raw*(upper_sum[link.index]-raw)*reciprocal*reciprocal;
            auto& entry=upper[link.index];entry[entry[0].index<0?0:1]={static_cast<int>(i),link.value};}
        const double value=diagonal[i]-sum;root[i]=std::sqrt(std::max(1e-12,value<diagonal[i]*0.25?diagonal[i]:value));
    }
    auto precondition=[&](){
        for(std::size_t i=0;i<n;++i){double value=r[i];for(const auto& link:lower[i])if(link.index>=0)value-=link.value*z[link.index];z[i]=value/root[i];}
        for(std::size_t i=n;i-->0;){double value=z[i];for(const auto& link:upper[i])if(link.index>=0)value-=link.value*z[link.index];z[i]=value/root[i];}
    };
    precondition();search=z;
    double rz=dot(r,z);std::uint32_t iterations=0;
    const auto limit=std::min<std::size_t>(n+1,2048);
    for(std::size_t iteration=0;iteration<limit && residual>tolerance;++iteration){
        matrix(search,product);const double denominator=dot(search,product);
        if(!(denominator>0))throw std::runtime_error("Liquid pressure matrix is not positive on the active residual.");
        const double alpha=rz/denominator;residual=0;
        for(std::size_t i=0;i<n;++i){pressure[i]+=alpha*search[i];r[i]-=alpha*product[i];residual=std::max(residual,std::abs(r[i]));}
        precondition();
        const double next=dot(r,z),beta=rz?next/rz:0;rz=next;for(std::size_t i=0;i<n;++i)search[i]=z[i]+beta*search[i];++iterations;
    }
    if(residual>tolerance)throw std::runtime_error("Liquid pressure failed to converge; residual="+std::to_string(residual));
    return iterations;
}
}
void LiquidSimulation::step(WorldState& w,const ContentRegistry& content,std::span<const CellPos> active,
    std::span<const std::uint8_t> attached,std::span<std::uint8_t> updated,std::span<std::uint8_t> transported){
    if(active.empty())return;
    // Supported legacy snapshots may contain liquid inside a body. With no
    // grid vacancy at all, the existing atomic displacement stage must reject
    // that overlap before any pressure impulses are committed.
    if(!w.bodies_.empty() && !w.counts_[0])return;
    std::vector<const MaterialDefinition*> definitions{nullptr};std::vector<Motion> motions{Motion::fixed};std::uint64_t liquid_count=0;
    for(std::size_t i=0;i<w.material_ids_.size();++i){const auto* d=content.find_material(w.material_ids_[i]);definitions.push_back(d);motions.push_back(d->source_liquid?Motion::fixed:d->motion());if(motions.back()==Motion::liquid)liquid_count+=w.counts_[i+1];}
    if(liquid_count<2)return;
    double speed=0;
    for(const auto p:active){const auto m=*w.material_at(p);if(m==Material::air)continue;
        const auto* d=definitions[static_cast<std::uint16_t>(m)];if(motions[static_cast<std::uint16_t>(m)]!=Motion::liquid)continue;
        const auto& s=*w.cell_state(p);speed=std::max(speed,std::abs(vx(s))+std::abs(vy(s))+liquid_gravity(*d));
    }
    for(const auto& body:w.bodies_){bool wet=false;double point_speed=0;const double cosine=std::cos(body.angle),sine=std::sin(body.angle);
        for(const auto& pixel:body.pixels){const double rx=cosine*pixel.local.x-sine*pixel.local.y,ry=sine*pixel.local.x+cosine*pixel.local.y;
            point_speed=std::max(point_speed,(std::abs(body.vx-body.angular_velocity*ry)+std::abs(body.vy+body.angular_velocity*rx))/60);
            if(wet)continue;const CellPos p{static_cast<int>(std::floor(body.x+rx)),static_cast<int>(std::floor(body.y+ry))};
            for(const auto d:directions){const auto m=w.material_at({p.x+d.x,p.y+d.y});if(m && motions[static_cast<std::uint16_t>(*m)]==Motion::liquid)wet=true;}}
        if(wet)speed=std::max(speed,point_speed);
    }
    const int count=std::max(1,static_cast<int>(std::ceil(speed)));const double delta=1.0/count;
    for(int part=0;part<count;++part){++w.simulation_stats_.liquid_substeps;substep(w,definitions,motions,active,attached,updated,transported,delta);}
}
void LiquidSimulation::substep(WorldState& w,std::span<const MaterialDefinition* const> defs,std::span<const Motion> motions,std::span<const CellPos> active,
    std::span<const std::uint8_t> attached,std::span<std::uint8_t> updated,std::span<std::uint8_t> transported,double delta){
    const auto& bounds=w.bounds_;const auto size=static_cast<std::size_t>(bounds.width)*bounds.height;
    auto started=std::chrono::steady_clock::now();
    auto elapsed=[&](){const auto now=std::chrono::steady_clock::now();const double value=std::chrono::duration<double,std::milli>(now-started).count();started=now;return value;};
    auto index=[&](CellPos p){return static_cast<std::size_t>(p.y-bounds.origin.y)*bounds.width+p.x-bounds.origin.x;};
    std::vector<int> lookup(size,-1);std::vector<Node> nodes;std::vector<std::uint8_t> open(size,0);
    std::size_t expected=0;for(std::size_t i=1;i<defs.size();++i)if(motions[i]==Motion::liquid)expected+=static_cast<std::size_t>(w.counts_[i]);nodes.reserve(std::min(expected,size));
    const bool has_bodies=!w.bodies_.empty();std::vector<int> owners(has_bodies?size:0,-1);
    if(has_bodies)for(const auto& box:w.body_bounds_)
        for(int y=std::max(bounds.origin.y,static_cast<int>(std::floor(box[1])));y<std::min(bounds.origin.y+bounds.height,static_cast<int>(std::ceil(box[3])));++y)
            for(int x=std::max(bounds.origin.x,static_cast<int>(std::floor(box[0])));x<std::min(bounds.origin.x+bounds.width,static_cast<int>(std::ceil(box[2])));++x){
                const CellPos p{x,y};if(owners[index(p)]>=0)continue;if(const auto owner=w.body_pixel_at(p))owners[index(p)]=static_cast<int>(owner->body);}
    auto owner_at=[&](CellPos p){return has_bodies?owners[index(p)]:-1;};
    std::vector<std::uint8_t> awake(size,0);for(const auto p:active)awake[index(p)]=1;
    // Resolve an unstable density interface before the pressure solve. Perfectly
    // flat, inverted layers otherwise form an exact but unstable PDE solution.
    std::vector<std::uint8_t> exchanged(size,0);
    for(const auto p:active){const CellPos q{p.x,p.y+1};if(!bounds.contains(q) || exchanged[index(p)] || exchanged[index(q)] || attached[index(p)] || attached[index(q)] || owner_at(p)>=0 || owner_at(q)>=0)continue;
        const auto a=*w.material_at(p),b=*w.material_at(q);if(a==Material::air || b==Material::air || a==b)continue;
        const auto* da=defs[static_cast<std::uint16_t>(a)];const auto* db=defs[static_cast<std::uint16_t>(b)];
        if(motions[static_cast<std::uint16_t>(a)]!=Motion::liquid || motions[static_cast<std::uint16_t>(b)]!=Motion::liquid || !da->gravity_q8 || da->density<=db->density || w.tick_%da->flow_interval || w.tick_%db->flow_interval)continue;
        auto sa=*w.cell_state(p),sb=*w.cell_state(q);sa.source_attachment=sb.source_attachment=0;w.set_cell(p,b,sb);w.set_cell(q,a,sa);
        const std::array<ElectricRelocation,2> changes{{{{p,0},{q,0}},{{q,0},{p,0}}}};ElectricalSimulation::transfer_batch(w,changes);
        exchanged[index(p)]=exchanged[index(q)]=transported[index(p)]=transported[index(q)]=1;w.simulation_stats_.moved_cells+=2;
    }
    for(int y=bounds.origin.y;y<bounds.origin.y+bounds.height;++y)for(int x=bounds.origin.x;x<bounds.origin.x+bounds.width;++x){
        const CellPos p{x,y};const auto m=*w.material_at(p);
        if(attached[index(p)] || owner_at(p)>=0)continue;
        if(m==Material::air){open[index(p)]=1;continue;}
        const auto* d=defs[static_cast<std::uint16_t>(m)];
        const auto motion=motions[static_cast<std::uint16_t>(m)];if(motion==Motion::gas || motion==Motion::fire)open[index(p)]=1;
        if(motion!=Motion::liquid || w.tick_%static_cast<std::uint64_t>(d->flow_interval))continue;
        lookup[index(p)]=static_cast<int>(nodes.size());nodes.push_back({p,m,*w.cell_state(p),d});
    }
    if(nodes.empty()){w.simulation_stats_.liquid_setup_ms+=elapsed();return;}
    // Connected pressure dependencies cross chunk rows as well as columns.
    // Isolated parcels retain the existing swept ballistic/row-outlet rules.
    std::vector<std::uint8_t> seen(nodes.size(),0),enabled(nodes.size(),0);
    std::vector<int> component_queue;component_queue.reserve(nodes.size());
    int components=0;
    for(std::size_t start=0;start<nodes.size();++start){
        if(seen[start])continue;
        auto& queue=component_queue;queue.clear();queue.push_back(static_cast<int>(start));seen[start]=1;bool moving=false;
        for(std::size_t k=0;k<queue.size();++k){const auto i=queue[k];moving=moving || awake[index(nodes[i].p)];
            for(const auto d:directions){const CellPos q{nodes[i].p.x+d.x,nodes[i].p.y+d.y};if(!bounds.contains(q))continue;
                const int j=lookup[index(q)];if(j>=0 && !seen[j]){seen[j]=1;queue.push_back(j);}}
        }
        int highest=std::numeric_limits<int>::max(),lowest_open=std::numeric_limits<int>::min();bool supported=false,spill=false;
        for(const int i:queue)for(const auto d:directions){const CellPos q{nodes[i].p.x+d.x,nodes[i].p.y+d.y};if(!bounds.contains(q)){if(d.y==1)supported=true;continue;}
            if(open[index(q)]){highest=std::min(highest,nodes[i].p.y);lowest_open=std::max(lowest_open,q.y);
                const CellPos below{q.x,q.y+1};spill=spill || (bounds.contains(below) && open[index(below)]);}
            else if(d.y==1 && lookup[index(q)]<0)supported=true;}
        // A falling component touching a ceiling needs a separating boundary,
        // rather than tensile pressure holding the old full raster in place.
        if(moving && queue.size()>1){for(const int i:queue){enabled[i]=1;nodes[i].hydrostatic=lowest_open<=highest && !spill;nodes[i].ballistic=!supported;nodes[i].component=components;}++components;}
    }
    std::fill(lookup.begin(),lookup.end(),-1);std::size_t kept=0;
    for(std::size_t i=0;i<nodes.size();++i)if(enabled[i]){if(i!=kept)nodes[kept]=std::move(nodes[i]);lookup[index(nodes[kept].p)]=static_cast<int>(kept);++kept;}
    nodes.resize(kept);if(nodes.empty()){w.simulation_stats_.liquid_setup_ms+=elapsed();return;}
    const auto n=nodes.size();std::vector<Face> faces;faces.reserve(n*3);
    std::vector<Body> bodies; bodies.reserve(w.bodies_.size());
    for(const auto& actual:w.bodies_){Body b;double local_x=0,local_y=0;
        for(const auto& pixel:actual.pixels){const double mass=defs[pixel.material]->density;b.mass+=mass;local_x+=mass*pixel.local.x;local_y+=mass*pixel.local.y;}
        local_x/=b.mass;local_y/=b.mass;
        for(const auto& pixel:actual.pixels){const double x=pixel.local.x-local_x,y=pixel.local.y-local_y;b.inertia+=defs[pixel.material]->density*(x*x+y*y+1.0/6);}
        const double cosine=std::cos(actual.angle),sine=std::sin(actual.angle);
        b.x=actual.x+cosine*local_x-sine*local_y;b.y=actual.y+sine*local_x+cosine*local_y;
        b.vx=actual.vx/60;b.vy=actual.vy/60;b.spin=actual.angular_velocity/60;bodies.push_back(b);
    }
    std::vector<double> energy(components,0);
    std::vector<CellPos> final_positions;final_positions.reserve(n);
    for(auto& node:nodes){
        const auto* d=node.definition;
        energy[node.component]+=d->density*kinetic(node.state);
        final_positions.push_back(node.p);
        node.x=vx(node.state);
        // Well-balanced discrete rest: when no atmospheric vacancy can accept
        // a lower parcel, reference hydrostatic pressure cancels gravity exactly.
        // The remaining solve still projects all existing dynamic momentum.
        node.y=std::clamp(vy(node.state)+(node.hydrostatic?0:liquid_gravity(*d)*delta),-d->terminal_speed_q8/256.0,d->terminal_speed_q8/256.0);
    }
    for(std::size_t i=0;i<n;++i)for(int d=0;d<4;++d){
        auto& node=nodes[i];const CellPos q{node.p.x+directions[d].x,node.p.y+directions[d].y};
        if(!bounds.contains(q))continue;
        const int owner=owner_at(q);if(owner>=0){
            const auto& body=bodies[owner];const auto direction=directions[d];
            const double rx=node.p.x+0.5+direction.x*0.5-body.x,ry=node.p.y+0.5+direction.y*0.5-body.y;
            const double torque=rx*direction.y-ry*direction.x,velocity=body.vx*direction.x+body.vy*direction.y+body.spin*torque;
            if(d==3 && node.ballistic && -node.y<velocity){node.faces[d]=static_cast<int>(faces.size());faces.push_back({static_cast<int>(i),-3,d,1/node.definition->density,-node.y,q});continue;}
            node.faces[d]=static_cast<int>(faces.size());faces.push_back({static_cast<int>(i),-2,d,0,velocity,q,0,owner,torque,rx,ry});continue;
        }
        const int j=lookup[index(q)];if(j>=0 && j<static_cast<int>(i))continue;
        if(j<0){if(!open[index(q)]){
                    if(d==3 && node.ballistic && node.y>0){node.faces[d]=static_cast<int>(faces.size());faces.push_back({static_cast<int>(i),-3,d,1/node.definition->density,-node.y,q});}
                    continue;}
        }
        const double a=node.definition->density,b=j>=0?nodes[j].definition->density:a;
        const double va=d%2?node.y*directions[d].y:node.x*directions[d].x;
        const double vb=j>=0?(d%2?nodes[j].y*directions[d].y:nodes[j].x*directions[d].x):va;
        const int face=static_cast<int>(faces.size());node.faces[d]=face;
        if(j>=0)nodes[j].faces[(d+2)%4]=face;
        faces.push_back({static_cast<int>(i),j,d,2/(a+b),(a*va+b*vb)/(a+b),q});
    }
    // Viscosity exchanges equal/opposite impulses between actual neighbors.
    // Uniform bulk momentum is not erased by a per-particle damping factor.
    std::vector<std::array<double,2>> viscous(n);
    for(const auto& face:faces)if(face.b>=0){const auto& a=nodes[face.a];const auto& b=nodes[face.b];
        const double ma=a.definition->density,mb=b.definition->density;
        const double coefficient=(1-(a.definition->damping_milli+b.definition->damping_milli)/2000.0)*0.25*delta/(1/ma+1/mb);
        const double x=coefficient*(b.x-a.x),y=coefficient*(b.y-a.y);
        viscous[face.a][0]+=x/ma;viscous[face.b][0]-=x/mb;viscous[face.a][1]+=y/ma;viscous[face.b][1]-=y/mb;
    }
    for(std::size_t i=0;i<n;++i){auto& node=nodes[i];node.x+=viscous[i][0];node.y+=viscous[i][1];
        const double friction=std::pow(node.definition->damping_milli/1000.0,delta);
        for(int d=0;d<4;++d)if(node.faces[d]<0){if(d%2)node.x*=friction;else node.y*=friction;}}
    if(!bodies.empty()){
        struct Drag {int face;double coefficient,x,y;};std::vector<Drag> drag;
        std::vector<int> node_contacts(n,0);std::vector<DragMatrix> matrices(bodies.size());
        for(std::size_t i=0;i<bodies.size();++i){const auto& b=bodies[i];auto& m=matrices[i];m.matrix[0][0]=m.matrix[1][1]=b.mass;m.matrix[2][2]=b.inertia;m.rhs={b.mass*b.vx,b.mass*b.vy,b.inertia*b.spin};}
        for(const auto& f:faces)if(f.body>=0)++node_contacts[f.a];
        for(std::size_t i=0;i<faces.size();++i){const auto& f=faces[i];if(f.body<0)continue;const auto& node=nodes[f.a];
            const double x=node.x,y=node.y-(node.hydrostatic?0:liquid_gravity(*node.definition)*delta),rate=8*delta/60;
            const double coefficient=node.definition->density/node_contacts[f.a]*rate/(1+rate);drag.push_back({static_cast<int>(i),coefficient,x,y});
            auto& m=matrices[f.body];m.matrix[0][0]+=coefficient;m.matrix[1][1]+=coefficient;m.matrix[0][2]-=coefficient*f.ry;m.matrix[2][0]-=coefficient*f.ry;
            m.matrix[1][2]+=coefficient*f.rx;m.matrix[2][1]+=coefficient*f.rx;m.matrix[2][2]+=coefficient*(f.rx*f.rx+f.ry*f.ry);
            m.rhs[0]+=coefficient*x;m.rhs[1]+=coefficient*y;m.rhs[2]+=coefficient*(f.rx*y-f.ry*x);
        }
        std::vector<std::array<double,3>> body_motions;body_motions.reserve(bodies.size());for(const auto& matrix:matrices)body_motions.push_back(matrix.solve());
        for(const auto& pair:drag){const auto& f=faces[pair.face];auto& node=nodes[f.a];const auto& b=bodies[f.body];
            const auto& next=body_motions[f.body];const double x=pair.coefficient*(pair.x-(next[0]-next[2]*f.ry)),y=pair.coefficient*(pair.y-(next[1]+next[2]*f.rx));
            const double middle_x=0.5*(b.vx+next[0])-(0.5*(b.spin+next[2]))*f.ry,middle_y=0.5*(b.vy+next[1])+(0.5*(b.spin+next[2]))*f.rx;
            energy[node.component]-=x*middle_x+y*middle_y;node.x-=x/node.definition->density;node.y-=y/node.definition->density;
        }
        for(std::size_t i=0;i<bodies.size();++i){auto& b=bodies[i];const auto& next=body_motions[i];const double difference=std::abs(next[0]-b.vx)+std::abs(next[1]-b.vy)+std::abs(next[2]-b.spin);b.vx=next[0];b.vy=next[1];b.spin=next[2];
            if(difference<1e-10)continue;
            auto& actual=w.bodies_[i];actual.vx=std::clamp(b.vx*60,-512.0,512.0);actual.vy=std::clamp(b.vy*60,-512.0,512.0);actual.angular_velocity=std::clamp(b.spin*60,-20.0,20.0);actual.sleeping=false;++w.body_revision_;
        }
    }
    for(auto& face:faces){const auto& a=nodes[face.a];const auto& b=nodes[face.b>=0?face.b:face.a];const auto d=directions[face.direction];
        face.predicted=(a.definition->density*(a.x*d.x+a.y*d.y)+b.definition->density*(b.x*d.x+b.y*d.y))/(a.definition->density+b.definition->density);
        if(face.body<0)face.velocity=face.predicted;
        else{const auto& wall=bodies[face.body];face.velocity=wall.vx*d.x+wall.vy*d.y+wall.spin*face.torque;}}
    std::vector<double> mass(components,0),momentum_x(components,0),momentum_y(components,0);
    std::vector<std::uint8_t> free_component(components,1),zero_gravity(components,1);
    for(const auto& node:nodes){const int c=node.component;mass[c]+=node.definition->density;momentum_x[c]+=node.definition->density*node.x;momentum_y[c]+=node.definition->density*node.y;
        if(node.definition->gravity_q8)zero_gravity[c]=0;
        for(const int edge:node.faces)if(edge<0)free_component[c]=0;
    }
    w.simulation_stats_.liquid_setup_ms+=elapsed();
    // MAC pressure: A = -div(1/rho grad), atmospheric p=0 on free
    // surfaces and zero normal flux at terrain/body/finite-world walls.
    // Sealed components have a constant-pressure nullspace; their RHS sums to
    // zero, so PCG solves in that compatible subspace without a fake outlet.
    std::vector<double> diagonal(n,0),fluid_diagonal(n,0),rhs(n,0),pressure(n,0),reference(n,0),r(n);
    std::vector<PressureRow> stencil(n);std::vector<int> wet_faces;
    for(std::size_t i=0;i<faces.size();++i){const auto& f=faces[i];if(f.body>=0)wet_faces.push_back(static_cast<int>(i));
        if(f.b>=0){auto& a=stencil[f.a];a.indices[a.count]=f.b;a.weights[a.count++]=f.inverse_mass;auto& b=stencil[f.b];b.indices[b.count]=f.a;b.weights[b.count++]=f.inverse_mass;}}
    for(const auto& f:faces){diagonal[f.a]+=f.inverse_mass;rhs[f.a]-=f.velocity;
        if(f.b>=0){diagonal[f.b]+=f.inverse_mass;rhs[f.b]+=f.velocity;}
    }
    fluid_diagonal=diagonal;
    for(const auto& f:faces)if(f.body>=0)diagonal[f.a]+=1/bodies[f.body].mass+f.torque*f.torque/bodies[f.body].inertia;
    std::vector<std::array<double,3>> impulses(bodies.size());
    auto body_impulses=[&](const std::vector<double>& in){std::fill(impulses.begin(),impulses.end(),std::array<double,3>{});
        for(const int at:wet_faces){const auto& f=faces[at];auto& impulse=impulses[f.body];const auto d=directions[f.direction];impulse[0]+=in[f.a]*d.x;impulse[1]+=in[f.a]*d.y;impulse[2]+=in[f.a]*f.torque;}};
    auto response=[&](const Face& f){const auto& impulse=impulses[f.body];const auto& body=bodies[f.body];const auto d=directions[f.direction];return (impulse[0]*d.x+impulse[1]*d.y)/body.mass+impulse[2]*f.torque/body.inertia;};
    bool coupled=false;
    auto multiply=[&](const std::vector<double>& in,std::vector<double>& out){
        for(std::size_t i=0;i<n;++i){double value=fluid_diagonal[i]*in[i];const auto& row=stencil[i];for(int j=0;j<row.count;++j)value-=row.weights[j]*in[row.indices[j]];out[i]=value;}
        if(coupled){body_impulses(in);for(const int at:wet_faces){const auto& f=faces[at];out[f.a]+=response(f);}}
    };
    auto& stats=w.simulation_stats_;
    if(!bodies.empty()){
        // Material gravity and rigid gravity have distinct established project
        // units. Separate the fluid's reference gravity response; the existing
        // Archimedes force supplies rigid hydrostatics. Only dynamic pressure
        // then exchanges momentum with the body, avoiding double buoyancy.
        std::vector<double> gravity_rhs(n,0);
        for(const auto& f:faces)if(f.body<0){const auto& a=nodes[f.a];const auto& b=nodes[f.b>=0?f.b:f.a];
            const double ga=a.hydrostatic?0:liquid_gravity(*a.definition)*delta,gb=b.hydrostatic?0:liquid_gravity(*b.definition)*delta;
            const double flux=(a.definition->density*ga+b.definition->density*gb)/(a.definition->density+b.definition->density)*directions[f.direction].y;
            gravity_rhs[f.a]-=flux;if(f.b>=0)gravity_rhs[f.b]+=flux;
        }
        stats.liquid_pressure_iterations+=solve_pressure(gravity_rhs,reference,fluid_diagonal,faces,multiply);
        body_impulses(reference);for(auto& f:faces)if(f.body>=0){const double change=response(f);f.velocity-=change;rhs[f.a]+=change;}
    }
    coupled=!bodies.empty();
    double max_rhs=0;for(const auto value:rhs)max_rhs=std::max(max_rhs,std::abs(value));
    // Rebuild a hydrostatic initial guess, never a hidden persistent cache.
    // This removes the large bulk column from PCG's work while leaving the
    // same residual criterion and the same pressure equation in force.
    if(max_rhs>1e-7){
        std::vector<double> level(components,0),weight(components,0),speed(components,0),count(components,0),density(components,0);
        std::vector<std::uint8_t> uniform(components,1);
        for(const auto& node:nodes){const auto c=node.component;if(count[c] && density[c]!=node.definition->density)uniform[c]=0;density[c]=node.definition->density;speed[c]+=node.y;++count[c];}
        for(const auto& face:faces)if(face.b==-1){const int c=nodes[face.a].component;level[c]+=face.outside.y;++weight[c];}
        for(std::size_t i=0;i<n;++i){const auto& node=nodes[i];const int c=node.component;if(uniform[c] && weight[c])pressure[i]=density[c]*speed[c]/count[c]*(node.p.y-level[c]/weight[c]);}
    }
    stats.liquid_divergence_before=max_rhs;
    stats.liquid_pressure_iterations+=solve_pressure(rhs,pressure,diagonal,faces,multiply);
    body_impulses(pressure);
    for(auto& f:faces){if(f.body>=0){const double change=response(f);f.velocity+=change;}
        else f.velocity+=f.inverse_mass*(pressure[f.a]-(f.b>=0?pressure[f.b]:0));}
    for(std::size_t i=0;i<n;++i)r[i]=pressure[i]-reference[i];body_impulses(r);
    for(const auto& f:faces)if(f.body>=0){const double change=response(f);const int c=nodes[f.a].component;const auto d=directions[f.direction];
        energy[c]-=r[f.a]*(f.velocity-0.5*change);momentum_x[c]-=r[f.a]*d.x;momentum_y[c]-=r[f.a]*d.y;}
    for(std::size_t i=0;i<bodies.size();++i){const auto& force=impulses[i];if(std::abs(force[0])+std::abs(force[1])+std::abs(force[2])<1e-10)continue;
        auto& actual=w.bodies_[i];actual.vx=std::clamp(actual.vx+force[0]/bodies[i].mass*60,-512.0,512.0);actual.vy=std::clamp(actual.vy+force[1]/bodies[i].mass*60,-512.0,512.0);
        actual.angular_velocity=std::clamp(actual.angular_velocity+force[2]/bodies[i].inertia*60,-20.0,20.0);actual.sleeping=false;++w.body_revision_;++stats.liquid_body_impulses;
    }
    std::fill(r.begin(),r.end(),0);for(const auto& f:faces){r[f.a]+=f.velocity;if(f.b>=0)r[f.b]-=f.velocity;}
    for(const auto value:r)stats.liquid_divergence_after=std::max(stats.liquid_divergence_after,std::abs(value));
    auto outward=[&](int i,int d){const int f=nodes[i].faces[d];return f<0?0.0:faces[f].velocity*(faces[f].a==i?1:-1);};
    for(std::size_t i=0;i<n;++i){auto& node=nodes[i];
        auto change=[&](int d){const int edge=node.faces[d];return edge<0?-(node.x*directions[d].x+node.y*directions[d].y):(faces[edge].velocity-faces[edge].predicted)*(faces[edge].a==static_cast<int>(i)?1:-1);};
        const double flip_x=node.x+0.5*(change(0)-change(2)),flip_y=node.y+0.5*(change(1)-change(3));
        const double pic_x=0.5*(outward(static_cast<int>(i),0)-outward(static_cast<int>(i),2)),pic_y=0.5*(outward(static_cast<int>(i),1)-outward(static_cast<int>(i),3));
        const double blend=0.05*delta;
        const double terminal=node.definition->terminal_speed_q8/256.0;
        velocity_q16(std::clamp(flip_x*(1-blend)+pic_x*blend,-terminal,terminal),node.state.velocity_x,node.state.momentum_remainder_x);
        velocity_q16(std::clamp(flip_y*(1-blend)+pic_y*blend,-terminal,terminal),node.state.velocity_y,node.state.momentum_remainder_y);
        if(node.ballistic && !node.hydrostatic)energy[node.component]+=node.definition->density*liquid_gravity(*node.definition)*delta*0.5*(vy(*w.cell_state(node.p))+vy(node.state));
    }
    stats.liquid_projection_ms+=elapsed();
    // Route an atmospheric vacancy upstream, then shift every real parcel by
    // one face. No overwrite, long-distance parcel teleport or per-pixel Node.
    // A whole-cell free surface has a one-cell capillary deadband: without
    // pre-existing momentum, transfers must strictly lower gravitational energy.
    std::vector<int> outlets;for(std::size_t f=0;f<faces.size();++f)if(faces[f].b==-1 && faces[f].velocity>1e-6)outlets.push_back(static_cast<int>(f));
    std::stable_sort(outlets.begin(),outlets.end(),[&](int a,int b){return faces[a].velocity>faces[b].velocity;});
    std::vector<std::uint32_t> visited(n,0);std::vector<int> parent(n,-1);std::uint32_t generation=0;
    std::vector<std::uint8_t> used(n,0),credited(n,0),committed(n,0),destination_used(size,0);
    std::vector<int> route_queue,route;route_queue.reserve(n);route.reserve(n);
    for(const int f:outlets){
        const auto& outlet=faces[f];const int start=outlet.a;
        if(used[start] || credited[start] || destination_used[index(outlet.outside)])continue;credited[start]=1;
        auto& head=nodes[start];const bool horizontal=outlet.direction%2==0;
        const auto below_outlet=w.surface_material_at({outlet.outside.x,outlet.outside.y+1});
        const bool spills_down=below_outlet && (*below_outlet==Material::air || defs[static_cast<std::uint16_t>(*below_outlet)]->motion()==Motion::gas || defs[static_cast<std::uint16_t>(*below_outlet)]->motion()==Motion::fire);
        if(horizontal)head.state.liquid_fraction_y=0;else head.state.liquid_fraction_x=0;
        auto& fraction=horizontal?head.state.liquid_fraction_x:head.state.liquid_fraction_y;
        const auto credit=fraction+static_cast<std::uint64_t>(std::llround(volume_unit*outlet.velocity*delta));
        fraction=static_cast<std::uint32_t>(std::min<std::uint64_t>(volume_unit-1,credit));
        auto& queue=route_queue;queue.clear();queue.push_back(start);visited[start]=++generation;parent[start]=-1;int donor=-1;
        for(std::size_t k=0;k<queue.size() && donor<0;++k){const int i=queue[k];++stats.liquid_route_visits;
            bool inlet=false;for(int d=0;d<4;++d){const int edge=nodes[i].faces[d];
                if(edge>=0 && (faces[edge].b==-1 || faces[edge].b==-3) && outward(i,d)<-1e-6)inlet=true;}
            if(inlet){
                const auto& source=nodes[i];const double gravity=liquid_gravity(*source.definition);
                const double energy_at_source=kinetic(source.state);
                const auto& original=*w.cell_state(source.p);
                const bool driven=std::abs(vx(original))+std::abs(vy(original))>=1.0/65536;
                if(source.p.y<outlet.outside.y || (source.p.y==outlet.outside.y && spills_down) || (driven && (gravity==0 || energy_at_source>gravity*std::max(0,source.p.y-outlet.outside.y))))donor=i;
            }
            if(donor>=0)break;
            for(int d=0;d<4;++d){const int edge=nodes[i].faces[d];if(edge<0 || outward(i,d)>=-1e-6)continue;
                const auto& face=faces[edge];const int j=face.a==i?face.b:face.a;
                if(j>=0 && !used[j] && visited[j]!=generation){visited[j]=generation;parent[j]=i;queue.push_back(j);}}
        }
        if(donor<0){head.state.liquid_fraction_x=head.state.liquid_fraction_y=0;continue;}
        if(credit<volume_unit){w.wake(head.p);continue;}
        route.clear();for(int i=donor;i>=0;i=parent[i])route.push_back(i);
        double work=0;CellPos target=outlet.outside;
        for(auto it=route.rbegin();it!=route.rend();++it){const auto& parcel=nodes[*it];work+=parcel.definition->density*liquid_gravity(*parcel.definition)*(target.y-parcel.p.y);target=parcel.p;}
        if(work<0 && energy[head.component]<-work){fraction=0;continue;}
        fraction=static_cast<std::uint32_t>((credit-volume_unit)%volume_unit);
        const auto displaced=*w.material_at(outlet.outside);auto displaced_state=*w.cell_state(outlet.outside);displaced_state.source_attachment=0;
        std::vector<ElectricRelocation> relocations;CellPos destination=outlet.outside;
        for(auto it=route.rbegin();it!=route.rend();++it){const int i=*it;auto& parcel=nodes[i];
            parcel.state.quiet_ticks=parcel.state.adhesion_ticks=0;
            parcel.state.source_attachment=0;
            w.set_cell(destination,parcel.material,parcel.state);relocations.push_back({{parcel.p,0},{destination,0}});
            if(!parcel.ballistic)energy[parcel.component]+=parcel.definition->density*liquid_gravity(*parcel.definition)*(destination.y-parcel.p.y);
            final_positions[i]=destination;
            used[i]=committed[i]=1;destination_used[index(destination)]=1;updated[index(destination)]=transported[index(destination)]=1;destination=parcel.p;
            ++stats.moved_cells;
        }
        w.set_cell(destination,displaced,displaced_state);relocations.push_back({{outlet.outside,0},{destination,0}});
        updated[index(destination)]=transported[index(destination)]=1;
        ElectricalSimulation::transfer_batch(w,relocations);++stats.liquid_transfers;
    }
    // A sealed volume can carry circulation without any free surface. Advect
    // state around directed face cycles instead of silently discarding it.
    std::vector<int> next(n,-1);std::vector<double> cycle_speed(n,0);
    for(std::size_t i=0;i<n;++i)if(!used[i])for(int d=0;d<4;++d){const int edge=nodes[i].faces[d];if(edge<0)continue;
        const auto& face=faces[edge];const int j=face.a==static_cast<int>(i)?face.b:face.a;const double speed=outward(static_cast<int>(i),d);
        if(j>=0 && !used[j] && speed>cycle_speed[i]+1e-6){next[i]=j;cycle_speed[i]=speed;}}
    std::vector<int> marked(n,-1);
    std::vector<int> cycle_path;cycle_path.reserve(n);
    for(std::size_t begin=0;begin<n;++begin){
        if(used[begin] || marked[begin]>=0 || next[begin]<0)continue;
        auto& path=cycle_path;path.clear();int at=static_cast<int>(begin);
        while(at>=0 && !used[at] && marked[at]<0){marked[at]=static_cast<int>(begin);path.push_back(at);at=next[at];}
        if(at<0 || used[at] || marked[at]!=static_cast<int>(begin))continue;
        const auto first=std::find(path.begin(),path.end(),at);std::vector<int> cycle(first,path.end());if(cycle.size()<4)continue;
        double speed=cycle_speed[at],work=0;
        for(const int i:cycle){speed=std::min(speed,cycle_speed[i]);work+=nodes[i].definition->density*liquid_gravity(*nodes[i].definition)*(nodes[next[i]].p.y-nodes[i].p.y);}
        auto& state=nodes[at].state;const bool horizontal=nodes[next[at]].p.x!=nodes[at].p.x;
        if(horizontal)state.liquid_fraction_y=0;else state.liquid_fraction_x=0;
        auto& fraction=horizontal?state.liquid_fraction_x:state.liquid_fraction_y;credited[at]=1;
        const auto credit=fraction+static_cast<std::uint64_t>(std::llround(speed*delta*volume_unit));fraction=static_cast<std::uint32_t>(std::min<std::uint64_t>(volume_unit-1,credit));
        if(credit<volume_unit){w.wake(nodes[at].p);continue;}
        if(work<0 && energy[nodes[at].component]<-work){fraction=0;continue;}
        fraction=static_cast<std::uint32_t>((credit-volume_unit)%volume_unit);std::vector<ElectricRelocation> relocations;
        for(const int i:cycle){auto& parcel=nodes[i];parcel.state.source_attachment=0;const auto to=nodes[next[i]].p;w.set_cell(to,parcel.material,parcel.state);
            final_positions[i]=to;used[i]=committed[i]=1;updated[index(to)]=transported[index(to)]=1;
            if(!parcel.ballistic)energy[parcel.component]+=parcel.definition->density*liquid_gravity(*parcel.definition)*(to.y-parcel.p.y);
            relocations.push_back({{parcel.p,0},{to,0}});++stats.moved_cells;}
        ElectricalSimulation::transfer_batch(w,relocations);++stats.liquid_transfers;
    }
    // Restore the conservative bulk mode lost when sampling face velocities
    // back to parcels. Without a fixed wall, pressure/viscosity exchange equal
    // opposite momentum; only quantization may remove a sub-Q16 remainder.
    std::vector<double> actual_x(components,0),actual_y(components,0);
    for(const auto& node:nodes){actual_x[node.component]+=node.definition->density*vx(node.state);actual_y[node.component]+=node.definition->density*vy(node.state);}
    for(auto& node:nodes){const int c=node.component;if(!free_component[c])continue;
        velocity_q16(vx(node.state)+(momentum_x[c]-actual_x[c])/mass[c],node.state.velocity_x,node.state.momentum_remainder_x);
        if(zero_gravity[c])velocity_q16(vy(node.state)+(momentum_y[c]-actual_y[c])/mass[c],node.state.velocity_y,node.state.momentum_remainder_y);
    }
    std::vector<double> outgoing_energy(components,0);
    for(const auto& node:nodes)outgoing_energy[node.component]+=node.definition->density*kinetic(node.state);
    std::vector<std::uint8_t> body_contact(components,0);for(const auto& face:faces)if(face.body>=0)body_contact[nodes[face.a].component]=1;
    std::vector<double> maximum_speed(components,0);
    for(auto& node:nodes){const double allowed=std::max(0.0,energy[node.component]);
        if(outgoing_energy[node.component]>allowed){const int c=node.component;const double mean_x=free_component[c]?momentum_x[c]/mass[c]:0,mean_y=free_component[c] && zero_gravity[c]?momentum_y[c]/mass[c]:0;
            const double bulk=0.5*mass[c]*(mean_x*mean_x+mean_y*mean_y),variation=std::max(0.0,outgoing_energy[c]-bulk);
            const double scale=variation>0?std::min(1.0,std::sqrt(std::max(0.0,allowed-bulk)/variation)):1;
            velocity_q16(mean_x+(vx(node.state)-mean_x)*scale,node.state.velocity_x,node.state.momentum_remainder_x);velocity_q16(mean_y+(vy(node.state)-mean_y)*scale,node.state.velocity_y,node.state.momentum_remainder_y);}
        maximum_speed[node.component]=std::max(maximum_speed[node.component],std::abs(vx(node.state))+std::abs(vy(node.state)));
    }
    for(std::size_t i=0;i<n;++i){auto& node=nodes[i];updated[index(node.p)]=1;
        // Whole-cell surface reconstruction can add spurious kinetic energy.
        // Limit each connected component by its incoming energy plus
        // the actual gravitational work of its committed face transfers.
        const bool contact_rest=node.hydrostatic && !body_contact[node.component] && node.definition->damping_milli<1000 && maximum_speed[node.component]<1.0/256;
        bool surface=false;for(const int edge:node.faces)surface=surface || (edge>=0 && faces[edge].b==-1);
        const bool reconstruct=!committed[i] && surface && node.definition->gravity_q8 && !body_contact[node.component] &&
            std::abs(vx(node.state))+std::abs(vy(node.state))<1.0/32 && !node.state.liquid_fraction_x && !node.state.liquid_fraction_y;
        if(contact_rest || reconstruct){node.state.velocity_x=node.state.velocity_y=0;node.state.momentum_remainder_x=node.state.momentum_remainder_y=0;
            if(contact_rest)node.state.liquid_fraction_x=node.state.liquid_fraction_y=0;}
        // A sub-cell pressure circulation with no admissible whole-cell route
        // is a contact reaction at this resolution, rather than endless jitter.
        if(!committed[i] && !credited[i])node.state.liquid_fraction_x=node.state.liquid_fraction_y=0;
        node.state.horizontal_fraction=node.state.vertical_fraction=0;
        w.set_cell(final_positions[i],node.material,node.state);
        const bool momentum=node.state.velocity_x || node.state.velocity_y || node.state.momentum_remainder_x || node.state.momentum_remainder_y;
        if(momentum)w.wake(final_positions[i]);
        // Pressure flux can be below the representable whole-cell surface
        // geometry. Leave motionless, unreserved parcels to the existing
        // monotone row-outlet/diagonal reconstruction, which closes pixel gaps
        // without inventing a continuous momentum impulse.
        if(!committed[i] && !momentum && !node.state.liquid_fraction_x && !node.state.liquid_fraction_y)
            updated[index(node.p)]=0;
    }
    stats.liquid_transport_ms+=elapsed();
}
}
