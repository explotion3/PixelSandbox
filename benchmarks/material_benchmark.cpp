#include "session/game_session.hpp"
#include "simulation/material_simulation.hpp"
#include "content/reaction_table.hpp"
#include "physics/box2d_solver.hpp"
#include "persistence/world_file.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "benchmark_material_catalog.hpp"

using namespace pixel;
namespace {
void require(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
WorldSnapshot fixture(const ContentRegistry& content,const std::string& name){
    auto snap=MaterialSimulation::make_lab(20261027,content,name=="lab").snapshot();
    if(name=="lab")return snap;
    const auto& bounds=snap.bounds;
    snap.cells.assign(snap.cells.size(),0);snap.states.assign(snap.states.size(),{});snap.initial_stone=0;
    auto put=[&](int x,int y,const std::string& id){
        const auto found=std::find(snap.material_ids.begin(),snap.material_ids.end(),id);
        require(found!=snap.material_ids.end(),"Missing benchmark material: "+id);
        const auto i=static_cast<std::size_t>(y)*bounds.width+x;
        snap.cells[i]=static_cast<std::uint16_t>(1+found-snap.material_ids.begin());
        snap.states[i]=MaterialSimulation::initial_state(*content.find_material(id));
        if(snap.cells[i]==1)++snap.initial_stone;
    };
    for(int y=0;y<bounds.height;++y)for(int x=0;x<bounds.width;++x){
        if(!x || !y || x==bounds.width-1 || y==bounds.height-1){put(x,y,"material.stone");continue;}
        // Dense but still moving: 3/4 of the interior, with distributed air.
        if(name=="water" && y<96)put(x,y,"material.water");
        if(name=="wide_pool" && x<=20 && y>=60 && y<85)put(x,y,"material.water");
        if(name=="steam" && (x+y)%4)put(x,y,"material.steam");
        if(name=="mixed" && (x+y)%4)put(x,y,y<64?"material.steam":"material.water");
        if(name=="thermal" && (x+y)%4)put(x,y,y<64?"material.steam":"material.water");
    }
    if(name=="thermal")for(std::size_t i=0;i<snap.cells.size();++i)if(snap.cells[i]>1)snap.states[i].temperature=(i%7==0)?120:0;
    return snap;
}
}
int main(int argc,char** argv){try{
    require(argc>=2 && argc<=5,"Usage: pixel_material_benchmark GAME_PATH [OUTPUT_DIRECTORY] [STEPS] [CASE]");
    std::ifstream file(std::filesystem::path(argv[1])/"content/reactions.txt");require(bool(file),"Cannot read reaction table");
    std::ostringstream text;text<<file.rdbuf();auto rules=parse_reaction_table(text.str());require(bool(rules),rules.error);
    auto catalog=ContentRegistry::build(benchmark_materials(),std::move(rules.reactions));require(bool(catalog),catalog.error);
    const int steps=argc>3?std::stoi(argv[3]):120;require(steps>=30,"At least 30 measured steps required");
    std::filesystem::path output;if(argc>2){output=argv[2];std::filesystem::create_directories(output);}
    std::cout<<"materials="<<catalog.registry->material_count()<<" rules="<<catalog.registry->reactions().size()<<" seed=20261027 bounds=256x128 warmup=10 steps="<<steps<<'\n';
    const std::vector<std::string> cases{"water","steam","mixed","thermal","wide_pool","lab"};
    const std::string selected=argc>4?argv[4]:"";require(selected.empty() || std::find(cases.begin(),cases.end(),selected)!=cases.end(),"Unknown benchmark case: "+selected);
    std::cout<<"case,median_ms,p95_ms,p99_ms,max_ms,mean_visited,mean_moved,mean_pressure_iterations,mean_substeps,mean_route_visits,setup_ms,projection_ms,transport_ms\n";
    for(const auto& name:cases){if(!selected.empty() && name!=selected)continue;
        GameSession session(catalog.registry,make_box2d_solver());const auto restored=session.restore_world(fixture(*catalog.registry,name));require(restored.ok,restored.error);require(bool(session.resume()),"Cannot resume benchmark");
        std::vector<double> timings;std::uint64_t visited=0,moved=0,iterations=0,substeps=0,route_visits=0;double setup=0,projection=0,transport=0;
        for(int i=0;i<steps+10;++i){
            const auto start=std::chrono::steady_clock::now();require(session.advance_fixed_step(),"Benchmark step failed");
            const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(name=="wide_pool")require(session.world()->material_count(session.world()->material_index("material.water"))==500,"Wide-pool benchmark lost liquid volume");
            if(i>=10){timings.push_back(ms);const auto view=*session.published_world();visited+=view.visited_cells;moved+=view.moved_cells;iterations+=view.liquid_pressure_iterations;substeps+=view.liquid_substeps;route_visits+=view.liquid_route_visits;setup+=view.liquid_setup_ms;projection+=view.liquid_projection_ms;transport+=view.liquid_transport_ms;}
            static_cast<void>(session.take_changes());
        }
        std::sort(timings.begin(),timings.end());auto percentile=[&](double q){return timings[static_cast<std::size_t>((timings.size()-1)*q)];};
        std::cout<<name<<std::fixed<<std::setprecision(3)<<','<<percentile(.5)<<','<<percentile(.95)<<','<<percentile(.99)<<','<<timings.back()<<','<<visited/steps<<','<<moved/steps<<','<<iterations/steps<<','<<substeps/steps<<','<<route_visits/steps<<','<<setup/steps<<','<<projection/steps<<','<<transport/steps<<std::endl;
        if(!output.empty()){const auto saved=persistence::save_world_file(output/(name+".psw"),*session.capture_snapshot());require(saved.ok,saved.error);}
    }
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
