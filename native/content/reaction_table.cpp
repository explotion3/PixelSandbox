#include "content/reaction_table.hpp"
#include <algorithm>
#include <charconv>
#include <utility>
#include <cmath>
#include <unordered_set>

namespace pixel {
namespace {
std::string_view trim(std::string_view s) {
    const auto first=s.find_first_not_of(" \t\r");
    if(first==s.npos)return {};
    return s.substr(first,s.find_last_not_of(" \t\r")-first+1);
}
bool integer(std::string_view text,int& output) {
    const auto result=std::from_chars(text.data(),text.data()+text.size(),output);
    return result.ec==std::errc{} && result.ptr==text.data()+text.size();
}
}
ReactionTableResult parse_reaction_table(std::string_view text) {
    ReactionTableResult result;
    if(text.starts_with("\xef\xbb\xbf"))text.remove_prefix(3);
    std::size_t line_number=0;
    while(!text.empty()) {
        ++line_number;
        const auto end=text.find('\n');
        const auto line=trim(text.substr(0,end));
        text=end==text.npos?std::string_view{}:text.substr(end+1);
        if(line.empty() || line.front()=='#')continue;
        auto fail=[&](std::string reason){return ReactionTableResult{{},"Reaction table line "+std::to_string(line_number)+": "+reason};};
        std::vector<std::string_view> fields;
        auto rest=line;
        for(;;) {
            const auto split=rest.find(';');
            fields.push_back(trim(rest.substr(0,split)));
            if(fields.back().empty())return fail("empty field.");
            if(split==rest.npos)break;
            rest.remove_prefix(split+1);
        }
        const auto named=std::find_if(fields.begin(),fields.end(),[](auto field){return field.find('=')!=field.npos;});
        const auto positional=static_cast<std::size_t>(named-fields.begin());
        if(positional<5 || positional>10 || positional==9)return fail("expected 5..8 or 10 positional fields before named options.");
        int numbers[]{1,0,-1000,10000};
        for(std::size_t i=4;i<std::min<std::size_t>(8,positional);++i)
            if(!integer(fields[i],numbers[i-4]))return fail("invalid or overflowing integer.");
        if(numbers[0]<1 || numbers[0]>10000 || numbers[1]<0 || numbers[1]>60000 ||
           numbers[2]<-1000 || numbers[3]>10000 || numbers[2]>numbers[3])return fail("numeric condition out of range.");
        ReactionDefinition r{std::string(fields[0]),std::string(fields[1]),std::string(fields[2]),std::string(fields[3]),static_cast<std::uint32_t>(numbers[0])};
        r.minimum_age=static_cast<std::uint32_t>(numbers[1]);r.minimum_temperature=numbers[2];r.maximum_temperature=numbers[3];
        if(positional==10){r.input_c=fields[8];r.output_c=fields[9];}
        std::unordered_set<std::string> seen;
        for(auto option=named;option!=fields.end();++option){
            const auto equals=option->find('=');
            if(equals==option->npos)return fail("positional field after named option.");
            const auto key=trim(option->substr(0,equals)),value=trim(option->substr(equals+1));
            if(!seen.insert(std::string(key)).second)return fail("duplicate named option: "+std::string(key));
            if(key=="probability"){
                float probability=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),probability);
                if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !std::isfinite(probability) || probability<0 || probability>100)return fail("probability must be finite in 0..100.");
                r.probability_gate=static_cast<int>(probability*100.0f);continue;
            }
            if(key=="direction"){
                if(value=="any")r.direction=-1;else if(value=="top")r.direction=0;else if(value=="bottom")r.direction=1;
                else if(value=="left")r.direction=2;else if(value=="right")r.direction=3;else return fail("invalid direction.");continue;
            }
            int number=0;if(!integer(value,number))return fail("invalid or overflowing option integer.");
            if(key=="blob_radius_a" || key=="blob_radius_b"){
                if(number<0 || number>64)return fail("blob radius must be in 0..64.");
                (key=="blob_radius_a"?r.blob_radius_a:r.blob_radius_b)=number;continue;
            }
            if(number<0 || number>1)return fail("reaction flag must be 0 or 1.");
            if(key=="fast")r.fast=number!=0;else if(key=="required")r.required_neighbor=number!=0;
            else if(key=="destroy_lonely")r.destroy_lonely=number!=0;else if(key=="restrict_a")r.restrict_a=number!=0;
            else if(key=="restrict_b")r.restrict_b=number!=0;else return fail("unknown named option: "+std::string(key));
        }
        if(r.probability_gate<0 && (seen.contains("fast") || seen.contains("required") || seen.contains("direction") || seen.contains("destroy_lonely") || seen.contains("restrict_a") || seen.contains("restrict_b")))return fail("source reaction options require probability.");
        result.reactions.push_back(std::move(r));
    }
    return result;
}
}
