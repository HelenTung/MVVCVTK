// 测试用途：编译检查计量对齐公开头的自足性和接口类型声明。
#include "Host/MetrologyAlignmentHostFeature.h"
static_assert(sizeof(AlignmentMatrix) == 16 * sizeof(double));
