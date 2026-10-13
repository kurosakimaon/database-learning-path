/*
================================================================
  02_raii_wrapper.cpp —— 第 4 关第二站：把 C API 包成"能进生产"的样子

  编译：  g++ -std=c++17 -O2 02_raii_wrapper.cpp -o 02.exe -lsqlite3
  运行：  .\02.exe

  师傅的话：
    上一个文件你手写了几十次 check(rc, ...) 和 finalize。
    写十次是学习，写一千次就是事故源——总有一次忘记 finalize，
    总有一次 return 前忘记 close，总有一次异常抛出后连接泄漏。

    这一关咱们用 C++ 的老本领把它收拾干净：
      资源获取即初始化（RAII）—— 对象活多久，资源就活多久；
      离开作用域，析构函数自动收尾。异常扔出去也一样收得住。

    本文件产出的 Db / Stmt / Row / Transaction 四件套，
    你明天就能抄进公司项目里用（当然，正式项目建议用成熟库，
    但"为什么这么设计"这套思路，比库本身值钱）。

    最后还有一个必须亲手测出来的数字：
      1 万条 INSERT，逐条自动提交 vs 放进一个事务里提交。
    不测一次，你永远不知道 fsync 有多贵。
================================================================
*/

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static void setup_utf8_console()
{
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
}

// ===========================================================================
// 一、异常类型
// ===========================================================================
//
// 为什么自定义异常，而不直接用 std::system_error？
// 因为数据库错误带三个信息：SQLite 主错误码、扩展错误码、可读文本。
// 单独一个类型，上层可以 catch(SqliteError&) 拿到 rc 做分支
// （比如 SQLITE_BUSY 要重试，SQLITE_CONSTRAINT 要给用户提示，
//   SQLITE_CORRUPT 要报警），这才是工程用法。
class SqliteError : public std::runtime_error
{
public:
    SqliteError(int rc, int extended, std::string msg, std::string ctx)
        : std::runtime_error(build(rc, extended, msg, ctx)),
          rc_(rc), extended_(extended), context_(std::move(ctx))
    {}

    [[nodiscard]] int rc() const noexcept { return rc_; }
    [[nodiscard]] int extended_rc() const noexcept { return extended_; }
    [[nodiscard]] const std::string &context() const noexcept { return context_; }

private:
    static std::string build(int rc, int extended, const std::string &msg, const std::string &ctx)
    {
        return "[" + ctx + "] sqlite rc=" + std::to_string(rc) +
               " (extended=" + std::to_string(extended) + "): " + msg;
    }

    int rc_;
    int extended_;
    std::string context_;
};

// 统一检查点：把 C 的返回码翻译成 C++ 的异常。
// 注意 sqlite3_errmsg 可能返回 nullptr（极端情况），这里兜一下。
static void throw_if_error(sqlite3 *db, int rc, const char *ctx)
{
    if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW)
        return;
    const char *m = db ? sqlite3_errmsg(db) : nullptr;
    const int ext = db ? sqlite3_extended_errcode(db) : rc;
    throw SqliteError(rc, ext, m ? m : "(no message)", ctx);
}

// ===========================================================================
// 二、Row：一行结果
// ===========================================================================
//
// 设计要点：
//   1. Row 是"值语义"的快照。sqlite3_column_text 返回的指针属于 stmt，
//      下一次 step 就失效了。所以 Row 必须在构造时把内容拷出来。
//      这是 SQLite C API 最经典的坑：拿 const char* 存起来，
//      循环跑完再看——内容全变了或者崩了。
//   2. get<T>(i) 支持 int32/int64/double/std::string；越界或类型不对抛异常，
//      而不是返回垃圾值。宁可炸在开发期，不要脏在生产线。
class Row
{
public:
    struct Cell
    {
        bool is_null = true;
        long long i = 0;
        double d = 0.0;
        std::string s;
    };

    Row() = default;

    // 从 stmt 拷一份当前行的快照。列数由 stmt 决定。
    explicit Row(sqlite3_stmt *st)
    {
        const int n = sqlite3_column_count(st);
        cells_.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            Cell c;
            const int type = sqlite3_column_type(st, i);
            c.is_null = (type == SQLITE_NULL);
            switch (type)
            {
            case SQLITE_INTEGER:
                c.i = sqlite3_column_int64(st, i);
                c.d = static_cast<double>(c.i);
                break;
            case SQLITE_FLOAT:
                c.d = sqlite3_column_double(st, i);
                c.i = static_cast<long long>(c.d);
                break;
            case SQLITE_TEXT:
            {
                const auto *p = sqlite3_column_text(st, i);
                const int nbytes = sqlite3_column_bytes(st, i);
                if (p)
                    c.s.assign(reinterpret_cast<const char *>(p), static_cast<size_t>(nbytes));
                break;
            }
            case SQLITE_BLOB:
            {
                // blob 这里按原始字节存进 s（演示够用）。
                const void *p = sqlite3_column_blob(st, i);
                const int nbytes = sqlite3_column_bytes(st, i);
                if (p && nbytes > 0)
                    c.s.assign(static_cast<const char *>(p), static_cast<size_t>(nbytes));
                break;
            }
            default:
                break; // SQLITE_NULL
            }
            cells_.push_back(std::move(c));
        }
    }

    [[nodiscard]] size_t size() const noexcept { return cells_.size(); }
    [[nodiscard]] bool is_null(size_t i) const { return at(i).is_null; }

    // get<T>：整数
    template <class T>
    [[nodiscard]] T get(size_t i) const;

