// =====================================================================
//  03_sql_cpp.cpp —— SQL 背后到底发生了什么
//  老师傅带徒弟系列 · 第 1 关 · 第 3 课
// =====================================================================
//
//  这个程序干三件事：
//    ① 用 SQLite 的 C API，把 01_sql_tour.sql 一条条拆开、逐条执行；
//    ② 每条查询顺手打印 EXPLAIN QUERY PLAN（数据库打算怎么干活）；
//    ③ 造一张 10 万行的大表，实测【无索引 / 有索引】的耗时与扫描步数差异。
//
//  为什么让你写这个？因为你熟 C++。SQL 看起来" magic "，
//  其实它就是一个库函数调用：prepare → step → step → ... → done。
//  没有魔法，只有一个状态机在一行一行往外吐数据。
//  你把这段代码读懂，"数据库是个黑盒"这层窗户纸就捅破了。
//
//  怎么编译（PowerShell，在本目录下）：
//      g++ -std=c++17 -Wall -Wextra 03_sql_cpp.cpp -o 03_sql_cpp.exe -lsqlite3
//  万一 -lsqlite3 找不到库，换全路径：
//      g++ -std=c++17 -Wall -Wextra 03_sql_cpp.cpp -o 03_sql_cpp.exe ^
//          C:\msys64\ucrt64\lib\libsqlite3.a
//  怎么运行：
//      .\03_sql_cpp.exe
//      .\03_sql_cpp.exe 01_sql_tour.sql        ← 也可以显式指定 SQL 文件
//
//  程序会自建一个临时 .db 文件，退出时自动删除，不会污染目录。
// =====================================================================

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------
// 错误处理：把 SQLite 的错误码翻译成能看懂的人话并抛出去。
// C++ 的规矩：底层 API 返回错误码，边界处转成异常，别让错误码一路乱串。
// ---------------------------------------------------------------------
class SqlError : public std::runtime_error {
public:
    SqlError(const std::string& what, int rc, sqlite3* db)
        : std::runtime_error(what + " 失败：" +
                             (db ? sqlite3_errmsg(db) : "sqlite3 句柄为空") +
                             " (rc=" + std::to_string(rc) + ")") {}
};

// 表名/结果集里一行的数据，全部以文本形态保存（NULL 单独标记）。
struct Cell {
    std::string text;
    bool isNull = false;
};

// ---------------------------------------------------------------------
// Database：RAII 包装 sqlite3*。构造开库，析构关库，禁拷贝。
// 类比：这就是你自己写的 std::fstream 那种东西——
//       资源的生命周期绑在对象上，靠栈展开自动收尾。
// ---------------------------------------------------------------------
class Database {
public:
    explicit Database(const std::string& path) {
        const int rc = sqlite3_open(path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            // 即使 open 失败，句柄也可能已分配，必须先关掉再抛
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw SqlError("sqlite3_open(" + path + ")", rc, db_);
        }
        // SQLite 外键默认关闭，每个连接都要自己打开。01/02 里讲过的头号坑。
        exec("PRAGMA foreign_keys = ON;");
    }

    ~Database() {
        if (db_) sqlite3_close(db_);
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* handle() const noexcept { return db_; }

    // 一把梭执行（不关心结果集的场合：建表、PRAGMA、CREATE INDEX 等）
    void exec(const std::string& sql) {
        char* err = nullptr;
        const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string msg = err ? err : "未知错误";
            sqlite3_free(err);
            throw SqlError("exec(" + msg + ")", rc, db_);
        }
    }

private:
    sqlite3* db_ = nullptr;
};

// ---------------------------------------------------------------------
// Statement：RAII 包装 sqlite3_stmt*。可移动、禁拷贝。
// 类比：std::unique_ptr<sqlite3_stmt>，只是顺带带了解码方法。
// ---------------------------------------------------------------------
class Statement {
public:
    Statement() = default;

