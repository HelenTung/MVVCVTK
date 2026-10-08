#include "SurfaceRecipe.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace {
template<class T> void WriteValue(std::ostream &out, const std::optional<T> &value) {
    out << bool(value) << ' '; if(value) out << *value << ' ';
}
template<class T> bool ReadValue(std::istream &in, std::optional<T> &value) {
    unsigned present=0; if(!(in>>present) || present>1) return false;
    if(present) {T next{}; if(!(in>>next)) return false; value=next;} else value.reset();
    return true;
}
template<std::size_t N> void WriteArray(std::ostream &out, const std::optional<std::array<double,N>> &value) {
    out << bool(value) << ' '; if(value) for(auto item:*value) out<<item<<' ';
}
template<std::size_t N> bool ReadArray(std::istream &in, std::optional<std::array<double,N>> &value) {
    unsigned present=0; if(!(in>>present) || present>1) return false;
    if(present) {std::array<double,N> next{}; for(auto &item:next) if(!(in>>item)) return false; value=next;}
    else value.reset(); return true;
}
}
std::string SurfaceRecipeCodec::GetError(const SurfaceRecipe &p) {
    if(p.method!=SurfaceDeterminationMethod::GlobalAutomatic || unsigned(p.componentSelection)>2)
        return "Only GlobalAutomatic and a supported component selection are accepted.";
    if(p.initialIsoValue && !std::isfinite(*p.initialIsoValue)) return "Non-finite material threshold.";
    if(p.materialRange) {
        const auto range=*p.materialRange;
        if(!std::isfinite(range[0]) || !std::isfinite(range[1]) || range[0]>=range[1] ||
           !std::isfinite(range[1]-range[0]) ||
           (p.initialIsoValue && (*p.initialIsoValue<=range[0] || *p.initialIsoValue>=range[1])))
            return "Global automatic surface requires finite background < threshold < material values.";
    }
    if(!std::isfinite(p.seedFraction) || p.seedFraction<=0 || p.seedFraction>=1)
        return "Seed fraction must be inside (0,1).";
    if(!std::isfinite(p.sharpCornerAngleDeg) || p.sharpCornerAngleDeg<=0 || p.sharpCornerAngleDeg>180)
        return "Invalid sharp-corner angle.";
    if(p.componentSelection==SurfaceComponentSelection::Seeded && !p.seedModelPoint)
        return "Seeded selection needs a model point.";
    if(p.seedModelPoint && !std::all_of(p.seedModelPoint->begin(),p.seedModelPoint->end(),
                                     [](double value){return std::isfinite(value);}))
        return "Non-finite seed point.";
    return {};
}
std::string SurfaceRecipeCodec::BuildText(const SurfaceRecipe &p) {
    if(!GetError(p).empty()) return {};
    std::ostringstream out; out.imbue(std::locale::classic());
    out<<std::setprecision(std::numeric_limits<double>::max_digits10)<<"surface-recipe 4\n";
    out<<unsigned(p.method)<<' '<<unsigned(p.componentSelection)<<' ';
    WriteValue(out,p.initialIsoValue); WriteArray(out,p.materialRange); WriteArray(out,p.seedModelPoint);
    out<<p.minimumObjectVoxels<<' '<<p.seedFraction<<' '<<p.sharpCornerAngleDeg<<'\n';
    return out.str();
}
SurfaceRecipeReadResult SurfaceRecipeCodec::GetRecipe(std::string_view text) {
    if(text.size()>1024U*1024U) return {{},"Recipe text limit exceeded."};
    std::istringstream in{std::string(text)}; in.imbue(std::locale::classic());
    std::string tag; unsigned version=0,method=0,selection=0; SurfaceRecipe p;
    if(!(in>>tag>>version) || tag!="surface-recipe" || version!=4)
        return {{},"Unsupported recipe schema."};
    if(!(in>>method>>selection) || method!=unsigned(SurfaceDeterminationMethod::GlobalAutomatic) || selection>2 ||
       !ReadValue(in,p.initialIsoValue) || !ReadArray(in,p.materialRange) || !ReadArray(in,p.seedModelPoint) ||
       !(in>>p.minimumObjectVoxels>>p.seedFraction>>p.sharpCornerAngleDeg)) return {{},"Malformed recipe."};
    p.componentSelection=static_cast<SurfaceComponentSelection>(selection);
    in>>std::ws; if(!in.eof()) return {{},"Trailing recipe data."};
    if(auto error=GetError(p);!error.empty()) return {{},error};
    return {p,{}};
}