private:
    [[nodiscard]] const Cell &at(size_t i) const
    {
        if (i >= cells_.size())
            throw std::out_of_range("Row::get 列下标越界: " + std::to_string(i));
        return cells_[i];
    }

    std::vector<Cell> cells_;
};

template <>
inline int32_t Row::get<int32_t>(size_t i) const
{
    const auto &c = at(i);
    if (c.is_null)
        throw SqliteError(SQLITE_NULL, SQLITE_NULL, "列 " + std::to_string(i) + " 是 NULL，取 int32 失败", "Row::get");
    return static_cast<int32_t>(c.i);
}

template <>
inline int64_t Row::get<int64_t>(size_t i) const
{
    const auto &c = at(i);
    if (c.is_null)
        throw SqliteError(SQLITE_NULL, SQLITE_NULL, "列 " + std::to_string(i) + " 是 NULL，取 int64 失败", "Row::get");
    return c.i;
}

template <>
inline double Row::get<double>(size_t i) const
{
    const auto &c = at(i);
    if (c.is_null)
        throw SqliteError(SQLITE_NULL, SQLITE_NULL, "列 " + std::to_string(i) + " 是 NULL，取 double 失败", "Row::get");
    return c.d;
}

template <>
inline std::string Row::get<std::string>(size_t i) const
{
    const auto &c = at(i);
    if (c.is_null)
        throw SqliteError(SQLITE_NULL, SQLITE_NULL, "列 " + std::to_string(i) + " 是 NULL，取 string 失败", "Row::get");
    return c.s; // 返回拷贝，安全
}

// ===========================================================================
// 三、Db：连接句柄（move-only）
// ===========================================================================
//
// 为什么 move-only？
//   两个对象持有同一个 sqlite3*，析构时就会 close 两次（double free）。
//   禁止拷贝、只允许移动，就把这个坑从"要靠自觉"变成"编译期不可能"。
//   这也是 unique_ptr 的设计哲学，照抄就对了。
class Db
{
public:
    explicit Db(const std::string &path)
    {
        sqlite3 *h = nullptr;
        // 用 sqlite3_open_v2 可以更明确地声明打开方式，
        // 也能加 SQLITE_OPEN_FULLMUTEX（序列化模式，多线程安全）。
        int rc = sqlite3_open_v2(path.c_str(), &h,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
        if (rc != SQLITE_OK)
        {
            const char *m = h ? sqlite3_errmsg(h) : "unknown";
            std::string what = std::string("打开数据库失败: ") + path + " : " + (m ? m : "");
            sqlite3_close(h);
            throw SqliteError(rc, h ? sqlite3_extended_errcode(h) : rc, what, "Db::Db");
        }
        handle_ = h;
        path_ = path;
        // 扩展错误码：打开后能区分 SQLITE_CONSTRAINT_UNIQUE / _FOREIGNKEY 等，
        // 光看 SQLITE_CONSTRAINT 什么都做不了。生产库标配。
        sqlite3_extended_result_codes(handle_, 1);
        // busy_timeout：遇到锁自动等一会儿再重试，省掉一大半 SQLITE_BUSY。
        // 单位毫秒。这个值不是越大越好——它直接变成用户的等待时间。
        sqlite3_busy_timeout(handle_, 5000);
    }

    Db(const Db &) = delete;
    Db &operator=(const Db &) = delete;

    Db(Db &&o) noexcept : handle_(o.handle_), path_(std::move(o.path_))
    {
        o.handle_ = nullptr; // 移走后必须置空，否则析构时双重 close
    }

    Db &operator=(Db &&o) noexcept
    {
        if (this != &o)
        {
            close_now();
            handle_ = o.handle_;
            path_ = std::move(o.path_);
            o.handle_ = nullptr;
        }
        return *this;
    }

    ~Db() { close_now(); }

    [[nodiscard]] sqlite3 *raw() const noexcept { return handle_; }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }

    // 无结果集语句（DDL、BEGIN/COMMIT 这类控制语句）。
    // 分成两个接口是有讲究的：DDL 没有"受影响行数"这个概念，
    // 让它返回 int 只会逼调用者到处写 (void)；而 DML 的行数必须关心，
    // 所以给它单独的名字并标 [[nodiscard]]，忘了检查就编译告警。
    void exec(const std::string &sql)
    {
        char *err = nullptr;
        const int rc = sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK)
        {
            std::string m = err ? err : "(no message)";
            sqlite3_free(err);
            throw SqliteError(rc, sqlite3_extended_errcode(handle_), m, "Db::exec");
        }
    }

    // DML（INSERT/UPDATE/DELETE）：必须看受影响行数。
    // "UPDATE 了 0 行"到底算成功还是失败？这是业务问题，但不能不回答。
    [[nodiscard]] int exec_dml(const std::string &sql)
    {
        exec(sql);
        return sqlite3_changes(handle_);
    }

    // 标量查询：取第一行第一列。取不到返回 std::nullopt。
    // 这是最高频的"COUNT(*)/MAX(x)"需求，值得单开一个接口。
    [[nodiscard]] std::optional<int64_t> query_scalar_int(const std::string &sql);

