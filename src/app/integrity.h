#pragma once

namespace md {

// 取当前进程的完整性级别 RID（0 = 查询失败）
unsigned long GetIntegrityRid();

// 若当前是 Low IL，自提升到 Medium IL；返回提升后的实际 RID
unsigned long EnsureMediumIntegrity();

// RID 对应名称（Low / Medium / High / Untrusted）
const char* IntegrityName(unsigned long rid);

} // namespace md
