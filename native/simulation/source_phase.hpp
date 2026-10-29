#pragma once
#include "content/content_registry.hpp"
#include "simulation/combustion.hpp"
#include <cmath>

namespace pixel {
inline bool source_phase_gate(CombustionRandom& random,int reverse,int strength){
    return strength>0 && static_cast<int>(random.next()%static_cast<unsigned>(reverse+1))<strength;
}
struct SourcePhaseCandidates {bool freeze=false,melt=false;};
inline SourcePhaseCandidates source_phase_candidates(const MaterialDefinition& d,int cold,int warm,bool blocker,CombustionRandom& random){
    SourcePhaseCandidates result;
    if(cold>0 && !d.cold_output.empty())result.freeze=source_phase_gate(random,d.cold_chance_rev,cold) && !blocker;
    if(warm>0 && !d.warmth_output.empty())result.melt=source_phase_gate(random,d.warmth_chance_rev,warm);
    return result;
}
inline int source_gas_initial_counter(int lifetime,CombustionRandom& random){
    if(lifetime<=0)return 0;
    const int spread=std::max(1,static_cast<int>(static_cast<float>(lifetime)*0.3f));
    return static_cast<int>(random.next()%static_cast<unsigned>(2*spread+1))-spread;
}
}