private:
    void close_now() noexcept
    {
        if (!handle_)
            return;
        const int rc = sqlite3_close(handle_);
        if (rc != SQLITE_OK)
        {
            // 关不掉通常说明还有没 finalize 的 stmt。
            // 析构函数里不能抛异常（可能 terminate），只能打日志。
            // 这就是"析构函数要 noexcept"的现实理由。
            std::fprintf(stderr, "[警告] sqlite3_close 返回 %d（可能有未释放的语句）\n", rc);
        }
        handle_ = nullptr;
    }

    sqlite3 *handle_ = nullptr;
    std::string path_;
};

// ===========================================================================
// 四、Stmt：预处理语句（RAII finalize）
// ===========================================================================
//
// 两个价值：
//   1. 自动 finalize —— 忘不掉了。
//   2. 语句复用 —— prepare 只做一次，reset 后反复 bind/step。
//      1 万条 INSERT 用同一条语句，比每次都 prepare 快得多。
class Stmt
{
public:
    Stmt(sqlite3 *db, const std::string &sql) : db_(db)
    {
        sqlite3_stmt *st = nullptr;
        const int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
        throw_if_error(db, rc, "Stmt::prepare");
        st_ = st;
        sql_ = sql;
    }

    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;

    Stmt(Stmt &&o) noexcept : db_(o.db_), st_(o.st_), sql_(std::move(o.sql_))
    {
        o.st_ = nullptr;
    }

    ~Stmt()
    {
        if (st_)
            sqlite3_finalize(st_);
    }

    // ---- bind 系列：重载而不是模板，是为了让"支持哪些类型"一眼可见 ----
    // 参数下标从 1 开始（SQLite 的规矩），接口里保持从 1，
    // 但内部会检查边界，越界直接抛，避免把 0 传给 C API 变成 SQLITE_RANGE。
    Stmt &bind(int idx, int32_t v) { return bind_i64(idx, v); }
    Stmt &bind(int idx, int64_t v) { return bind_i64(idx, v); }
    Stmt &bind(int idx, double v)
    {
        check_bound(idx);
        throw_if_error(db_, sqlite3_bind_double(st_, idx, v), "bind(double)");
        return *this;
    }
    Stmt &bind(int idx, const char *v)
    {
        check_bound(idx);
        if (!v)
            return bind_null(idx);
        // SQLITE_TRANSIENT：让 SQLite 自己拷贝。宁可多拷一次，不赌生命周期。
        throw_if_error(db_, sqlite3_bind_text(st_, idx, v, -1, SQLITE_TRANSIENT), "bind(text)");
        return *this;
    }
    Stmt &bind(int idx, const std::string &v)
    {
        check_bound(idx);
        // 用 size() 而不是 -1：显式给长度，二进制安全，
        // 也不会因为字符串里有 '\0' 而被截断。
        throw_if_error(db_,
                       sqlite3_bind_text(st_, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT),
                       "bind(text)");
        return *this;
    }

    Stmt &bind_null(int idx)
    {
        check_bound(idx);
        throw_if_error(db_, sqlite3_bind_null(st_, idx), "bind(null)");
        return *this;
    }

    // 可空语义：nullopt → SQL NULL。业务里"这列可能没值"太常见了。
    Stmt &bind_opt(int idx, const std::optional<std::string> &v)
    {
        return v ? bind(idx, *v) : bind_null(idx);
    }
    Stmt &bind_opt(int idx, const std::optional<double> &v)
    {
        return v ? bind(idx, *v) : bind_null(idx);
    }

    // 复用语句前必须 rewind（对应 sqlite3_reset）。
    // 为什么公开它：因为"复用同一条语句"是性能优化的核心手段，
    // 而复用前不复位 = 未定义行为（绑定已 step 过的语句会返回 SQLITE_MISUSE）。
    // 与其让人到处踩，不如把开关交出来并写清楚规矩。
    void rewind() noexcept { sqlite3_reset(st_); }

    // 执行到结束（用于 INSERT/UPDATE/DELETE）。返回受影响行数。
    void run()
    {
        const int rc = sqlite3_step(st_);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        {
            const int ext = sqlite3_extended_errcode(db_);
            const char *m = sqlite3_errmsg(db_);
            // step 失败后语句停在错误状态，复位后再用，不然下次 step 还是这个错。
            rewind();
            throw SqliteError(rc, ext, m ? m : "(no message)", "Stmt::run [" + sql_ + "]");
        }
        rewind();
    }

    // 取下一行；没有更多行返回 nullopt。
    // 用 optional 而不是"返回 bool + 出参"，调用点写成 while (auto row = stmt.next())
    // 非常顺手，也不会忘记初始化出参。
    [[nodiscard]] std::optional<Row> next()
    {
        const int rc = sqlite3_step(st_);
        if (rc == SQLITE_ROW)
            return Row(st_);
        if (rc == SQLITE_DONE)
        {
            rewind();
            return std::nullopt;
        }
        const int ext = sqlite3_extended_errcode(db_);
        const char *m = sqlite3_errmsg(db_);
        rewind();
        throw SqliteError(rc, ext, m ? m : "(no message)", "Stmt::next [" + sql_ + "]");
    }

    // 一次性把所有行取走（结果集不大时用，方便排序/复用）。
    [[nodiscard]] std::vector<Row> fetch_all()
    {
        std::vector<Row> out;
        while (auto r = next())
            out.push_back(std::move(*r));
        return out;
    }

