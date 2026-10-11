// 连续灰度的解析控制：内部孔隙、切触、边界截距及缺失支持。
#include "ThicknessMaterialField.h"
#include <iostream>

int GetMaterialFieldTestFailures()
{
    using namespace ThicknessMaterialField;
    int failures = 0;
    const auto check = [&](bool ok, const char *message) {
        if (!ok)
        {
            ++failures;
            std::cerr << "FAILED: " << message << '\n';
        }
    };
    Field field;
    field.extent = {0, 1, 0, 1, 0, 1};
    field.node = [](const Index &, double &v) {
        v = 1;
        return true;
    };
    const auto full = field.GetMaterialPath({0, .3, .4}, {1, 0, 0}, 1, 1e-10, 0);
    check(full.status == Status::Valid && full.trim[0] == 0 && full.trim[1] == 0,
          "constant material has one full interval");
    check(field.GetMaterialPath({.25,.5,.5},{.5,0,0},1,.3,0).status==Status::Unresolved,
          "positive certificate cannot bypass the four-epsilon short-path gate");
    check(field.GetMaterialPath({.25,.5,.5},{.5,0,0},1,1e-8,-1).status==Status::Unresolved
          && field.GetMaterialPath({.25,.5,.5},{.5,0,0},1,1e-8,0,0).status==Status::Unresolved,
          "positive certificate preserves invalid trim and tolerance gates");
    field.extent={0,10002,0,1,0,1};
    check(field.GetMaterialPath({0,.3,.4},{10002,0,0},10002,1,0).status==Status::Unresolved,
          "positive certificate cannot bypass the original long-path cut budget");
    field.extent={0,1,0,1,0,1};
    field.node = [](const Index &i, double &v) {
        v = double(i[0]) - .25;
        return true;
    };
    check(field.GetMaterialAt({.25, .3, .4}) == -1 && field.GetMaterialAt({.3, .3, .4}) == 1 &&
              field.GetMaterialAt({.2, .3, .4}) == 0,
          "exact boundary is not assigned a material side");
    Result positive;
    check(field.GetPositivePath({.3,.3,.4},{.5,0,0},.5,1e-8,positive)
          && positive.status==Status::Valid && positive.trim==std::array<double,2>{0,0},
          "mixed corners use a strict positive Bernstein certificate without exact conversions");
    field.extent={0,4,0,1,0,1};
    field.node=[](const Index& i,double& v) {v=std::abs(double(i[0])-2.);return true;};
    Result touching;
    check(!field.GetPositivePath({.5,.3,.4},{3,0,0},3,1e-8,touching)
          && field.GetMaterialPath({.5,.3,.4},{3,0,0},3,1e-8,0).status!=Status::Valid,
          "zero contact at a cell face cannot receive a positive certificate");
    field.extent={0,1,0,1,0,1};
    field.node=[](const Index& i,double& v) {v=double(i[0])-.25;return true;};
    const auto trimmed = field.GetMaterialPath({0, .3, .4}, {1, 0, 0}, 1, 1e-10, .25000001);
    check(trimmed.status == Status::Valid && std::abs(trimmed.trim[0] - .25) < 2e-9,
          "endpoint trim is bounded by the continuous crossing");
    check(field.GetMaterialPath({0, .3, .4}, {1, 0, 0}, 1, 1e-10, .24999999).status == Status::Invalid,
          "endpoint trim beyond the declared allowance is rejected");
    field.node = [](const Index &i, double &v) {
        const std::array<double, 4> bernstein{-.08, .14, -.14, .08};
        v = bernstein[std::size_t(i[0] + i[1] + i[2])];
        return true;
    };
    check(field.GetMaterialPath({0, 0, 0}, {1, 1, 1}, std::sqrt(3.), 1e-10, 1).status == Status::Invalid,
          "cubic diagonal with three crossings cannot skip its internal air interval");
    field.node = [](const Index &i, double &v) {
        v = (double(i[0]) - .5) * (double(i[1]) - .5);
        return true;
    };
    check(field.GetMaterialPath({0, 0, .5}, {1, 1, 0}, std::sqrt(2.), 1e-10, 0).status != Status::Valid,
          "exact tangent contact is unresolved rather than silently certified");
    field.node = [](const Index &, double &v) {
        v = 0;
        return true;
    };
    check(field.GetMaterialPath({0, .3, .4}, {1, 0, 0}, 1, 1e-10, 1).status == Status::Unresolved,
          "a whole interval on the implicit surface has no fabricated material");
    field.node = [](const Index &i, double &v) {
        v = 1;
        return i != Index{1, 1, 1};
    };
    check(field.GetMaterialPath({0, .3, .4}, {1, 0, 0}, 1, 1e-10, 0).status == Status::Missing,
          "a missing trilinear corner is missing support");
    check(field.GetMaterialPath({-.1, .3, .4}, {1, 0, 0}, 1, 1e-10, 1).status == Status::Missing,
          "endpoint outside source cannot be excused by trim allowance");
    field.check = [] { throw std::runtime_error("cancelled"); };
    bool cancelled = false;
    try
    {
        field.GetMaterialPath({0, .3, .4}, {1, 0, 0}, 1, 1e-10, 0);
    }
    catch (const std::runtime_error &)
    {
        cancelled = true;
    }
    check(cancelled, "cancellation escapes the numeric unresolved classification");
    return failures;
}
