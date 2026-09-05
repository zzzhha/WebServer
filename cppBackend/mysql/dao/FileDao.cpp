#include "FileDao.h"
#include <cstring>
#include <sstream>

// 转义一个字符串供 SQL 字面量使用。返回需要手动释放的堆缓冲，或空串表示失败。
std::string FileDao::Escape(MYSQL* sql, const std::string& src) {
  if (src.size() > (SIZE_MAX - 1) / 2) return "";
  std::vector<char> buf(2 * src.size() + 1);
  unsigned long len = mysql_real_escape_string(sql, buf.data(), src.c_str(), src.size());
  if (len > buf.size()) return "";
  return std::string(buf.data(), len);
}

bool FileDao::EnsureSchema() {
  MYSQL* sql = nullptr;
  SqlConnRAII raii(&sql, SqlConnPool::Instance());
  if (!sql) {
    LOGERROR("FileDao::EnsureSchema failed: get connection from pool failed");
    return false;
  }
  const char* ddl =
      "CREATE TABLE IF NOT EXISTS files ("
      "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
      "folder VARCHAR(32) NOT NULL,"
      "name VARCHAR(255) NOT NULL,"
      "size BIGINT NOT NULL DEFAULT 0,"
      "mime_type VARCHAR(64),"
      "updated_at BIGINT NOT NULL,"
      "thumb_state ENUM('none','ok','fail') NOT NULL DEFAULT 'none',"
      "thumb_width INT NOT NULL DEFAULT 0,"
      "poster_state ENUM('none','ok','fail') NOT NULL DEFAULT 'none',"
      "created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
      "updated_row_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
      "UNIQUE KEY uk_folder_name (folder, name),"
      "KEY idx_folder_mtime (folder, updated_at DESC),"
      "KEY idx_folder_name_sort (folder, name),"
      "KEY idx_name (name)"
      ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";
  if (mysql_query(sql, ddl) != 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), "FileDao::EnsureSchema failed: %s", mysql_error(sql));
    LOGERROR(buf);
    return false;
  }
  LOGINFO("FileDao::EnsureSchema ok");
  return true;
}

bool FileDao::Upsert(const FileMeta& meta) {
  MYSQL* sql = nullptr;
  SqlConnRAII raii(&sql, SqlConnPool::Instance());
  if (!sql) {
    LOGERROR("FileDao::Upsert failed: get connection from pool failed");
    return false;
  }
  std::string esc_folder = Escape(sql, meta.folder);
  std::string esc_name = Escape(sql, meta.name);
  std::string esc_mime = Escape(sql, meta.mime_type);
  if (esc_folder.empty() || esc_name.empty()) {
    LOGERROR("FileDao::Upsert: escaping failed");
    return false;
  }
  // 文件 mtime 变化时重置缩略图/封面状态（下次请求重新生成）。
  std::string sql_stmt =
      "INSERT INTO files(folder, name, size, mime_type, updated_at, thumb_state, thumb_width, poster_state) VALUES('"
      + esc_folder + "','" + esc_name + "'," + std::to_string(meta.size) + ",'" + esc_mime + "',"
      + std::to_string(meta.updated_at) + ",'" + meta.thumb_state + "'," + std::to_string(meta.thumb_width)
      + ",'" + meta.poster_state + "')"
      " ON DUPLICATE KEY UPDATE size=VALUES(size), mime_type=VALUES(mime_type),"
      " updated_at=VALUES(updated_at),"
      " thumb_state=IF(VALUES(updated_at)=updated_at, thumb_state, 'none'),"
      " thumb_width=IF(VALUES(updated_at)=updated_at, thumb_width, 0),"
      " poster_state=IF(VALUES(updated_at)=updated_at, poster_state, 'none')";
  if (mysql_query(sql, sql_stmt.c_str()) != 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), "FileDao::Upsert failed: %s", mysql_error(sql));
    LOGERROR(buf);
    return false;
  }
  return true;
}