    [[nodiscard]] int param_count() const noexcept { return sqlite3_bind_parameter_count(st_); }

private:
    Stmt &bind_i64(int idx, int64_t v)
    {
        check_bound(idx);
        throw_if_error(db_, sqlite3_bind_int64(st_, idx, v), "bind(int64)");
        return *this;
    }

    void check_bound(int idx) const
    {
        if (idx < 1 || idx > sqlite3_bind_parameter_count(st_))
            throw std::out_of_range("bind 参数下标越界: " + std::to_string(idx) +
                                    "，该语句共 " + std::to_string(sqlite3_bind_parameter_count(st_)) + " 个参数");
    }

    sqlite3 *db_;
    sqlite3_stmt *st_ = nullptr;
    std::string sql_;
};

inline std::optional<int64_t> Db::query_scalar_int(const std::string &sql)
{
    Stmt st(raw(), sql);
    auto row = st.next();
    if (!row || row->size() == 0 || row->is_null(0))
        return std::nullopt;
    return row->get<int64_t>(0);
}

// 便捷函数：从 Db 造 Stmt，省得每次写 db.raw()。
[[nodiscard]] inline Stmt prepare(Db &db, const std::string &sql)
{
    return Stmt(db.raw(), sql);
}

// ===========================================================================
// 五、Transaction：异常安全的 BEGIN / COMMIT / ROLLBACK
// ===========================================================================
//
// 这是本文件最值钱的一个小类。想想手写会出什么事：
//     exec("BEGIN");
//     ... 中间某句抛异常 ...
//     exec("COMMIT");   ← 永远执行不到
// 结果：事务挂在那里不结束，连接一直被写锁占着，别人全堵住。
// 用 RAII：析构时如果没显式 commit()，一律 ROLLBACK。
// 于是"异常安全"不是靠小心，而是靠结构保证。
class Transaction
{
public:
    enum class Kind
    {
        Deferred, // 默认：等到真正读写时才加锁
        Immediate, // 一上来就抢写锁，适合明确要写的场景（避免升级失败）
        Exclusive  // 排他，最重
    };

    explicit Transaction(Db &db, Kind kind = Kind::Deferred) : db_(db)
    {
        const char *stmt =
            kind == Kind::Immediate ? "BEGIN IMMEDIATE" : kind == Kind::Exclusive ? "BEGIN EXCLUSIVE"
                                                                                   : "BEGIN";
        db_.exec(stmt);
        active_ = true;
    }

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    ~Transaction()
    {
        if (!active_)
            return;
        // 析构里绝对不能抛：异常正在展开时再抛就是 std::terminate。
        try
        {
            if (!committed_)
                db_.exec("ROLLBACK");
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "[警告] 事务析构回滚失败：%s\n", e.what());
        }
    }

    void commit()
    {
        if (!active_)
            throw std::logic_error("事务已结束，重复 commit");
        db_.exec("COMMIT"); // 这里失败会抛异常给上层，由上层决定重试还是报错
        committed_ = true;
        active_ = false;
    }

    void rollback()
    {
        if (!active_)
            return;
        db_.exec("ROLLBACK");
        active_ = false;
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool committed() const noexcept { return committed_; }

private:
    Db &db_;
    bool active_ = false;
    bool committed_ = false;
};

// ===========================================================================
// 六、计时小工具
// ===========================================================================
class Timer
{
public:
    Timer() : t0_(std::chrono::steady_clock::now()) {}
    [[nodiscard]] double ms() const
    {
        using namespace std::chrono;
        return duration_cast<duration<double, std::milli>>(steady_clock::now() - t0_).count();
    }
    void restart() { t0_ = std::chrono::steady_clock::now(); }

private:
    std::chrono::steady_clock::time_point t0_;
};

// 看库文件有多大：批量导入后文件大小是个很好的旁证，
// 说明"数据量"和"磁盘占用"不是一回事（页填充率、索引、WAL 都算在里面）。
static long long file_size_of(const std::string &p)
{
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    return ec ? -1 : static_cast<long long>(sz);
}