    // prepare 一段 SQL。返回时 tail 指向"这条语句之后的剩余文本"，
    // 调用方靠它继续切分下一条语句——这是逐条执行 SQL 文件的标准姿势。
    Statement(sqlite3* db, const char* sql, const char** tail) : db_(db) {
        const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, tail);
        if (rc != SQLITE_OK) throw SqlError("sqlite3_prepare_v2", rc, db);
    }

    Statement(Statement&& other) noexcept
        : db_(other.db_), stmt_(other.stmt_) {
        other.stmt_ = nullptr;
    }
    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            reset();
            db_ = other.db_;
            stmt_ = other.stmt_;
            other.stmt_ = nullptr;
        }
        return *this;
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    ~Statement() { reset(); }

    void reset() noexcept {
        if (stmt_) { sqlite3_finalize(stmt_); stmt_ = nullptr; }
    }

    // 给外部 prepare 用的"裸槽位"：先释放旧句柄，再把内部指针地址交出去。
    // 谨慎使用——交出裸指针等于暂时放弃 RAII 的保护，
    // 只有"我马上要拿它当输出参数"这种场合才该用。
    sqlite3_stmt** mutableSlot() noexcept {
        reset();
        return &stmt_;
    }
    bool valid() const noexcept { return stmt_ != nullptr; }

    // 只读句柄，供 sqlite3_column_* / sqlite3_stmt_status 这类 C API 使用。
    sqlite3_stmt* raw() const noexcept { return stmt_; }

    // 推进一步：SQLITE_ROW = 有一行数据；SQLITE_DONE = 取完了。
    int step() {
        const int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            throw SqlError("sqlite3_step", rc, db_);
        }
        return rc;
    }

    void rewind() { sqlite3_reset(stmt_); }

    int columnCount() const { return sqlite3_column_count(stmt_); }

    std::string columnName(int i) const {
        const char* n = sqlite3_column_name(stmt_, i);
        return n ? n : "?";
    }

    Cell column(int i) const {
        Cell c;
        if (sqlite3_column_type(stmt_, i) == SQLITE_NULL) {
            c.isNull = true;
            return c;
        }
        const unsigned char* txt = sqlite3_column_text(stmt_, i);
        c.text = txt ? reinterpret_cast<const char*>(txt) : "";
        return c;
    }

    // 读取"全表扫描步数"这个运行期计数器——本程序用它把"扫描代价"变成数字。
    long long fullScanSteps() const {
        return sqlite3_stmt_status(stmt_, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

// ---------------------------------------------------------------------
// 终端表格打印。坑：SQLite 出来的中文是 UTF-8，一个汉字 3 字节但占 2 列宽。
// 直接用 string::size() 对齐，表格必歪。所以要算"显示宽度"。
// ---------------------------------------------------------------------
std::size_t displayWidth(const std::string& s) {
    std::size_t w = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        std::size_t extra = 0;
        if (c < 0x80)        { cp = c; }
        else if (c < 0xE0)   { cp = c & 0x1Fu;        extra = 1; }
        else if (c < 0xF0)   { cp = c & 0x0Fu;        extra = 2; }
        else                 { cp = c & 0x07u;        extra = 3; }
        for (std::size_t k = 1; k <= extra; ++k) {
            if (i + k >= s.size()) break;
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        }
        i += extra + 1;
        const bool wide =
            (cp >= 0x1100 && cp <= 0x115F) ||
            (cp >= 0x2E80 && cp <= 0xA4CF) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE6F) ||
            (cp >= 0xFF00 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6);
        w += wide ? 2 : 1;
    }
    return w;
}

std::string padTo(const std::string& s, std::size_t width) {
    const std::size_t w = displayWidth(s);
    return w >= width ? s : s + std::string(width - w, ' ');
}

void printLine(const std::vector<std::size_t>& widths, char l, char m, char r) {
    std::string out(1, l);
    for (std::size_t i = 0; i < widths.size(); ++i) {
        out.append(widths[i] + 2, '-');
        out.push_back(i + 1 == widths.size() ? r : m);
    }
    std::cout << out << '\n';
}

void printRow(const std::vector<Cell>& cells, const std::vector<std::size_t>& widths) {
    std::cout << "|";
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const std::string text = cells[i].isNull ? "NULL" : cells[i].text;
        std::cout << ' ' << padTo(text, widths[i]) << " |";
    }
    std::cout << '\n';
}

