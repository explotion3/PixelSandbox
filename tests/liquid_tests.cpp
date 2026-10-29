#include "session/game_session.hpp"
#include "simulation/material_simulation.hpp"
#include "simulation/liquid_simulation.hpp"
#include "persistence/world_file.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace pixel;
namespace {
void need(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
std::shared_ptr<const ContentRegistry> catalog(int gravity=256,int damping=900){
    MaterialDefinition stone;stone.id="material.stone";stone.display_name="Stone";
    MaterialDefinition water;water.id="material.water";water.display_name="Water";water.sand=false;water.is_static=false;water.density=4;water.gravity_q8=gravity;water.damping_milli=damping;
    auto oil=water;oil.id="material.oil";oil.display_name="Oil";oil.density=1;
    const auto result=ContentRegistry::build({stone,water,oil});need(bool(result),result.error);return result.registry;
}
WorldSnapshot blank(const ContentRegistry& content){
    auto s=MaterialSimulation::make_lab(20261027,content).snapshot();s.bounds={{-32,32},64,48};
    s.cells.assign(64*48,0);s.states.assign(s.cells.size(),{});s.initial_stone=0;return s;
}
std::size_t index(const WorldSnapshot& s,int x,int y){return static_cast<std::size_t>(y-s.bounds.origin.y)*s.bounds.width+x-s.bounds.origin.x;}
void put(WorldSnapshot& s,int x,int y,std::uint16_t material){const auto i=index(s,x,y);if(s.cells[i]==1)--s.initial_stone;s.cells[i]=material;s.states[i]={};if(material==1)++s.initial_stone;}
void restore(GameSession& session,const WorldSnapshot& s){const auto result=session.restore_world(s);need(result.ok,result.error);need(bool(session.resume()),"resume");}
void step(GameSession& session,int n=1){for(int i=0;i<n;++i)need(session.advance_fixed_step(),"advance");}
int surface(const GameSession& session,int x){for(int y=32;y<80;++y)if(session.world()->material_at({x,y})==session.world()->material_index("material.water"))return y;return 80;}
double mechanical_energy(const GameSession& s){double value=0;const auto snapshot=*s.capture_snapshot();for(std::size_t i=0;i<snapshot.cells.size();++i)if(snapshot.cells[i]>1){const auto* d=s.content().find_material(snapshot.material_ids[snapshot.cells[i]-1]);const auto& state=snapshot.states[i];const double y=snapshot.bounds.origin.y+static_cast<int>(i/snapshot.bounds.width);value+=d->density*(0.5*(std::pow((state.velocity_x+state.momentum_remainder_x/256.0)/256,2)+std::pow((state.velocity_y+state.momentum_remainder_y/256.0)/256,2))-liquid_gravity(*d)*y);}return value;}
std::shared_ptr<const ContentRegistry> source_catalog(){
    auto definitions=catalog()->materials();
    for(auto& d:definitions)if(!d.is_static){d.source_liquid=true;d.liquid_gravity=1.5f;d.liquid_damping=0.8f;d.liquid_flow_speed=0.9f;}
    const auto c=ContentRegistry::build(std::move(definitions));need(bool(c),c.error);return c.registry;
}
void source_motion(){
    const auto c=source_catalog();GameSession s(c);auto snap=blank(*c);
    put(snap,-1,63,2);restore(s,snap);step(s);
    need(s.world()->material_at({-1,64})==s.world()->material_index("material.water"),"source first fall crosses a chunk boundary by one cell");
    need(std::abs(s.world()->cell_state({-1,64})->liquid_fall-0.375f)<1e-6,"raw gravity feeds the source accumulator, not metres/sec");
    need(s.published_world()->liquid_pressure_iterations==0 && s.published_world()->liquid_substeps==0,"source liquid bypasses pressure projection");
    snap=blank(*c);for(int x=-32;x<32;++x)put(snap,x,65,1);put(snap,-2,64,1);put(snap,-1,64,2);
    restore(s,snap);step(s);need(s.world()->material_at({-1,64})==s.world()->material_index("material.water"),"sub-cell lateral credit is retained");
    need(std::abs(s.world()->cell_state({-1,64})->liquid_lateral-0.9f)<1e-6,"one-sided source flow adds raw flow speed");
    step(s);need(s.world()->material_at({2,64})==s.world()->material_index("material.water"),"lateral damping permits multiple checked swaps");
    need(std::abs(s.world()->cell_state({2,64})->liquid_lateral-0.9216f)<1e-5,"successful side swaps multiply credit by damping");
    // A high impulse still checks each intermediate cell and cannot tunnel.
    snap=blank(*c);for(int y=32;y<80;++y)put(snap,0,y,1);put(snap,-2,50,2);snap.states[index(snap,-2,50)].velocity_x=8192;
    restore(s,snap);step(s,80);for(int y=32;y<80;++y)for(int x=1;x<32;++x)need(s.world()->material_at({x,y})!=s.world()->material_index("material.water"),"source impulse cannot cross a one-cell wall");
    need(s.world()->material_count(s.world()->material_index("material.water"))==1,"source falling parcel remains inside finite world");
    // Fully supported rectangular pool: no arbitrary deadline for mixed fluids.
    snap=blank(*c);for(int y=32;y<80;++y)for(int x=-32;x<32;++x)put(snap,x,y,1);
    for(int y=54;y<64;++y)for(int x=-8;x<8;++x)put(snap,x,y,2);
    restore(s,snap);step(s,5);auto steady=*s.capture_snapshot();static_cast<void>(s.take_changes());step(s,100);auto after=*s.capture_snapshot();after.tick=steady.tick;
    need(after==steady && !s.published_world()->active_chunks && s.take_changes().dirty_chunks.empty(),"enclosed source pool sleeps for 100 ticks without churn");
    // Opening a wall wakes the previously sleeping pool.
    const auto erased=s.submit({CommandKind::erase,{8,60},1,{}});need(bool(erased),erased.error);step(s,30);
    need(s.world()->material_at({8,60})==s.world()->material_index("material.water"),"local opening wakes a resting pool");
}
void source_density(){
    const auto c=source_catalog();int immediate=0;
    for(std::uint64_t seed=1;seed<=128;++seed){GameSession s(c);auto snap=blank(*c);snap.seed=seed;snap.tick=5;
        for(int y=32;y<80;++y)for(int x=-32;x<32;++x)put(snap,x,y,1);
        put(snap,-1,63,2);put(snap,-1,64,3);snap.states[index(snap,-1,63)].created_tick=3;snap.states[index(snap,-1,64)].created_tick=4;
        restore(s,snap);step(s);if(s.world()->material_at({-1,64})==s.world()->material_index("material.water"))++immediate;
        step(s,80);need(s.world()->material_at({-1,64})==s.world()->material_index("material.water"),"heavier source liquid eventually exchanges across chunk seam");
        need(s.world()->cell_state({-1,64})->created_tick==3 && s.world()->cell_state({-1,63})->created_tick==4,"density exchange carries both complete parcel states");
        need(s.world()->material_count(s.world()->material_index("material.water"))==1 && s.world()->material_count(s.world()->material_index("material.oil"))==1,"density exchange preserves both quantities");
    }
    std::cout<<"source density immediate swaps="<<immediate<<"/128\n";need(immediate>0 && immediate<128,"density is gated, not an unconditional swap");
}
void source_save(){
    const auto c=source_catalog();GameSession a(c),b(c);auto snap=blank(*c);
    for(int x=-32;x<32;++x)put(snap,x,65,1);put(snap,-2,64,1);put(snap,-1,64,2);
    restore(a,snap);step(a);const auto moving=*a.capture_snapshot();
    const auto path=std::filesystem::temp_directory_path()/("PixelSandbox-source-liquid-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".psw");
    const auto saved=persistence::save_world_file(path,moving);need(saved.ok,saved.error);auto loaded=persistence::load_world_file(path);std::filesystem::remove(path);
    need(loaded.snapshot && *loaded.snapshot==moving,"source float accumulators roundtrip exactly");restore(b,*loaded.snapshot);
    for(int t=0;t<120;++t){step(a);step(b);need(a.capture_snapshot()==b.capture_snapshot(),"source active save preserves subsequent trajectory");}
    auto bad=*a.capture_snapshot();const auto before=a.capture_snapshot();auto found=std::find(bad.cells.begin(),bad.cells.end(),2);need(found!=bad.cells.end(),"water survived");
    bad.states[static_cast<std::size_t>(found-bad.cells.begin())].liquid_lateral=std::numeric_limits<float>::quiet_NaN();
    need(!a.restore_world(bad) && a.capture_snapshot()==before,"nonfinite source state rejected atomically");
}

void pressure(){
    const auto c=catalog();GameSession s(c);auto snap=blank(*c);
    // U-shaped channel: both legs and the only bottom connection cross chunks.
    for(int y=32;y<80;++y)for(int x=-32;x<32;++x)put(snap,x,y,1);
    for(int y=36;y<74;++y)for(int x=-26;x<26;++x){
        if(y<70 && x>=-18 && x<18)continue;
        put(snap,x,y,(y>=70 || (x<-18 && y>=44) || (x>=18 && y>=64))?2:0);
    }
    restore(s,snap);const auto amount=s.world()->material_count(s.world()->material_index("material.water"));
    bool rose=false;double worst=0;const double incoming_energy=mechanical_energy(s),voxel_energy=amount*4*physical::gravity_per_tick;
    int elapsed=0;
    for(;elapsed<8000;++elapsed){step(s);rose=rose || surface(s,21)<64;worst=std::max(worst,s.published_world()->liquid_divergence_after);need(s.world()->material_count(s.world()->material_index("material.water"))==amount,"U-tube volume conserved every step");need(mechanical_energy(s)<=incoming_energy+voxel_energy+1e-6,"unforced pressure energy stays within the one-cell surface quantization bound");if(elapsed>100 && !s.published_world()->active_chunks && !s.published_world()->moved_cells)break;}
    std::cout<<"U surfaces="<<surface(s,-23)<<','<<surface(s,21)<<" max_divergence="<<worst<<" settling_ticks="<<std::min(elapsed+1,8000)<<'\n';
    need(rose,"pressure must drive liquid UP the receiving leg");need(std::abs(surface(s,-23)-surface(s,21))<=1,"U-tube equalizes through its bottom connection");
    need(worst<1e-5,"pressure projection divergence residual");
    const auto steady=*s.capture_snapshot();static_cast<void>(s.take_changes());step(s,100);auto after=*s.capture_snapshot();after.tick=steady.tick;
    need(after==steady && !s.published_world()->active_chunks && s.take_changes().dirty_chunks.empty(),"hydrostatic liquid sleeps without state or display churn");
    // Close the sole connection: pressure cannot reach the isolated leg.
    for(int y=70;y<74;++y)put(snap,0,y,1);restore(s,snap);step(s,200);
    need(surface(s,-23)==44 && surface(s,21)==64,"wall prevents pressure leakage between sealed basins");
    snap=blank(*c);for(int y=32;y<76;++y){put(snap,-11,y,1);put(snap,11,y,1);}for(int x=-11;x<=11;++x)put(snap,x,32,1);
    for(int y=33;y<50;++y)for(int x=-10;x<=10;++x)put(snap,x,y,2);restore(s,snap);
    int detached=0;for(;detached<60;++detached){step(s);bool clear=true;for(int x=-10;x<=10;++x)clear=clear && s.world()->material_at({x,33})==Material::air;if(clear)break;}
    std::cout<<"ceiling detachment ticks="<<detached+1<<'\n';
    for(int x=-10;x<=10;++x)need(s.world()->material_at({x,33})==Material::air,"ceiling contact detaches without a fictitious tensile pressure support");
    need(s.world()->material_count(s.world()->material_index("material.water"))==357,"ceiling detachment preserves every parcel");
}
void momentum(){
    const auto c=catalog(0,1000);GameSession s(c);auto snap=blank(*c);
    for(int y=48;y<60;++y)for(int x=-14;x<14;++x){put(snap,x,y,2);if(x<-10)snap.states[index(snap,x,y)].velocity_x=256;}
    restore(s,snap);step(s);const auto view=*s.published_world();
    bool shared=false;for(int y=48;y<60;++y)for(int x=-10;x<4;++x){const auto* state=s.world()->cell_state({x,y});shared=shared || state->velocity_x>0;}
    std::cout<<"momentum divergence="<<view.liquid_divergence_before<<" -> "<<view.liquid_divergence_after<<" transfers="<<view.liquid_transfers<<'\n';
    need(shared,"incoming momentum is transmitted to previously stationary liquid");need(view.liquid_divergence_after<1e-5,"momentum is projected to incompressible face flux");
    step(s,30);need(s.world()->material_count(s.world()->material_index("material.water"))==336,"momentum transport conserves volume");
    // A one-cell barrier and world boundary must withstand high velocities.
    snap=blank(*c);for(int y=32;y<80;++y)put(snap,0,y,1);
    for(int y=45;y<55;++y)for(int x=-5;x<0;++x){put(snap,x,y,2);snap.states[index(snap,x,y)].velocity_x=2048;}
    restore(s,snap);step(s,100);for(int y=32;y<80;++y)for(int x=1;x<32;++x)need(s.world()->material_at({x,y})!=s.world()->material_index("material.water"),"pressure and momentum do not tunnel through thin walls");
    // Closed circulation also transports actual state, not just a velocity view.
    snap=blank(*c);for(int y=32;y<80;++y)for(int x=-32;x<32;++x)put(snap,x,y,1);snap.tick=10;
    const CellPos loop[]{{-1,63},{0,63},{0,64},{-1,64}};
    for(int i=0;i<4;++i){put(snap,loop[i].x,loop[i].y,2);auto& state=snap.states[index(snap,loop[i].x,loop[i].y)];
        state.created_tick=i+1;state.velocity_x=i<2?256:-256;state.velocity_y=i==0 || i==3?-256:256;}
    restore(s,snap);bool circulated=false;for(int t=0;t<6;++t){step(s);circulated=circulated || s.world()->cell_state(loop[0])->created_tick!=1;}
    need(circulated,"a sealed vortex transports saved parcel state around a closed face cycle");
    // Uniform translation has no viscous gradient: mixed-density parcels must
    // retain their total momentum and move more than one cell per fixed tick.
    const auto viscous=catalog(0,900);GameSession plug(viscous);snap=blank(*viscous);
    for(int y=48;y<60;++y)for(int x=-14;x<14;++x){put(snap,x,y,x<0?2:3);snap.states[index(snap,x,y)].velocity_x=384;}
    restore(plug,snap);const auto incoming=mechanical_energy(plug);step(plug,6);const auto translated=*plug.capture_snapshot();
    std::int64_t total=0;for(std::size_t i=0;i<translated.cells.size();++i)if(translated.cells[i]>1){need(translated.states[i].velocity_x==384 && translated.states[i].velocity_y==0,"viscosity preserves uniform bulk velocity");total+=static_cast<std::int64_t>(translated.states[i].velocity_x)*(translated.cells[i]==2?4:1);}
    need(total==static_cast<std::int64_t>(14*12*5)*384 && mechanical_energy(plug)==incoming,"closed internal exchanges preserve mixed-density bulk momentum and energy");
    need(plug.world()->material_at({-5,48})==plug.world()->material_index("material.water") && plug.world()->material_at({-14,48})==Material::air,"CFL substeps transport a 1.5-cell-per-tick plug nine whole cells");
    snap=blank(*viscous);for(int y=48;y<52;++y)for(int x=-14;x<-6;++x){put(snap,x,y,2);snap.states[index(snap,x,y)].momentum_remainder_x=128;}
    restore(plug,snap);step(plug,520);
    need(plug.world()->material_at({-14,48})==Material::air && plug.world()->material_at({-6,48})==plug.world()->material_index("material.water"),"sub-Q8 uniform momentum eventually transports a real cell");
    const auto slow_state=*plug.capture_snapshot();for(const auto& state:slow_state.states)if(state.momentum_remainder_x)need(state.velocity_x==0 && state.momentum_remainder_x==128,"uniform fine momentum is preserved without viscous bulk damping");
    // A sealed pressure solve has no atmospheric gauge anchor.
    snap=blank(*c);for(int y=32;y<80;++y)for(int x=-32;x<32;++x)put(snap,x,y,1);
    for(const auto p:loop)put(snap,p.x,p.y,2);snap.states[index(snap,-1,63)].velocity_x=256;restore(s,snap);step(s);
    need(s.published_world()->liquid_pressure_iterations>0 && s.published_world()->liquid_divergence_after<1e-6 && s.world()->material_count(s.world()->material_index("material.water"))==4,"sealed compatible pressure solves without adding a fake outlet");
}
void saved(){
    const auto c=catalog();GameSession a(c),b(c);auto s=blank(*c);
    for(int x=-32;x<32;++x)put(s,x,77,1);
    for(int y=52;y<68;++y)for(int x=-16;x<-4;++x){put(s,x,y,2);s.states[index(s,x,y)].velocity_x=384;}
    restore(a,s);step(a,13);const auto moving=*a.capture_snapshot();
    need(std::any_of(moving.states.begin(),moving.states.end(),[](const auto& state){return state.liquid_fraction_x || state.liquid_fraction_y;}),"save fixture contains pending sub-cell pressure flux");
    need(std::any_of(moving.states.begin(),moving.states.end(),[](const auto& state){return state.momentum_remainder_x || state.momentum_remainder_y;}),"save fixture contains sub-Q8 momentum");
    const auto file=std::filesystem::temp_directory_path()/("PixelSandbox-pressure-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".psw");
    const auto written=persistence::save_world_file(file,moving);need(written.ok,written.error);const auto read=persistence::load_world_file(file);std::error_code error;std::filesystem::remove(file,error);
    need(read.snapshot && *read.snapshot==moving,"pressure motion saved exactly in existing format");restore(b,*read.snapshot);
    for(int t=0;t<150;++t){step(a);step(b);need(a.capture_snapshot()==b.capture_snapshot(),"pressure field rebuild preserves saved future");}
    const auto before=a.capture_snapshot();auto bad=*before;bad.states[index(bad,-10,76)].velocity_x=9000;need(!a.restore_world(bad) && a.capture_snapshot()==before,"invalid momentum cannot replace world");
    bad=*before;bad.states[index(bad,-10,76)].liquid_fraction_x=1u<<24;need(!a.restore_world(bad) && a.capture_snapshot()==before,"invalid liquid volume credit cannot replace world");
    bad=*before;bad.states[index(bad,-10,76)].momentum_remainder_x=256;need(!a.restore_world(bad) && a.capture_snapshot()==before,"invalid fine momentum cannot replace world");
}
void head(){
    const auto c=catalog();std::array<std::uint64_t,2> escaped{};
    for(int case_index=0;case_index<2;++case_index){GameSession s(c);auto snap=blank(*c);const int top=case_index?40:60;
        for(int y=32;y<80;++y)for(int x=-32;x<32;++x)put(snap,x,y,1);
        for(int y=36;y<74;++y)for(int x=-20;x<19;++x)put(snap,x,y,y>=top?2:0);
        for(int y=36;y<74;++y)for(int x=20;x<30;++x)put(snap,x,y,0);
        put(snap,19,66,0);restore(s,snap);const auto amount=s.world()->material_count(s.world()->material_index("material.water"));
        for(int t=0;t<60;++t){step(s);need(s.world()->material_count(s.world()->material_index("material.water"))==amount,"pressurized outlet conserves exact liquid count");}
        for(int y=32;y<80;++y)for(int x=19;x<32;++x)if(s.world()->material_at({x,y})==s.world()->material_index("material.water"))++escaped[case_index];
    }
    std::cout<<"outlet shallow/deep="<<escaped[0]<<'/'<<escaped[1]<<'\n';need(escaped[0]>0 && escaped[1]>escaped[0],"deeper pressure head drives greater outflow through the same opening");
}
}
int main(int argc,char** argv){try{const std::string name=argc==2?argv[1]:"";if(name=="pressure")pressure();else if(name=="momentum")momentum();else if(name=="pressure_save")saved();else if(name=="pressure_head")head();else if(name=="source_liquid_motion")source_motion();else if(name=="source_liquid_density")source_density();else if(name=="source_liquid_save")source_save();else throw std::runtime_error("Unknown liquid test");std::cout<<"PASS "<<name<<'\n';return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