// ===========================================================================
// 七、业务演示
// ===========================================================================
//
// 小业务：订单导入。orders(id, user_id, amount, memo, created_seq)
//   1. 逐条自动提交导入 N 条 → 计时
//   2. 单事务批量导入 N 条    → 计时
//   3. 对比，算倍数
//   4. 用事务做一次"转账"演示（异常自动回滚）
// ===========================================================================
static void demo_batch_vs_transaction(const char *path, const int N)
{
    std::printf("\n---------------- 实测：逐条提交 vs 单事务提交（N=%d）----------------\n", N);
    double e1 = 0.0, e2 = 0.0, e3 = 0.0; // 三个场景的耗时（毫秒）

    // ---------- 场景 1：逐条自动提交 ----------
    {
        std::remove(path);
        std::remove("02_orders.db-journal");
        Db db(path);
        db.exec("CREATE TABLE orders(id INTEGER PRIMARY KEY, user_id INTEGER,"
                " amount REAL NOT NULL, memo TEXT, created_seq INTEGER)");

        Timer t;
        Stmt st(db.raw(), "INSERT INTO orders(user_id,amount,memo,created_seq) VALUES(?,?,?,?)");
        for (int i = 0; i < N; ++i)
        {
            st.bind(1, static_cast<int64_t>(i % 1000))
                .bind(2, 9.9 + (i % 100) * 0.5)
                .bind(3, std::string("订单-") + std::to_string(i))
                .bind(4, static_cast<int32_t>(i));
            st.run(); // 没有事务 → 每条 INSERT 都自带一个隐式事务并 fsync
            if (i > 0 && i % 2000 == 0)
            {
                // 这个场景慢到需要打进度，否则你以为程序挂了。
                // "慢到要打进度的插入"本身就是本文件最重要的实验结论。
                std::printf("  ...已逐条提交 %d 条，用时 %.0f ms\n", i, t.ms());
                std::fflush(stdout);
            }
        }
        const double elapsed = t.ms();
        e1 = elapsed;
        const auto cnt = db.query_scalar_int("SELECT COUNT(*) FROM orders");
        std::printf("场景1 逐条自动提交：%10.1f ms  （写入 %lld 行）\n", elapsed,
                    cnt.value_or(-1));
        std::printf("         → 平均每条 %.3f ms。慢在哪？每条都要 fsync 落盘 + 写 WAL/回滚日志。\n",
                    elapsed / N);
    }

    // ---------- 场景 2：放进一个事务 ----------
    {
        std::remove(path);
        std::remove("02_orders.db-journal");
        Db db(path);
        db.exec("CREATE TABLE orders(id INTEGER PRIMARY KEY, user_id INTEGER,"
                " amount REAL NOT NULL, memo TEXT, created_seq INTEGER)");

        Timer t;
        {
            Transaction tx(db, Transaction::Kind::Immediate);
            Stmt st(db.raw(), "INSERT INTO orders(user_id,amount,memo,created_seq) VALUES(?,?,?,?)");
            for (int i = 0; i < N; ++i)
            {
                st.bind(1, static_cast<int64_t>(i % 1000))
                    .bind(2, 9.9 + (i % 100) * 0.5)
                    .bind(3, std::string("订单-") + std::to_string(i))
                    .bind(4, static_cast<int32_t>(i));
                st.run();
            }
            tx.commit(); // 只 fsync 一次
        }
        const double elapsed = t.ms();
        e2 = elapsed;
        const auto cnt = db.query_scalar_int("SELECT COUNT(*) FROM orders");
        std::printf("场景2 单事务批量提交：%9.1f ms  （写入 %lld 行）\n", elapsed,
                    cnt.value_or(-1));
        std::printf("         → 平均每条 %.3f ms。库文件 %lld 字节（页大小 4096，可见页填充率的影响）。\n",
                    elapsed / N, file_size_of(path));
    }

    // ---------- 场景 3：中等事务（每 500 条一批）——生产里最常用的折中 ----------
    {
        std::remove(path);
        std::remove("02_orders.db-journal");
        Db db(path);
        db.exec("CREATE TABLE orders(id INTEGER PRIMARY KEY, user_id INTEGER,"
                " amount REAL NOT NULL, memo TEXT, created_seq INTEGER)");

        const int kBatch = 500;
        Timer t;
        Stmt st(db.raw(), "INSERT INTO orders(user_id,amount,memo,created_seq) VALUES(?,?,?,?)");
        for (int i = 0; i < N; i += kBatch)
        {
            Transaction tx(db, Transaction::Kind::Immediate);
            const int end = (i + kBatch < N) ? i + kBatch : N;
            for (int j = i; j < end; ++j)
            {
                st.bind(1, static_cast<int64_t>(j % 1000))
                    .bind(2, 9.9 + (j % 100) * 0.5)
                    .bind(3, std::string("订单-") + std::to_string(j))
                    .bind(4, static_cast<int32_t>(j));
                st.run();
            }
            tx.commit();
        }
        const double elapsed = t.ms();
        e3 = elapsed;
        const auto cnt = db.query_scalar_int("SELECT COUNT(*) FROM orders");
        std::printf("场景3 每 %d 条一个事务：%8.1f ms  （写入 %lld 行）\n", kBatch, elapsed,
                    cnt.value_or(-1));
        std::printf("         → 为什么不全放一个事务？万一失败，代价是整批重做；\n"
                    "           而且长事务会挡住 checkpoint、撑大 WAL、让别人读不到新数据。\n");
    }

    // ---------- 汇总：倍数是算出来的，不是抄来的 ----------
    std::printf("\n  汇总（本机实测）：\n");
    std::printf("    逐条自动提交      : %10.1f ms\n", e1);
    std::printf("    单事务批量提交    : %10.1f ms\n", e2);
    std::printf("    每 500 条一事务   : %10.1f ms\n", e3);
    if (e2 > 0.0)
        std::printf("    >>> 单事务比逐条快 %.1f 倍；每 500 条一批比逐条快 %.1f 倍。\n",
                    e1 / e2, e1 / (e3 > 0.0 ? e3 : 1.0));
    std::printf("    （这个倍数跟机器、磁盘、杀软、系统缓存都相关，你自己那台机器跑出来会不一样，\n"
                "      但\"事务快一到两个数量级\"这个结论在哪儿都一样。）\n");

    std::remove(path);
    std::remove("02_orders.db-journal");
    std::remove("02_orders.db-wal");
    std::remove("02_orders.db-shm");
}