// ---------------------------------------------------------------------
// EXPLAIN QUERY PLAN：向数据库要"执行计划"。
// 这是你看清 SQL 真实执行方式的第一台显微镜。
// ---------------------------------------------------------------------
bool printQueryPlan(sqlite3* db, const std::string& sql) {
    Statement plan;
    const char* unused = nullptr;
    try {
        plan = Statement(db, ("EXPLAIN QUERY PLAN " + sql).c_str(), &unused);
    } catch (const SqlError&) {
        // PRAGMA / BEGIN / CREATE TABLE 这类语句给不出查询计划，正常现象
        return false;
    }
    std::vector<std::string> details;
    while (plan.step() == SQLITE_ROW) {
        const unsigned char* d = sqlite3_column_text(plan.raw(), 3);
        details.emplace_back(d ? reinterpret_cast<const char*>(d) : "?");
    }
    if (details.empty()) return false;   // DDL / 事务控制语句：计划为空
    std::cout << "  [执行计划] ";
    for (std::size_t i = 0; i < details.size(); ++i) {
        std::cout << (i ? " / " : "") << details[i];
    }
    std::cout << '\n';
    return true;
}

// ---------------------------------------------------------------------
// 执行一条语句并表格化打印结果。
// maxRows 限制打印行数，避免长输出把终端刷爆。
// 返回：{结果行数, 全表扫描步数, 耗时微秒}
// ---------------------------------------------------------------------
struct RunStat {
    long long rows = 0;
    long long scanSteps = 0;
    long long micros = 0;
};

RunStat runAndPrint(sqlite3* db, const std::string& sql, std::size_t maxRows,
                    bool showPlan) {
    RunStat st;
    const char* tail = nullptr;
    Statement stmt(db, sql.c_str(), &tail);   // 只取第一条语句

    if (showPlan) printQueryPlan(db, sql);

    const auto t0 = std::chrono::steady_clock::now();

    const int cols = stmt.columnCount();
    std::vector<std::string> headers;
    std::vector<std::size_t> widths;
    std::vector<std::vector<Cell>> preview;

    bool headerReady = false;
    while (stmt.step() == SQLITE_ROW) {
        ++st.rows;
        if (!headerReady && cols > 0) {
            for (int i = 0; i < cols; ++i) {
                headers.push_back(stmt.columnName(i));
                widths.push_back(displayWidth(headers.back()));
            }
            headerReady = true;
        }
        std::vector<Cell> row;
        row.reserve(cols);
        for (int i = 0; i < cols; ++i) {
            Cell c = stmt.column(i);
            if (preview.size() < maxRows) {
                widths[i] = std::max(widths[i], displayWidth(c.isNull ? "NULL" : c.text));
            }
            row.push_back(std::move(c));
        }
        if (preview.size() < maxRows) preview.push_back(std::move(row));
    }

    const auto t1 = std::chrono::steady_clock::now();
    st.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    if (cols > 0) st.scanSteps = stmt.fullScanSteps();

    if (cols > 0) {
        printLine(widths, '+', '+', '+');
        printRow([&] {
            std::vector<Cell> h;
            for (auto& s : headers) h.push_back({s, false});
            return h;
        }(), widths);
        printLine(widths, '+', '+', '+');
        for (auto& r : preview) printRow(r, widths);
        printLine(widths, '+', '+', '+');
        if (st.rows > static_cast<long long>(preview.size())) {
            std::cout << "  ... 共 " << st.rows << " 行，只展示前 "
                      << preview.size() << " 行\n";
        }
    }
    std::cout << "  → " << st.rows << " 行";
    if (cols > 0) {
        std::cout << "，全表扫描步数 " << st.scanSteps
                  << "，耗时 " << st.micros << " 微秒\n";
    } else {
        std::cout << "（非查询语句），耗时 " << st.micros << " 微秒\n";
    }
    return st;
}

// 纯执行、不打印结果，只返回计时和扫描步数——用于性能对照实验。
RunStat runSilently(sqlite3* db, const std::string& sql, int times) {
    RunStat total;
    for (int i = 0; i < times; ++i) {
        const char* tail = nullptr;
        Statement stmt(db, sql.c_str(), &tail);
        const auto t0 = std::chrono::steady_clock::now();
        long long rows = 0;
        while (stmt.step() == SQLITE_ROW) ++rows;
        const auto t1 = std::chrono::steady_clock::now();
        total.rows = rows;   // 记录最后一次的结果行数，用于校验"两次结果一致"
        total.micros += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        total.scanSteps += stmt.fullScanSteps();
    }
    total.micros /= (times > 0 ? times : 1);
    total.scanSteps /= (times > 0 ? times : 1);
    return total;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("打不开文件：" + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF) s.erase(0, 3);  // 去 BOM
    return s;
}

