// sinter 诊断实现：单独成 TU，避免 Diagnostics 的方法被到处内联
#include "support/diagnostics.hpp"

namespace sinter {
    // 目前 Diagnostics 全部为轻量内联实现；本 TU 保留给后续诊断
    // （源码位置、错误恢复、彩色输出）扩展，保证 ABI 稳定点存在。
    void diagnostics_anchor() {}
} // namespace sinter