static void demo_transaction_exception_safety(const char *path)
{
    std::printf("\n---------------- 实测：事务的异常安全 ----------------\n");
    std::remove(path);
    std::remove("02_txn_demo.db-journal");
    Db db(path);
    db.exec("CREATE TABLE account(id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
            " balance REAL NOT NULL, CHECK(balance >= 0))");
    Stmt(db.raw(), "INSERT INTO account(name,balance) VALUES(?,?)").bind(1, std::string("A")).bind(2, 100.0).run();
    Stmt(db.raw(), "INSERT INTO account(name,balance) VALUES(?,?)").bind(1, std::string("B")).bind(2, 30.0).run();

    // 用正确的方式打印余额
    auto show = [&](const char *tag)
    {
        Stmt q(db.raw(), "SELECT name, balance FROM account ORDER BY id");
        std::printf("%s", tag);
        for (auto row : q.fetch_all())
            std::printf("  %s=%.2f", row.get<std::string>(0).c_str(), row.get<double>(1));
        std::printf("\n");
    };
    show("[当前余额]");

    // 转账 A→B 30，转账成功后故意插一条违反 CHECK 约束的数据制造异常
    std::printf("\n执行转账 A→B 30，随后故意插入违反 CHECK 约束的数据：\n");
    try
    {
        Transaction tx(db, Transaction::Kind::Immediate);
        Stmt(db.raw(), "UPDATE account SET balance = balance - ? WHERE name = ?")
            .bind(1, 30.0)
            .bind(2, std::string("A"))
            .run();
        Stmt(db.raw(), "UPDATE account SET balance = balance + ? WHERE name = ?")
            .bind(1, 30.0)
            .bind(2, std::string("B"))
            .run();
        // 故意制造失败：违反 CHECK(balance >= 0)
        Stmt(db.raw(), "INSERT INTO account(name,balance) VALUES(?,?)")
            .bind(1, std::string("bad"))
            .bind(2, -1.0)
            .run();
        tx.commit(); // 永远到不了这里
        std::printf("（如果看到这行说明异常没抛出，代码有问题）\n");
    }
    catch (const SqliteError &e)
    {
        std::printf("捕获异常：%s\n", e.what());
        std::printf("  → 异常抛出后 Transaction 析构自动 ROLLBACK，不需要任何手动清理。\n");
    }
    show("[回滚后余额]");

    const auto rows = db.query_scalar_int("SELECT COUNT(*) FROM account").value_or(-1);
    std::printf("账户数=%lld（应为 2，脏数据没留下来）\n", rows);

    // 唯一约束演示：约束冲突的错误码怎么拿
    std::printf("\n---------------- 实测：约束冲突的错误码 ----------------\n");
    db.exec("CREATE UNIQUE INDEX ux_account_name ON account(name)");
    try
    {
        Stmt(db.raw(), "INSERT INTO account(name,balance) VALUES(?,?)").bind(1, std::string("A")).bind(2, 1.0).run();
    }
    catch (const SqliteError &e)
    {
        std::printf("捕获：%s\n", e.what());
        std::printf("  rc=%d（SQLITE_CONSTRAINT=%d），extended=%d → 上层可据此给用户\"重名\"提示。\n",
                    e.rc(), SQLITE_CONSTRAINT, e.extended_rc());
    }

    std::remove(path);
    std::remove("02_txn_demo.db-journal");
}

static void demo_type_and_null(const char *path)
{
    std::printf("\n---------------- 实测：类型绑定 / NULL / get<T> ----------------\n");
    std::remove(path);
    Db db(path);
    db.exec("CREATE TABLE t(i INTEGER, f REAL, s TEXT)");

    {
        Stmt st(db.raw(), "INSERT INTO t(i,f,s) VALUES(?,?,?)");
        st.bind(1, static_cast<int64_t>(42)).bind(2, 3.14).bind(3, std::string("你好，SQLite")).run();
        // 第二行：整数为 NULL
        st.bind_null(1).bind(2, 2.718).bind(3, std::string("f 有值 i 无值")).run();
        // 第三行：字符串为 NULL，演示 optional
        st.bind(1, static_cast<int64_t>(-7)).bind(2, -0.5).bind_opt(3, std::optional<std::string>{}).run();
    }

    Stmt q(db.raw(), "SELECT i, f, s FROM t ORDER BY rowid");
    for (auto row : q.fetch_all())
    {
        std::printf("行（%zu 列）: ", row.size());
        std::printf("i=%s ", row.is_null(0) ? "NULL" : std::to_string(row.get<int64_t>(0)).c_str());
        std::printf("f=%.3f ", row.get<double>(1));
        std::printf("s=%s", row.is_null(2) ? "NULL" : row.get<std::string>(2).c_str());
        std::printf("\n");
    }

    // 拿 NULL 去 get<T> 会怎样？——抛异常，不返回 0 蒙混过关。
    try
    {
        Stmt q2(db.raw(), "SELECT i FROM t WHERE f = 2.718");
        auto r = q2.next();
        if (r)
            std::printf("试着取 NULL 列：%d\n", r->get<int32_t>(0));
    }
    catch (const SqliteError &e)
    {
        std::printf("取 NULL 列被拦下：%s\n", e.what());
        std::printf("  → 这就是\"宁可开发期炸，不要线上静默出错\"。\n");
    }

    // 越界同样立刻报错
    try
    {
        Stmt q3(db.raw(), "SELECT i FROM t LIMIT 1");
        auto r = q3.next();
        if (r)
            std::ignore = r->get<int32_t>(9);
    }
    catch (const std::out_of_range &e)
    {
        std::printf("列下标越界被拦下：%s\n", e.what());
    }

    // exec_dml：受影响行数必须看到。
    const int deleted = db.exec_dml("DELETE FROM t WHERE i IS NULL");
    std::printf("exec_dml 删除 i IS NULL 的行，受影响 %d 行（这个 0/非 0 就是业务答案）\n", deleted);
    const int again = db.exec_dml("DELETE FROM t WHERE i = 999999");
    std::printf("再删一个不存在的 key，受影响 %d 行 → 上层要知道\"什么都没改\"\n", again);

    std::remove(path);
}

