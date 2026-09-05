#pragma once

#include "sqlconnpool.h"
#include "sqlConnRAII.h"
#include <mysql/mysql.h>
#include <optional>
#include <string>
#include <vector>

// 文件元数据实体（文件系统为事实源；url/downloadUrl 由代码用 folder+name 拼接，不入库）
struct FileMeta {
  std::string folder;
  std::string name;
  long long size = 0;
  std::string mime_type;
  long long updated_at = 0;        // 文件系统 mtime（epoch 秒）
  std::string thumb_state = "none";  // none|ok|fail
  int thumb_width = 0;               // 已生成缩略图宽度（0 = 未生成）
  std::string poster_state = "none"; // none|ok|fail（视频封面）
};

/**
 * FileDao 类：纯数据访问层（files 表）
 * 只负责数据库的增删改查，不含业务逻辑。
 */
class FileDao {
 public:
  // 建表（幂等）。服务启动时调用一次，确保 files 表存在。
  static bool EnsureSchema();

  // upsert：INSERT ... ON DUPLICATE KEY UPDATE。文件 mtime 变化时重置缩略图/封面状态。
  static bool Upsert(const FileMeta& meta);

  // 删除某 folder 下不在 presentNames 中的行（文件在磁盘上已删除）。
  static bool DeleteMissing(const std::string& folder, const std::vector<std::string>& presentNames);

  // 分页查询 + 搜索 + 排序。返回总行数到 total，当前页写入 out。
  static bool QueryPage(const std::string& folder, const std::string& q,
                        const std::string& sort, const std::string& order,
                        int page, int pageSize,
                        std::vector<FileMeta>& out, long long& total);

  // 取单个文件（按 folder+name）。
  static std::optional<FileMeta> Get(const std::string& folder, const std::string& name);

  // 更新缩略图/封面状态。
  static bool SetThumbState(const std::string& folder, const std::string& name,
                            const std::string& state, int width);
  static bool SetPosterState(const std::string& folder, const std::string& name,
                             const std::string& state);

 private:
  static std::string Escape(MYSQL* sql, const std::string& src);
};