bool FileDao::DeleteMissing(const std::string& folder, const std::vector<std::string>& presentNames) {
  MYSQL* sql = nullptr;
  SqlConnRAII raii(&sql, SqlConnPool::Instance());
  if (!sql) {
    LOGERROR("FileDao::DeleteMissing failed: get connection from pool failed");
    return false;
  }
  std::string esc_folder = Escape(sql, folder);
  if (esc_folder.empty()) return false;

  if (presentNames.empty()) {
    std::string del = "DELETE FROM files WHERE folder='" + esc_folder + "'";
    if (mysql_query(sql, del.c_str()) != 0) {
      LOGERROR(std::string("FileDao::DeleteMissing(empty) failed: ") + mysql_error(sql));
      return false;
    }
    return true;
  }

  std::ostringstream in;
  in << "(";
  for (size_t i = 0; i < presentNames.size(); ++i) {
    if (i) in << ",";
    in << "'" << Escape(sql, presentNames[i]) << "'";
  }
  in << ")";
  std::string del = "DELETE FROM files WHERE folder='" + esc_folder + "' AND name NOT IN " + in.str();
  if (mysql_query(sql, del.c_str()) != 0) {
    LOGERROR(std::string("FileDao::DeleteMissing failed: ") + mysql_error(sql));
    return false;
  }
  return true;
}

// 构造 LIKE 模式：转义 % _ \ 并包裹 %...%
static std::string BuildLikePattern(const std::string& q) {
  std::string out;
  out.reserve(q.size() + 2);
  for (char c : q) {
    if (c == '\\' || c == '%' || c == '_') out.push_back('\\');
    out.push_back(c);
  }
  return "%" + out + "%";
}

bool FileDao::QueryPage(const std::string& folder, const std::string& q,
                        const std::string& sort, const std::string& order,
                        int page, int pageSize,
                        std::vector<FileMeta>& out, long long& total) {
  MYSQL* sql = nullptr;
  SqlConnRAII raii(&sql, SqlConnPool::Instance());
  if (!sql) {
    LOGERROR("FileDao::QueryPage failed: get connection from pool failed");
    return false;
  }
  out.clear();
  total = 0;

  std::string esc_folder = Escape(sql, folder);
  if (esc_folder.empty()) return false;

  std::string where = "WHERE folder='" + esc_folder + "'";
  if (!q.empty()) {
    where += " AND name LIKE '" + Escape(sql, BuildLikePattern(q)) + "'";
  }

  // 排序白名单：防止 SQL 注入。
  std::string col;
  if (sort == "name") col = "name";
  else if (sort == "size") col = "size";
  else col = "updated_at";
  std::string dir = (order == "asc") ? "ASC" : "DESC";

  // 总数
  std::string count_sql = "SELECT COUNT(*) FROM files " + where;
  if (mysql_query(sql, count_sql.c_str()) != 0) {
    LOGERROR(std::string("FileDao::QueryPage count failed: ") + mysql_error(sql));
    return false;
  }
  MYSQL_RES* c_res = mysql_store_result(sql);
  if (!c_res) {
    LOGERROR(std::string("FileDao::QueryPage count store failed: ") + mysql_error(sql));
    return false;
  }
  MYSQL_ROW crow = mysql_fetch_row(c_res);
  if (crow && crow[0]) total = std::atoll(crow[0]);
  mysql_free_result(c_res);

  int p = page < 1 ? 1 : page;
  int ps = pageSize < 1 ? 1 : (pageSize > 200 ? 200 : pageSize);
  long long offset = (long long)(p - 1) * ps;

  std::string query_sql = "SELECT folder,name,size,mime_type,updated_at,thumb_state,thumb_width,poster_state FROM files "
                          + where + " ORDER BY " + col + " " + dir
                          + ", name ASC LIMIT " + std::to_string(offset) + "," + std::to_string(ps);
  if (mysql_query(sql, query_sql.c_str()) != 0) {
    LOGERROR(std::string("FileDao::QueryPage select failed: ") + mysql_error(sql));
    return false;
  }
  MYSQL_RES* res = mysql_store_result(sql);
  if (!res) {
    LOGERROR(std::string("FileDao::QueryPage store failed: ") + mysql_error(sql));
    return false;
  }
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    FileMeta m;
    if (row[0]) m.folder = row[0];
    if (row[1]) m.name = row[1];
    if (row[2]) m.size = std::atoll(row[2]);
    if (row[3]) m.mime_type = row[3];
    if (row[4]) m.updated_at = std::atoll(row[4]);
    if (row[5]) m.thumb_state = row[5];
    if (row[6]) m.thumb_width = std::atoi(row[6]);
    if (row[7]) m.poster_state = row[7];
    out.push_back(std::move(m));
  }
  mysql_free_result(res);
  return true;
}