// ===========================================================================
// 八、查询：导完数据总得有人读
// ===========================================================================
//
// 这一段演示三件事：
//   1. 单事务导入 1 万条（这是"能进生产"的写法）；
//   2. 无索引的范围/等值查询 vs 建索引后的差距 —— 全表扫描是 O(N)；
//   3. 聚合 + 分组查询用 fetch_all 取回、按 C++ 的方式处理结果。
// 数据库慢，九成是"没建索引"和"在循环里查库（N+1）"，这里都摸一遍。
// ===========================================================================
static void demo_import_and_query(const char *path, const int N)
{
    std::printf("\n---------------- 实测：批量导入 + 查询（N=%d）----------------\n", N);
    std::remove(path);
    std::remove("02_shop.db-journal");
    Db db(path);
    db.exec("CREATE TABLE orders(id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL,"
            " amount REAL NOT NULL, memo TEXT, created_seq INTEGER NOT NULL)");

    // 导入：一个事务 + 复用同一条语句。这就是生产写法。
    // user_id 故意只取 0..(K-1)：这样每人有多条订单，
    // 分组统计和索引查询才有意义（不然每组就一行，看不出差别）。
    const int kUsers = 50;
    Timer t;
    {
        Transaction tx(db, Transaction::Kind::Immediate);
        Stmt st(db.raw(), "INSERT INTO orders(user_id,amount,memo,created_seq) VALUES(?,?,?,?)");
        for (int i = 0; i < N; ++i)
        {
            st.bind(1, static_cast<int64_t>(i % kUsers))
                .bind(2, 1.5 + (i % 97) * 0.25)
                .bind(3, std::string("SKU-") + std::to_string(i % 500))
                .bind(4, static_cast<int32_t>(i));
            st.run();
        }
        tx.commit();
    }
    const double import_ms = t.ms();
    std::printf("批量事务导入 %d 行，耗时 %.1f ms（%.1f 万行/秒）\n", N, import_ms,
                N / import_ms);

    const auto total = db.query_scalar_int("SELECT COUNT(*) FROM orders").value_or(-1);
    std::printf("表内行数：%lld（自增主键 id 从 1 开始，验证导入完整）\n", static_cast<long long>(total));

    // ---- 无索引的等值查询 ----
    Timer q;
    const auto hit1 = db.query_scalar_int("SELECT COUNT(*) FROM orders WHERE user_id = 777").value_or(-1);
    const double no_index_ms = q.ms();
    std::printf("无索引 user_id=777 计数：%lld 行，耗时 %.2f ms（全表扫描）\n",
                static_cast<long long>(hit1), no_index_ms);

    // ---- 建索引 ----
    Timer ix;
    db.exec("CREATE INDEX idx_orders_user ON orders(user_id)");
    std::printf("建索引 idx_orders_user 耗时 %.1f ms（索引不是免费的，写放大就发生在这）\n", ix.ms());

    Timer q2;
    const auto hit2 = db.query_scalar_int("SELECT COUNT(*) FROM orders WHERE user_id = 777").value_or(-1);
    const double with_index_ms = q2.ms();
    std::printf("有索引 user_id=777 计数：%lld 行，耗时 %.2f ms", static_cast<long long>(hit2), with_index_ms);
    if (with_index_ms > 0.0)
        std::printf("，快 %.1f 倍\n", no_index_ms / with_index_ms);
    else
        std::printf("（快到计时精度以下了）\n");
    if (hit1 != hit2)
        std::printf("  ⚠ 结果不一致！索引不该改变结果，说明有问题。\n");

    // ---- 聚合 + 分组：取 Top 5 消费用户 ----
    std::printf("\n消费 Top 5 用户（GROUP BY + ORDER BY + LIMIT）：\n");
    Stmt agg(db.raw(),
             "SELECT user_id, COUNT(*) AS cnt, SUM(amount) AS total "
             "FROM orders GROUP BY user_id ORDER BY total DESC LIMIT 5");
    std::printf("  %-10s %-8s %-12s\n", "user_id", "订单数", "总金额");
    for (const auto &row : agg.fetch_all())
    {
        // 注意顺序：列的下标跟着 SELECT 的书写顺序走，
        // 改 SQL 时忘了改下标是经典事故。字段多的时候按名字取更稳。
        std::printf("  %-10lld %-8lld %-12.2f\n",
                    static_cast<long long>(row.get<int64_t>(0)),
                    static_cast<long long>(row.get<int64_t>(1)),
                    row.get<double>(2));
    }

    // ---- N+1 反面教材 vs 一条 JOIN 搞定 ----
    std::printf("\nN+1 反面教材（在循环里查库）：\n");
    Timer n1;
    long long sum_a = 0;
    {
        Stmt users(db.raw(), "SELECT DISTINCT user_id FROM orders ORDER BY user_id LIMIT 50");
        auto uids = users.fetch_all(); // 先取出来！边遍历语句边用同连接查另一条语句要小心
        Stmt per(db.raw(), "SELECT SUM(amount) FROM orders WHERE user_id = ?");
        for (const auto &r : uids)
        {
            const int64_t uid = r.get<int64_t>(0);
            per.rewind(); // 上一轮只 step 了一行就走人，必须复位再绑，否则 SQLITE_MISUSE
            per.bind(1, uid);
            auto one = per.next();
            if (one && !one->is_null(0))
                sum_a += static_cast<long long>(one->get<double>(0));
        }
        per.rewind();
    }
    const double n1_ms = n1.ms();

    Timer j;
    long long sum_b = 0;
    {
        Stmt one(db.raw(),
                 "SELECT user_id, SUM(amount) FROM orders GROUP BY user_id "
                 "HAVING user_id IN (SELECT DISTINCT user_id FROM orders ORDER BY user_id LIMIT 50)");
        for (const auto &r : one.fetch_all())
            if (!r.is_null(1))
                sum_b += static_cast<long long>(r.get<double>(1));
    }
    const double join_ms = j.ms();

    std::printf("  循环单查 50 次 ：%.2f ms（结果 %lld）\n", n1_ms, sum_a);
    std::printf("  一条聚合查询    ：%.2f ms（结果 %lld）\n", join_ms, sum_b);
    std::printf("  → 差距可能不大，但每次往返都要付\"往返成本\"：解析、加锁、系统调用。\n"
                "    线上真数据库是跨网络的，一次往返 0.5~几毫秒，N+1 能要命。\n");

    std::printf("\n库文件最终大小：%lld 字节\n", file_size_of(path));
    std::remove(path);
    std::remove("02_shop.db-journal");
}

