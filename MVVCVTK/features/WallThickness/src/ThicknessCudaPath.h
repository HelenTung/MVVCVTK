#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace ThicknessCudaPath {
enum class ScalarType {Unknown,Int8,UInt8,Int16,UInt16,Int32,UInt32,Float32,Float64};
struct Measurement {
    std::array<double,3> source{};
};
struct Value {double thickness=0;bool valid=false;};
struct Params {
    std::array<double,3> spacing{};
    std::array<double,9> direction{};
    double epsilon=0,maxDistance=0,maxBoundaryError=0,tolerance=1e-9;
    std::uint32_t directionCount=9;
    double coneCos=1,coneSin=0;
    std::array<double,17> phiCos{},phiSin{};
    bool sourceLocal=true;
};
struct Input {
    std::array<int,6> extent{};
    std::array<std::size_t,3> blocks{};
    double threshold=0;
    ScalarType scalarType=ScalarType::Unknown;
    const void* values=nullptr;std::size_t valueBytes=0;
    const std::uint8_t* mask=nullptr;std::size_t maskBytes=0;
    const std::int8_t* signs=nullptr;std::size_t signCount=0;
    const std::uint32_t* offsets=nullptr;std::size_t offsetCount=0;
    const void* children=nullptr;std::size_t childBytes=0;
};
class Client {
public:
    virtual ~Client()=default;
    virtual std::vector<Value> Measure(const std::vector<Measurement>& requests,const Params& params,const std::function<void()>& check)=0;
};
std::unique_ptr<Client> Create(const Input& input) noexcept;
}