// 临时 .db 文件的守卫：析构时删掉。哪怕中途抛异常也不会留下垃圾文件。
// 这就是 RAII 的价值——"一定会被清理"不靠自觉，靠语言保证。
class TempDbFile {
public:
    explicit TempDbFile(std::filesystem::path p) : path_(std::move(p)) {}
    ~TempDbFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        if (ec) std::cerr << "临时库删除失败：" << ec.message() << '\n';
    }
    TempDbFile(const TempDbFile&) = delete;
    TempDbFile& operator=(const TempDbFile&) = delete;
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

void section(const std::string& title) {
    std::cout << "\n=====================================================================\n"
              << "  " << title << "\n"
              << "=====================================================================\n";
}

}  // namespace

int main(int argc, char** argv) try {
    // ---------- 0. 定位 SQL 文件 ----------
    std::filesystem::path sqlPath;
    if (argc > 1) {
        sqlPath = argv[1];
    } else {
        sqlPath = "01_sql_tour.sql";
        if (!std::filesystem::exists(sqlPath)) {
            // 退一步：去 exe 自己所在的目录找
            std::error_code ec;
            auto exeDir = std::filesystem::path(argv[0]).parent_path();
            auto cand = exeDir / "01_sql_tour.sql";
            if (std::filesystem::exists(cand, ec)) sqlPath = cand;
        }
    }
    if (!std::filesystem::exists(sqlPath)) {
        std::cerr << "找不到 SQL 文件：" << sqlPath.string()
                  << "\n用法：" << argv[0] << " [01_sql_tour.sql]\n";
        return 1;
    }

    // ---------- 1. 建临时库（自动清理）----------
    auto tmpPath = std::filesystem::temp_directory_path() / "sqlcpp_tour_tmp.db";
    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);           // 上次可能没删干净
    TempDbFile guard(tmpPath);

    std::cout << "临时数据库：" << tmpPath.string() << "（退出时自动删除）\n"
              << "SQLite 版本：" << sqlite3_libversion() << '\n';

    Database db(tmpPath.string());

    // ---------- 2. 逐条执行 SQL 文件 ----------
    section("第一幕：把 " + sqlPath.filename().string() + " 逐条拆开执行");
    std::cout << "拆语句的姿势：sqlite3_prepare_v2 会顺带告诉你\"这条语句到哪儿结束\"\n"
              << "（tail 指针），拿着它继续 prepare 下一条即可。不用自己数分号。\n\n";

    const std::string content = readFile(sqlPath);
    const char* p = content.c_str();
    const char* end = p + content.size();

    int stmtIndex = 0, okCount = 0, failCount = 0;
    long long totalRows = 0, totalScan = 0, totalMicros = 0;

    while (p < end) {
        const char* tail = nullptr;
        Statement stmt;
        const int rc = sqlite3_prepare_v2(db.handle(), p, -1, stmt.mutableSlot(), &tail);
        if (rc != SQLITE_OK) {
            // 语法错误：打印出错位置附近文本，跳过这一段继续（教学程序不能一崩就全完）
            ++failCount;
            std::string piece(p, tail ? tail : end);
            piece.erase(0, piece.find_first_not_of(" \t\r\n"));
            if (piece.size() > 90) piece = piece.substr(0, 90) + "...";
            std::cerr << "  [准备失败] " << sqlite3_errmsg(db.handle())
                      << "\n             片段：" << piece << '\n';
            if (!tail) break;
            p = tail;
            continue;
        }
        if (!stmt.valid()) {   // 只剩注释或空白，收尾
            break;
        }
        ++stmtIndex;

        // 提取这条语句的原始文本，用于打印执行计划
        std::string sql(p, tail);
        p = tail;

        std::cout << "---- 语句 #" << stmtIndex << " ----\n";
        try {
            auto st = runAndPrint(db.handle(), sql, /*maxRows=*/12, /*showPlan=*/true);
            totalRows += st.rows;
            totalScan += st.scanSteps;
            totalMicros += st.micros;
            ++okCount;
        } catch (const SqlError& e) {
            ++failCount;
            std::cerr << "  [执行失败] " << e.what() << '\n';
        }
    }

    std::cout << "\n逐条执行完毕：共 " << stmtIndex << " 条，成功 " << okCount
              << " 条，失败 " << failCount << " 条；累计 " << totalRows
              << " 行 / 全表扫描 " << totalScan << " 步 / "
              << (totalMicros / 1000.0) << " 毫秒\n";

    // ---------- 3. 索引对照实验 ----------
    section("第二幕：索引到底值不值——10 万行实测");
    std::cout << "造一张 10 万行的表 big_loans，同一条查询测两遍：\n"
              << "查询：SELECT COUNT(*), SUM(amount) FROM big_loans WHERE reader_id = 123;\n"
              << "（每条重复跑 20 次取平均，抹平第一次的冷启动噪声）\n\n";

    db.exec("DROP TABLE IF EXISTS big_loans;");
    db.exec("CREATE TABLE big_loans (id INTEGER PRIMARY KEY, reader_id INTEGER,"
            " book_id INTEGER, amount INTEGER, note TEXT);");

    const auto tInsert0 = std::chrono::steady_clock::now();
    db.exec("BEGIN;");
    db.exec(R"(
        WITH RECURSIVE seq(n) AS (
            SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 100000
        )
        INSERT INTO big_loans (reader_id, book_id, amount, note)
        SELECT (n % 900) + 1, (n % 120) + 1, n % 1000, 'note-' || n FROM seq;
    )");
    db.exec("COMMIT;");
    const auto tInsert1 = std::chrono::steady_clock::now();
    std::cout << "灌入 100000 行耗时 "
              << std::chrono::duration_cast<std::chrono::milliseconds>(tInsert1 - tInsert0).count()
              << " 毫秒\n\n";

    const std::string probe =
        "SELECT COUNT(*) AS cnt, SUM(amount) AS total FROM big_loans WHERE reader_id = 123;";

    std::cout << "【A. 没有索引】\n";
    runAndPrint(db.handle(), probe, 5, true);
    auto noIdx = runSilently(db.handle(), probe, 20);

    const auto tIdx0 = std::chrono::steady_clock::now();
    db.exec("CREATE INDEX idx_big_reader ON big_loans(reader_id);");
    const auto tIdx1 = std::chrono::steady_clock::now();

    std::cout << "\n【B. 建了索引 idx_big_reader(reader_id) 之后】\n"
              << "（建索引本身花了 "
              << std::chrono::duration_cast<std::chrono::milliseconds>(tIdx1 - tIdx0).count()
              << " 毫秒——天下没有免费的午餐）\n";
    runAndPrint(db.handle(), probe, 5, true);
    auto withIdx = runSilently(db.handle(), probe, 20);

    std::cout << "\n【对照结论】\n"
              << "  无索引：平均 " << noIdx.micros << " 微秒，全表扫描 "
              << noIdx.scanSteps << " 步\n"
              << "  有索引：平均 " << withIdx.micros << " 微秒，全表扫描 "
              << withIdx.scanSteps << " 步\n"
              << "  两次都返回同样的 cnt / total（上面 A、B 两块表格里数字一模一样）——\n"
              << "  索引只改【怎么找】，绝不改【找到什么】。结果不对，一定不是索引的锅。\n";
    if (noIdx.micros > 0 && withIdx.micros > 0) {
        std::cout << "  提速约 "
                  << (static_cast<double>(noIdx.micros) / static_cast<double>(withIdx.micros))
                  << " 倍。扫描步数从十万级掉到 0，就是 B 树目录的威力。\n";
    }

    // ---------- 4. 索引不是万能药 ----------
    section("第三幕：索引只对\"用得上它的查询\"有效");
    const std::string probe2 =
        "SELECT COUNT(*) AS cnt FROM big_loans WHERE amount = 555;";
    std::cout << "查询：" << probe2 << "\n";
    std::cout << "amount 列上没有索引，虽然表上已经躺着一个 reader_id 索引——\n";
    auto miss = runSilently(db.handle(), probe2, 20);
    runAndPrint(db.handle(), probe2, 3, true);
    std::cout << "  平均 " << miss.micros << " 微秒，全表扫描 " << miss.scanSteps
              << " 步：照样从头扫到尾。\n"
              << "  记住这句话：索引是\"按列、按查询\"起作用的。建了不相干的索引，\n"
              << "  写入付了代价，查询一分钱好处都捞不到。\n";

    // ---------- 5. 收尾 ----------
    section("小结");
    std::cout << "1. SQL 落到程序里，就是 prepare_v2 + step 循环，没有魔法。\n"
              << "2. EXPLAIN QUERY PLAN 看到 SCAN = 逐行硬扫，SEARCH ... USING INDEX = 走树。\n"
              << "3. 全表扫描步数（SQLITE_STMTSTATUS_FULLSCAN_STEP）能把\"代价\"变成可读数字。\n"
              << "4. 索引以写换读、以空间换时间，且只对命中它的查询生效。\n"
              << "5. 句柄交给 RAII 对象管，临时文件走守卫类，异常也漏不掉清理。\n";

    std::cout << "\n临时库即将删除：" << guard.path().string() << '\n';
    return 0;
} catch (const std::exception& e) {
    std::cerr << "\n致命错误：" << e.what() << '\n';
    return 2;
}