std::optional<FileMeta> FileDao::Get(const std::string& folder, const std::string& name) {
  MYSQL* sql = nullptr;
  SqlConnRAII raii(&sql, SqlConnPool::Instance());
  if (!sql) {
    LOGERROR("FileDao::Get failed: get connection from pool failed");
    return std::nullopt;
  }
  std::string esc_folder = Escape(sql, folder);
  std::string esc_name = Escape(sql, name);
  if (esc_folder.empty() || esc_name.empty()) return std::nullopt;
  std::string q = "SELECT folder,name,size,mime_type,updated_at,thumb_state,thumb_width,poster_state FROM files "
                  "WHERE folder='" + esc_folder + "' AND name='" + esc_name + "' LIMIT 1";
  if (mysql_query(sql, q.c_str()) != 0) {
    LOGERROR(std::string("FileDao::Get failed: ") + mysql_error(sql));
    return std::nullopt;
  }
  MYSQL_RES* res = mysql_store_result(sql);
  if (!res) return std::nullopt;
  MYSQL_ROW row = mysql_fetch_row(res);
  if (!row) {
    mysql_free_result(res);
    return std::nullopt;
  }
  FileMeta m;
  if (row[0]) m.folder = row[0];
  if (row[1]) m.name = row[1];
  if (row[2]) m.size = std::atoll(row[2]);
  if (row[3]) m.mime_type = row[3];
  if (row[4]) m.updated_at = std::atoll(row[4]);
  if (row[5]) m.thumb_state = row[5];
  if (row[6]) m.thumb_width = std::atoi(row[6]);
  if (row[7]) m.poster_state = row[7];
  mysql_free_result(res);
  return m;
}

bool FileDao::SetThumbState(const std::string& folder, const std::string& name,
                            const std::string& state, int width) {
  MYSQL* sql = nullptr;
  SqlConnRAII raii(&sql, SqlConnPool::Instance());
  if (!sql) return false;
  std::string esc_folder = Escape(sql, folder);
  std::string esc_name = Escape(sql, name);
  std::string esc_state = Escape(sql, state);
  if (esc_folder.empty() || esc_name.empty() || esc_state.empty()) return false;
  std::string u = "UPDATE files SET thumb_state='" + esc_state + "', thumb_width=" + std::to_string(width) +
                  " WHERE folder='" + esc_folder + "' AND name='" + esc_name + "'";
  return mysql_query(sql, u.c_str()) == 0;
}

bool FileDao::SetPosterState(const std::string& folder, const std::string& name,
                             const std::string& state) {
  MYSQL* sql = nullptr;
  SqlConnRAII raii(&sql, SqlConnPool::Instance());
  if (!sql) return false;
  std::string esc_folder = Escape(sql, folder);
  std::string esc_name = Escape(sql, name);
  std::string esc_state = Escape(sql, state);
  if (esc_folder.empty() || esc_name.empty() || esc_state.empty()) return false;
  std::string u = "UPDATE files SET poster_state='" + esc_state +
                  "' WHERE folder='" + esc_folder + "' AND name='" + esc_name + "'";
  return mysql_query(sql, u.c_str()) == 0;
}
