#include "persistence/world_file.hpp"
#include <chrono>
#include <bit>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace pixel::persistence {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_file_size = 16 * 1024 * 1024;
double elapsed(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
template<class T> void put(Bytes& bytes, T value) {
    using U = std::make_unsigned_t<T>;
    auto bits = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
}
std::uint64_t checksum(const Bytes& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
    return hash;
}
struct Reader {
    const Bytes& bytes;
    std::size_t position = 0;
    template<class T> T get() {
        if (sizeof(T) > bytes.size() - position) throw std::runtime_error("Truncated save field.");
        std::uint64_t bits = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) bits |= static_cast<std::uint64_t>(bytes[position++]) << (8 * i);
        if constexpr (std::is_signed_v<T>) {
            return std::bit_cast<T>(static_cast<std::make_unsigned_t<T>>(bits));
        } else return static_cast<T>(bits);
    }
    std::string text() {
        const auto size = get<std::uint32_t>();
        if (size == 0 || size > 128 || size > bytes.size() - position) throw std::runtime_error("Invalid material ID length in save.");
        std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(position), bytes.begin() + static_cast<std::ptrdiff_t>(position + size));
        position += size;
        return value;
    }
};
void put_state(Bytes& body,const CellState& state){
    put(body,state.temperature);put(body,state.life);put(body,state.fuel);put<std::uint8_t>(body,state.burning?1:0);
    put(body,state.created_tick);put(body,state.velocity_x);put(body,state.velocity_y);put(body,state.vertical_fraction);put(body,state.horizontal_fraction);
    put<std::uint8_t>(body,state.mobilized?1:0);put(body,state.quiet_ticks);put(body,state.adhesion_ticks);put(body,state.adhesion_cooldown);put(body,state.detonation_ticks);put(body,state.electrified_ticks);
    put(body,state.liquid_fraction_x);put(body,state.liquid_fraction_y);
    put(body,state.momentum_remainder_x);put(body,state.momentum_remainder_y);
    put(body,state.damage_taken);put(body,state.gas_counter);put(body,state.electric_stamp);
    put(body,state.fire_intensity);put(body,state.burning_ticks);put(body,state.fire_color);put<std::uint8_t>(body,state.ignition_pending?1:0);
    put(body,state.source_attachment);
    put(body,std::bit_cast<std::uint32_t>(state.liquid_fall));
    put(body,std::bit_cast<std::uint32_t>(state.liquid_lateral));put(body,state.liquid_direction);
}
CellState get_state(Reader& r,std::uint32_t version){
    CellState state;state.temperature=r.get<std::int32_t>();state.life=r.get<std::uint16_t>();state.fuel=version>=7?r.get<std::uint32_t>():r.get<std::uint16_t>();
    const auto burning=r.get<std::uint8_t>();if(burning>1)throw std::runtime_error("Invalid burning flag.");state.burning=burning!=0;
    if(version>=3){state.created_tick=r.get<std::uint64_t>();state.velocity_x=r.get<std::int32_t>();state.velocity_y=r.get<std::int32_t>();state.vertical_fraction=r.get<std::uint16_t>();state.horizontal_fraction=r.get<std::uint16_t>();}
    if(version>=4){const auto mobilized=r.get<std::uint8_t>();if(mobilized>1)throw std::runtime_error("Invalid mobilized flag.");state.mobilized=mobilized!=0;state.quiet_ticks=r.get<std::uint16_t>();state.adhesion_ticks=r.get<std::uint16_t>();state.adhesion_cooldown=r.get<std::uint16_t>();}
    if(version>=5)state.detonation_ticks=r.get<std::uint16_t>();if(version>=6)state.electrified_ticks=r.get<std::uint16_t>();
    if(version>=10){state.liquid_fraction_x=r.get<std::uint32_t>();state.liquid_fraction_y=r.get<std::uint32_t>();}
    if(version>=11){state.momentum_remainder_x=r.get<std::int16_t>();state.momentum_remainder_y=r.get<std::int16_t>();}
    if(version>=13){state.damage_taken=r.get<std::uint32_t>();state.gas_counter=r.get<std::int32_t>();state.electric_stamp=r.get<std::uint64_t>();}
    if(version>=12){state.fire_intensity=r.get<std::uint8_t>();state.burning_ticks=r.get<std::uint16_t>();state.fire_color=r.get<std::uint32_t>();const auto pending=r.get<std::uint8_t>();if(pending>1)throw std::runtime_error("Invalid pending ignition flag.");state.ignition_pending=pending!=0;}
    if(version>=14)state.source_attachment=r.get<std::uint8_t>();
    if(version>=15){state.liquid_fall=std::bit_cast<float>(r.get<std::uint32_t>());state.liquid_lateral=std::bit_cast<float>(r.get<std::uint32_t>());state.liquid_direction=r.get<std::uint8_t>();}
    return state;
}
Bytes encode(const WorldSnapshot& s) {
    const auto error = validate_snapshot(s);
    if (!error.empty()) throw std::runtime_error(error);
    Bytes body;
    put(body, s.seed); put(body, s.tick);
    put<std::int32_t>(body, s.bounds.origin.x); put<std::int32_t>(body, s.bounds.origin.y);
    put<std::uint32_t>(body, s.bounds.width); put<std::uint32_t>(body, s.bounds.height);
    put(body, s.chunk_edge); put(body, s.building_edge); put(body, s.initial_stone);
    put<std::uint32_t>(body, static_cast<std::uint32_t>(s.material_ids.size()));
    for (const auto& id : s.material_ids) {
        put<std::uint32_t>(body, static_cast<std::uint32_t>(id.size()));
        body.insert(body.end(), id.begin(), id.end());
    }
    put<std::uint32_t>(body, static_cast<std::uint32_t>(s.cells.size()));
    for (auto material : s.cells) put(body, material);
    put<std::uint32_t>(body, static_cast<std::uint32_t>(s.drops.size()));
    for (auto drop : s.drops) { put<std::int32_t>(body, drop.position.x); put<std::int32_t>(body, drop.position.y); put(body, drop.amount); }
    put(body, s.held_stone);
    put<std::uint8_t>(body,s.material_lab ? 1 : 0);
    put<std::uint32_t>(body,save_format_version-1); // Explicit simulation-state revision.
    for (const auto& state:s.states)put_state(body,state);
    put<std::uint32_t>(body,static_cast<std::uint32_t>(s.electric_pulses.size()));
    for(const auto& p:s.electric_pulses){
        put<std::int32_t>(body,p.position.x);put<std::int32_t>(body,p.position.y);put<std::int32_t>(body,p.previous.x);put<std::int32_t>(body,p.previous.y);
        put(body,p.energy);put(body,p.speed);put(body,p.heat_interval);put(body,p.heat);put(body,p.glow_ticks);
        put(body,p.body);put(body,p.previous_body);
        put<std::uint8_t>(body,p.source_rules?1:0);put(body,std::bit_cast<std::uint32_t>(p.avg_x));put(body,std::bit_cast<std::uint32_t>(p.avg_y));
        put(body,p.splittings_left);put(body,p.splitting_energy);put(body,std::bit_cast<std::uint32_t>(p.probability_to_heat));put<std::uint8_t>(body,p.set_fire?1:0);
    }
    put(body,s.next_body_id);put<std::uint32_t>(body,static_cast<std::uint32_t>(s.bodies.size()));
    for(const auto& b:s.bodies){
        put(body,b.id);put(body,b.shape_revision);
        for(const auto value:{b.x,b.y,b.angle,b.vx,b.vy,b.angular_velocity})put(body,std::bit_cast<std::uint64_t>(value));
        put<std::uint8_t>(body,b.sleeping?1:0);put<std::uint32_t>(body,static_cast<std::uint32_t>(b.pixels.size()));
        for(const auto& p:b.pixels){put<std::int16_t>(body,static_cast<std::int16_t>(p.local.x));put<std::int16_t>(body,static_cast<std::int16_t>(p.local.y));put(body,p.material);put_state(body,p.state);}
    }
    put(body,s.freeze_chance);put(body,s.unfreeze_chance);
    Bytes file{'P','S','B','W','A','L','L',0};
    put(file, s.version); put<std::uint64_t>(file, body.size()); put(file, checksum(body));
    file.insert(file.end(), body.begin(), body.end());
    if(file.size()>max_file_size)throw std::runtime_error("Save file exceeds 16 MiB limit; previous save was not replaced.");
    return file;
}
WorldSnapshot decode(const Bytes& bytes) {
    if (bytes.size() < 28 || std::string(bytes.begin(), bytes.begin() + 8) != std::string("PSBWALL\0", 8)) throw std::runtime_error("Corrupt save: invalid magic/header.");
    Reader header{bytes, 8};
    WorldSnapshot s;
    s.version = header.get<std::uint32_t>();
    const auto version=s.version;
    if (version < 1 || version > save_format_version) throw std::runtime_error("Unknown save format version: " + std::to_string(s.version));
    const auto size = header.get<std::uint64_t>();
    const auto expected_checksum = header.get<std::uint64_t>();
    if (size != bytes.size() - 28) throw std::runtime_error("Corrupt save: payload length mismatch.");
    Bytes body(bytes.begin() + 28, bytes.end());
    if (checksum(body) != expected_checksum) throw std::runtime_error("Corrupt save: checksum mismatch.");
    Reader r{body};
    s.seed = r.get<std::uint64_t>(); s.tick = r.get<std::uint64_t>();
    s.bounds.origin = {r.get<std::int32_t>(), r.get<std::int32_t>()};
    const auto width = r.get<std::uint32_t>(), height = r.get<std::uint32_t>();
    if (width == 0 || height == 0 || width > 512 || height > 512) throw std::runtime_error("Invalid saved world dimensions.");
    s.bounds.width = static_cast<int>(width); s.bounds.height = static_cast<int>(height);
    s.chunk_edge = r.get<std::uint32_t>(); s.building_edge = r.get<std::uint32_t>(); s.initial_stone = r.get<std::uint64_t>();
    const auto mapping_size = r.get<std::uint32_t>();
    if (mapping_size < 1 || mapping_size > 65534 || (version==1 && mapping_size!=1)) throw std::runtime_error("Invalid saved material mapping size.");
    s.material_ids.clear();
    for (std::uint32_t i = 0; i < mapping_size; ++i) s.material_ids.push_back(r.text());
    const auto cells = r.get<std::uint32_t>();
    if (cells != width * height) throw std::runtime_error("Saved material array does not match dimensions.");
    s.cells.reserve(cells);
    for (std::uint32_t i = 0; i < cells; ++i) s.cells.push_back(r.get<std::uint16_t>());
    const auto drops = r.get<std::uint32_t>();
    if (drops > 100) throw std::runtime_error("Too many saved drops.");
    for (std::uint32_t i = 0; i < drops; ++i) {
        const CellPos p{r.get<std::int32_t>(), r.get<std::int32_t>()};
        s.drops.push_back({p, r.get<std::uint64_t>()});
    }
    s.held_stone = r.get<std::uint64_t>();
    s.states.resize(cells);
    if (version>=2) {
        const auto lab=r.get<std::uint8_t>();
        if (lab>1) throw std::runtime_error("Invalid laboratory flag.");
        s.material_lab=lab!=0;
        if(r.get<std::uint32_t>()!=version-1) throw std::runtime_error("Unknown simulation rule revision.");
        for(auto& state:s.states)state=get_state(r,version);
    }
    if(version>=6){
        const auto pulses=r.get<std::uint32_t>();if(pulses>cells+(version>=9?65536u:0u))throw std::runtime_error("Too many electric pulses.");
        s.electric_pulses.reserve(pulses);
        for(std::uint32_t i=0;i<pulses;++i){
            ElectricPulse p;p.position={r.get<std::int32_t>(),r.get<std::int32_t>()};p.previous={r.get<std::int32_t>(),r.get<std::int32_t>()};
            p.energy=r.get<std::uint32_t>();p.speed=r.get<std::uint16_t>();p.heat_interval=r.get<std::uint16_t>();p.heat=r.get<std::uint16_t>();p.glow_ticks=r.get<std::uint16_t>();if(version>=9){p.body=r.get<std::uint32_t>();p.previous_body=r.get<std::uint32_t>();}
            if(version>=13){const auto source=r.get<std::uint8_t>();if(source>1)throw std::runtime_error("Invalid source electricity flag.");p.source_rules=source!=0;
                p.avg_x=std::bit_cast<float>(r.get<std::uint32_t>());p.avg_y=std::bit_cast<float>(r.get<std::uint32_t>());p.splittings_left=r.get<std::uint16_t>();p.splitting_energy=r.get<std::uint32_t>();p.probability_to_heat=std::bit_cast<float>(r.get<std::uint32_t>());
                const auto fire=r.get<std::uint8_t>();if(fire>1)throw std::runtime_error("Invalid direct electrical fire flag.");p.set_fire=fire!=0;}
            s.electric_pulses.push_back(p);
        }
    }
    if(version>=8){
        s.next_body_id=r.get<std::uint32_t>();const auto count=r.get<std::uint32_t>();if(count>64)throw std::runtime_error("Too many saved rigid bodies.");
        for(std::uint32_t i=0;i<count;++i){
            RigidBodyState b;b.id=r.get<std::uint32_t>();b.shape_revision=r.get<std::uint64_t>();
            b.x=std::bit_cast<double>(r.get<std::uint64_t>());b.y=std::bit_cast<double>(r.get<std::uint64_t>());b.angle=std::bit_cast<double>(r.get<std::uint64_t>());
            b.vx=std::bit_cast<double>(r.get<std::uint64_t>());b.vy=std::bit_cast<double>(r.get<std::uint64_t>());b.angular_velocity=std::bit_cast<double>(r.get<std::uint64_t>());
            const auto sleeping=r.get<std::uint8_t>();if(sleeping>1)throw std::runtime_error("Invalid rigid sleeping flag.");b.sleeping=sleeping!=0;
            const auto pixels=r.get<std::uint32_t>();if(!pixels || pixels>1024)throw std::runtime_error("Invalid rigid pixel count.");
            for(std::uint32_t j=0;j<pixels;++j){RigidPixel p;p.local={r.get<std::int16_t>(),r.get<std::int16_t>()};p.material=r.get<std::uint16_t>();p.state=get_state(r,version);b.pixels.push_back(p);}
            s.bodies.push_back(std::move(b));
        }
    }
    if(version>=13){s.freeze_chance=r.get<std::uint8_t>();s.unfreeze_chance=r.get<std::uint8_t>();}
    s.version=save_format_version; // Older saves have no rigid bodies; existing material fields are retained.
    if (r.position != body.size()) throw std::runtime_error("Unexpected trailing save data.");
    const auto error = validate_snapshot(s);
    if (!error.empty()) throw std::runtime_error(error);
    return s;
}
} // namespace