// =====================================================================
//  老师傅的话
//
//  徒弟，你为什么得写这个程序？因为你只会写 SQL 的时候，SQL 是个黑盒：
//  你写一条语句，它给你结果，你不知道中间发生了啥，于是你也没法判断
//  这条语句到底是快还是慢。等你把 prepare/step 这两下亲手敲过，
//  "查询"这件事就从玄学变成了代码——一个状态机在一行一行往外吐数据，
//  跟你写的 while (std::getline(in, line)) 没有本质区别。
//
//  第二件事，比 API 更重要：EXPLAIN QUERY PLAN 和全表扫描步数。
//  这俩是你从"会写 SQL"跨到"会写快的 SQL"的唯一入口。
//  别信"加个索引就快了"这种口号，跑一遍，看数字。数字不会骗你，
//  你的直觉会。这个程序里 100000 行那一段，你亲手改一改：
//  把数据量改成 100 万，改成 10 行，看看差距怎么变，感受一下
//  "数据量小的时候索引没用甚至更慢"到底是怎么回事。
//
//  第三件事，也是师傅最想啰嗦的：RAII。
//  这个程序里没有一个 sqlite3_close / sqlite3_finalize 需要你手写。
//  Database 和 Statement 的析构函数替你把资源收了，TempDbFile 替你把
//  垃圾文件删了——哪怕中途抛异常，栈展开也会一个一个调析构。
//  你在 C++ 里养成的这个习惯，到了数据库编程里就是保命的东西。
//  多少线上事故的源头，就是"错误分支上忘了 finalize 那个 stmt"。
//
//  课后练习（都要求你亲手改代码 + 编译 + 跑出结果）：
//
//  练习 1：把 mutableSlot() 这套"裸槽位"用法改干净
//    main 的逐条执行循环里，先 prepare 一次判断边界，再由 runAndPrint
//    重新 prepare 一次——同一条 SQL 编译了两遍。请你把 Statement 改成
//    带一个"静态工厂 + 输出 tail"的形式（例如
//      static Statement prepareWithTail(sqlite3*, const char*, const char** tail);
//    ），让 main 直接把准备好的语句交给打印函数复用，去掉重复 prepare。
//    做完想一想：为什么"交出裸槽位"这种写法在 RAII 类里要尽量少用？
//
//  练习 2：给第三幕补一个"复合索引"实验
//    在 big_loans 上建 CREATE INDEX idx_big_reader_amount ON big_loans(reader_id, amount)，
//    然后比较这三条查询的执行计划与扫描步数：
//      a) WHERE reader_id = 123
//      b) WHERE reader_id = 123 AND amount = 555
//      c) WHERE amount = 555
//    观察 c 用不上这个索引——这就是"最左前缀"原则。把它跑出来、看懂。
//
//  练习 3：让程序能安全处理一个"有语法错误的 SQL 文件"
//    自己写一个 bad.sql，里面故意放一条写错的语句（比如 SELECT FROM WHERE），
//    前后各放几条正常语句，然后运行  .\03_sql_cpp.exe bad.sql 。
//    确认程序不会崩、会打印友好错误、并继续执行后面的语句。
//    （这个"局部失败不影响全局"的能力，就是本文件 02 里
//      那条故意违反外键的语句所体现的同一种工程素养。）
// =====================================================================
