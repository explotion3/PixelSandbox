#include "physics/box2d_solver.hpp"
#include "core/physical_units.hpp"
#include <box2d/box2d.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace pixel {
namespace {
constexpr float cells_per_meter=static_cast<float>(physical::cells_per_meter);
struct Rect {int x,y,width,height;std::uint16_t material;};
std::vector<Rect> rectangles(std::vector<std::uint16_t> mask,int edge){
    std::vector<Rect> result;
    for(int y=0;y<edge;++y)for(int x=0;x<edge;++x){
        const auto material=mask[static_cast<std::size_t>(y)*edge+x];if(!material)continue;
        int width=1;while(x+width<edge && mask[static_cast<std::size_t>(y)*edge+x+width]==material)++width;
        int height=1;
        while(y+height<edge){bool full=true;for(int dx=0;dx<width;++dx)if(mask[static_cast<std::size_t>(y+height)*edge+x+dx]!=material){full=false;break;}if(!full)break;++height;}
        for(int dy=0;dy<height;++dy)for(int dx=0;dx<width;++dx)mask[static_cast<std::size_t>(y+dy)*edge+x+dx]=0;
        result.push_back({x,y,width,height,material});
    }
    return result;
}
class BoxSolver final:public RigidSolver {
    struct Surface {int x,y,width,height;};
    struct Body {b2BodyId native{};std::uint64_t revision=0;BodyMotion last{};};
    struct Terrain {std::uint64_t revision=0;std::vector<std::uint16_t> mask;b2BodyId native{};std::vector<Surface> surfaces;};
    b2WorldId world_{};
    std::unordered_map<std::uint32_t,Body> bodies_;
    std::vector<Terrain> terrain_;
    WorldBounds bounds_;
    const WorldState* contact_world_=nullptr;
    const std::vector<const MaterialDefinition*>* contact_definitions_=nullptr;
    static bool pre_solve(b2ShapeId a,b2ShapeId b,b2Manifold* manifold,void* context){
        const auto& self=*static_cast<BoxSolver*>(context);
        if(!self.contact_world_ || !manifold->pointCount)return true;
        const auto* surface=static_cast<const Surface*>(b2Shape_GetUserData(a));
        auto normal=manifold->normal;
        if(!surface){surface=static_cast<const Surface*>(b2Shape_GetUserData(b));normal={-normal.x,-normal.y};}
        if(!surface)return true;
        // Adjacent terrain rectangles (including chunk/material seams) have
        // internal faces. Suppress those contacts, retaining every exposed
        // wall/step face. This is separate from material friction mixing.
        for(int i=0;i<manifold->pointCount;++i){
            const auto p=manifold->points[i].point;
            double x=self.bounds_.origin.x+p.x*cells_per_meter,y=self.bounds_.origin.y+p.y*cells_per_meter;
            if(std::abs(normal.x)>std::abs(normal.y)){
                x=surface->x+(normal.x>0?surface->width+0.25:-0.25);
                y=std::clamp(y,surface->y+0.01,surface->y+surface->height-0.01);
            }else{
                y=surface->y+(normal.y>0?surface->height+0.25:-0.25);
                x=std::clamp(x,surface->x+0.01,surface->x+surface->width-0.01);
            }
            const CellPos at{static_cast<int>(std::floor(x)),static_cast<int>(std::floor(y))};
            const auto m=self.contact_world_->material_at(at);if(!m || *m==Material::air)return true;
            const auto* d=(*self.contact_definitions_)[static_cast<std::uint16_t>(*m)];
            if(!d->sand || d->motion(self.contact_world_->cell_state(at)->mobilized)!=Motion::fixed)return true;
        }
        return false;
    }
    b2Vec2 position(double x,double y)const{return {static_cast<float>((x-bounds_.origin.x)/cells_per_meter),static_cast<float>((y-bounds_.origin.y)/cells_per_meter)};}
    static void box(b2BodyId body,float cx,float cy,float width,float height,float density,float friction=0.6f,Surface* surface=nullptr){
        auto shape=b2DefaultShapeDef();shape.density=density;shape.material.friction=friction;shape.material.restitution=0.05f;
        shape.userData=surface;shape.enablePreSolveEvents=surface!=nullptr;
        const auto polygon=b2MakeOffsetBox(width/2,height/2,{cx,cy},b2Rot_identity);b2CreatePolygonShape(body,&shape,&polygon);
    }
    void initialize(const WorldState& w){
        bounds_=w.view().bounds;auto def=b2DefaultWorldDef();def.gravity={0,static_cast<float>(physical::gravity)};def.workerCount=0;
        world_=b2CreateWorld(&def);terrain_.resize(w.chunks().size());b2World_SetPreSolveCallback(world_,pre_solve,this);
        auto fixed=b2DefaultBodyDef();const auto wall=b2CreateBody(world_,&fixed);
        const auto width=bounds_.width/cells_per_meter,height=bounds_.height/cells_per_meter;
        box(wall,width/2,-0.5f/cells_per_meter,width+2/cells_per_meter,1/cells_per_meter,0);
        box(wall,width/2,height+0.5f/cells_per_meter,width+2/cells_per_meter,1/cells_per_meter,0);
        box(wall,-0.5f/cells_per_meter,height/2,1/cells_per_meter,height,0);
        box(wall,width+0.5f/cells_per_meter,height/2,1/cells_per_meter,height,0);
    }
public:
    ~BoxSolver()override{reset();}
    void reset()noexcept override{if(b2World_IsValid(world_))b2DestroyWorld(world_);world_={};bodies_.clear();terrain_.clear();}
    std::vector<BodyMotion> step(const WorldState& w,const ContentRegistry& content,double seconds)override{
        if(w.bodies().empty()){reset();return {};}
        if(!b2World_IsValid(world_))initialize(w);
        std::vector<const MaterialDefinition*> definitions{nullptr};for(const auto& id:w.material_ids())definitions.push_back(content.find_material(id));
        for(std::size_t i=0;i<w.chunks().size();++i){
            const auto& chunk=w.chunks()[i];auto& cache=terrain_[i];if(cache.revision==chunk.collision_revision)continue;
            std::vector<std::uint16_t> mask(chunk_size*chunk_size,0);
            for(std::size_t j=0;j<mask.size();++j){
                const auto m=static_cast<std::uint16_t>(chunk.cells[j]);if(!m)continue;const auto* d=definitions[m];
                if(d->sand && d->motion(chunk.states[j].mobilized)==Motion::fixed)mask[j]=m;
            }
            cache.revision=chunk.collision_revision;if(cache.mask==mask)continue;cache.mask=mask;
            if(b2Body_IsValid(cache.native))b2DestroyBody(cache.native);cache.native={};cache.surfaces.clear();
            const auto rects=rectangles(std::move(mask),chunk_size);if(rects.empty())continue;
            auto def=b2DefaultBodyDef();def.position=position(chunk.position.x*chunk_size,chunk.position.y*chunk_size);cache.native=b2CreateBody(world_,&def);
            cache.surfaces.reserve(rects.size());
            for(const auto& r:rects)cache.surfaces.push_back({chunk.position.x*chunk_size+r.x,chunk.position.y*chunk_size+r.y,r.width,r.height});
            for(std::size_t j=0;j<rects.size();++j){const auto& r=rects[j];box(cache.native,(r.x+r.width*0.5f)/cells_per_meter,(r.y+r.height*0.5f)/cells_per_meter,r.width/cells_per_meter,r.height/cells_per_meter,0,definitions[r.material]->solid_friction,&cache.surfaces[j]);}
        }
        for(auto it=bodies_.begin();it!=bodies_.end();){
            if(std::none_of(w.bodies().begin(),w.bodies().end(),[&](const auto& b){return b.id==it->first;})){b2DestroyBody(it->second.native);it=bodies_.erase(it);}else ++it;
        }
        for(const auto& b:w.bodies()){
            auto& cache=bodies_[b.id];
            if(cache.revision!=b.shape_revision || !b2Body_IsValid(cache.native)){
                if(b2Body_IsValid(cache.native))b2DestroyBody(cache.native);
                auto def=b2DefaultBodyDef();def.type=b2_dynamicBody;def.position=position(b.x,b.y);def.rotation=b2MakeRot(static_cast<float>(b.angle));def.isBullet=true;
                cache.native=b2CreateBody(world_,&def);
                std::vector<std::uint16_t> mask(65*65,0);for(const auto& p:b.pixels)mask[static_cast<std::size_t>(p.local.y+32)*65+p.local.x+32]=p.material;
                for(const auto& r:rectangles(std::move(mask),65))box(cache.native,(r.x-32+r.width*0.5f-0.5f)/cells_per_meter,(r.y-32+r.height*0.5f-0.5f)/cells_per_meter,r.width/cells_per_meter,r.height/cells_per_meter,definitions[r.material]->density,definitions[r.material]->solid_friction);
                cache.revision=b.shape_revision;cache.last.id=0;
            }
            if(cache.last.id!=b.id || cache.last.x!=b.x || cache.last.y!=b.y || cache.last.angle!=b.angle)b2Body_SetTransform(cache.native,position(b.x,b.y),b2MakeRot(static_cast<float>(b.angle)));
            if(cache.last.id!=b.id || cache.last.vx!=b.vx || cache.last.vy!=b.vy)b2Body_SetLinearVelocity(cache.native,{static_cast<float>(b.vx/cells_per_meter),static_cast<float>(b.vy/cells_per_meter)});
            if(cache.last.id!=b.id || cache.last.angular_velocity!=b.angular_velocity)b2Body_SetAngularVelocity(cache.native,static_cast<float>(b.angular_velocity));
            if(cache.last.id!=b.id || cache.last.sleeping!=b.sleeping)b2Body_SetAwake(cache.native,!b.sleeping);
            // Reconstruct a local hydrostatic density through the body's own
            // occupied row. Displaced water is stored outside the body, so raw
            // grid overlap alone cannot measure submersion on subsequent ticks.
            auto fluid=[&](CellPos p)->std::optional<CellPos>{
                const auto m=w.material_at(p);if(!m || *m==Material::air)return {};
                return definitions[static_cast<std::uint16_t>(*m)]->motion(w.cell_state(p)->mobilized)==Motion::liquid?std::optional{p}:std::nullopt;
            };
            const auto cosine=std::cos(b.angle),sine=std::sin(b.angle);
            for(const auto& pixel:b.pixels){
                const double x=b.x+cosine*pixel.local.x-sine*pixel.local.y,y=b.y+sine*pixel.local.x+cosine*pixel.local.y;
                const CellPos at{static_cast<int>(std::floor(x)),static_cast<int>(std::floor(y))};
                auto sample=fluid(at);
                if(!sample)for(const int direction:{-1,1}){
                    for(int distance=1;distance<=94;++distance){
                        const CellPos next{at.x+direction*distance,at.y};
                        if(!w.material_at(next))break;
                        const auto owner=w.body_pixel_at(next);
                        if(owner && w.bodies()[owner->body].id==b.id)continue;
                        sample=fluid(next);break;
                    }
                    if(sample)break;
                }
                if(!sample)continue;
                const auto m=static_cast<std::uint16_t>(*w.material_at(*sample));
                const auto point=position(x,y);
                const float displaced_mass=definitions[m]->density/(cells_per_meter*cells_per_meter);
                // Rigid hydrostatics use the established rigid gravity units.
                // Dynamic pressure and equal/opposite drag are already solved
                // by the native liquid stage; do not apply one-way drag here.
                b2Body_ApplyForce(cache.native,{0,-displaced_mass*static_cast<float>(physical::gravity)},point,true);
            }
        }
        contact_world_=&w;contact_definitions_=&definitions;
        b2World_Step(world_,static_cast<float>(seconds),4);
        contact_world_=nullptr;contact_definitions_=nullptr;
        std::vector<BodyMotion> result;result.reserve(w.bodies().size());
        for(const auto& b:w.bodies()){
            auto& cache=bodies_[b.id];const auto p=b2Body_GetPosition(cache.native),v=b2Body_GetLinearVelocity(cache.native);
            BodyMotion motion{b.id,bounds_.origin.x+static_cast<double>(p.x)*cells_per_meter,bounds_.origin.y+static_cast<double>(p.y)*cells_per_meter,b2Rot_GetAngle(b2Body_GetRotation(cache.native)),
                std::clamp(static_cast<double>(v.x)*cells_per_meter,-512.0,512.0),std::clamp(static_cast<double>(v.y)*cells_per_meter,-512.0,512.0),std::clamp(static_cast<double>(b2Body_GetAngularVelocity(cache.native)),-20.0,20.0),!b2Body_IsAwake(cache.native)};
            b2Body_SetLinearVelocity(cache.native,{static_cast<float>(motion.vx/cells_per_meter),static_cast<float>(motion.vy/cells_per_meter)});b2Body_SetAngularVelocity(cache.native,static_cast<float>(motion.angular_velocity));
            cache.last=motion;result.push_back(motion);
        }
        return result;
    }
};
}
std::unique_ptr<RigidSolver> make_box2d_solver(){return std::make_unique<BoxSolver>();}
}