FileResult save_world_file(const std::filesystem::path& path, const WorldSnapshot& snapshot) {
    const auto start = Clock::now();
    auto temp = path; temp += ".tmp";
    bool temp_created = false;
    try {
        const auto bytes = encode(snapshot);
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("Cannot open temporary save file: " + temp.string());
        temp_created = true;
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) throw std::runtime_error("Failed writing/flushing temporary save file.");
        stream.close();
        if (stream.fail()) throw std::runtime_error("Failed closing temporary save file.");
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace save file; Windows error " + std::to_string(GetLastError()));
#else
        std::filesystem::rename(temp, path);
#endif
        return {true, {}, elapsed(start)};
    } catch (const std::exception& error) {
        if (temp_created) { std::error_code ignored; std::filesystem::remove(temp, ignored); }
        return {false, error.what(), elapsed(start)};
    }
}
LoadResult load_world_file(const std::filesystem::path& path) {
    const auto start = Clock::now();
    try {
        const auto size = std::filesystem::file_size(path);
        if (size > max_file_size) throw std::runtime_error("Save file exceeds 16 MiB limit.");
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot open save file: " + path.string());
        Bytes bytes(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!stream || stream.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Incomplete or changing save file.");
        return {decode(bytes), {}, elapsed(start)};
    } catch (const std::exception& error) { return {std::nullopt, error.what(), elapsed(start)}; }
}
} // namespace pixel::persistence
