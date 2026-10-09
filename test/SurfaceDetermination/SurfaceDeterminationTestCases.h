// 测试用途：声明表面确定各类回归的失败计数入口。
#pragma once
#include <cstddef>
int GetSurfaceSeedWindowFailCount();
int GetSurfaceOptimizationFailCount();

int GetSurfaceRealInputFailCount(const char* path,int nx,int ny,int nz,double iso,
                                std::size_t expectedPoints,std::size_t expectedTriangles);

int GetSurfaceAlgorithmFailCount();
int GetSurfaceGeometryFailCount();
int GetSurfaceLifecycleFailCount();
int GetSurfaceDisplayFailCount();

int GetSurfaceContractFailCount();

int GetSurfaceBusinessFailCount();