// Db 的 move 语义演示：把连接"交给"另一个函数处理。
static int64_t use_db_elsewhere(Db db)
{
    // 连接所有权随移动进了这个函数，函数结束时析构 → 自动 close。
    // 调用方不需要、也没能力去 close，这就堵死了"谁来关"的扯皮。
    return db.query_scalar_int("SELECT 1 + 1").value_or(-1);
}

int main(int argc, char **argv)
{
    setup_utf8_console();

    // 可选参数：数据条数。默认 10000（师傅要求的量级）。
    // 留这个口子是因为"逐条提交"这一项在本机要跑一分多钟，
    // 学徒想快速看别的实验时可以给个小 N；但正式结论请用 10000。
    int N = 10000;
    if (argc > 1)
    {
        const int v = std::atoi(argv[1]);
        if (v > 0)
            N = v;
    }

    std::printf("\n================================================================\n");
    std::printf("  02 · 用 RAII 把 SQLite C API 包装成生产级 C++ 接口\n");
    std::printf("================================================================\n");

    std::printf("\n---------------- 组件自检 ----------------\n");
    {
        Db db(":memory:");
        std::printf("Db 构造成功（内存库），扩展错误码已开、busy_timeout=5000ms\n");
        std::printf("SELECT 1+1 → %lld（所有权移动进函数，出来自动关连接）\n",
                    static_cast<long long>(use_db_elsewhere(std::move(db))));
        // 验证 move-only：下面两行如果取消注释，编译期就该报错。
        // Db a(":memory:"); Db b(a);       // error: 拷贝构造被 delete
        // Db c(":memory:"); c = a;         // error: 拷贝赋值被 delete
        std::printf("Db / Stmt / Transaction 均 delete 了拷贝、只留移动 → 双重释放编译期不可能。\n");
    }

    demo_type_and_null("02_types.db");
    demo_transaction_exception_safety("02_txn_demo.db");
    demo_import_and_query("02_shop.db", N); // 生产写法：批量事务导入 + 查询
    demo_batch_vs_transaction("02_orders.db", N);

    std::printf("\n================================================================\n");
    std::printf("  师傅总结\n");
    std::printf("================================================================\n");
    std::printf("1. 资源交给对象管：Db 关连接、Stmt 做 finalize、Transaction 管提交/回滚。\n"
                "   人只负责创建，不负责收尾——收尾是编译器的事。\n");
    std::printf("2. move-only 不是炫技，是把\"双重释放\"从运行时 bug 降级成编译错误。\n");
    std::printf("3. 析构函数里不要抛异常（会 terminate），只能打日志；\n"
                "   所以\"提交失败\"必须在 commit() 里显式抛给上层，不能拖到析构。\n");
    std::printf("4. Row 必须拷贝数据。sqlite3_column_text 的指针属于 stmt，下次 step 就作废。\n");
    std::printf("5. 事务是性能第一开关：逐条提交和单事务提交差的不是一点半点，\n"
                "   是几十倍甚至上百倍。差多少，上面的毫秒数自己看。\n");
    std::printf("6. 批量大小要权衡：事务太大 = 失败代价大 + WAL 膨胀 + 阻塞 checkpoint。\n"
                "   500~5000 条一批是常见的起手值，然后拿真实负载压测再定。\n");
    std::printf("7. [[nodiscard]] 让\"忘记检查返回值\"编译不过。免费的安全，为什么不加？\n");
    return 0;
}
