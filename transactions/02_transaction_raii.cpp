/* =====================================================================
 *  02_transaction_raii.cpp
 *  第 3 关 · 心法期 —— C++ 端的事务工程实践：RAII + 重试
 *
 *  编译（Windows / MSYS2 g++ 15.2，无 CMake，单文件直出）：
 *      g++ -std=c++17 -O2 02_transaction_raii.cpp -o 02_transaction_raii.exe -lsqlite3
 *  运行：
 *      .\02_transaction_raii.exe
 *
 *  这一份要教会你一件事：
 *      "异常安全的事务边界" 不该靠人肉记得写 ROLLBACK，
 *      而应该像 std::lock_guard 一样 —— 离开作用域就自动处理干净。
 *
 *  对照表（你熟的 C++ 概念 ↔ 数据库概念）：
 *      std::lock_guard / std::scoped_lock   ↔  Transaction（构造 BEGIN，析构 COMMIT/ROLLBACK）
 *      栈展开 (stack unwinding)             ↔  异常触发的自动 ROLLBACK
 *      std::mutex 按 key 分片               ↔  行锁（04 号文件我们自己手搓一个）
 *      带超时的 try_lock                    ↔  busy_timeout / SQLITE_BUSY 重试
 *      写时复制 + 快照迭代器                ↔  MVCC 版本链 + read view（见 mvcc_animator.html）
 * ===================================================================== */

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

/* ---------------------------------------------------------------------
 *  小工具：带毫秒时间戳的日志，方便肉眼看出线程交错
 * ------------------------------------------------------------------- */
static auto g_t0 = std::chrono::steady_clock::now();

static long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now() - g_t0).count();
}

// tag 用左对齐 8 字符，多线程输出才能对齐成一列一列的时间线。
// 注意 printf 的 %-Ns 按字节算宽度，中文一个字 3 字节会错位，
// 所以这里自己按"显示宽度"（CJK 算 2 列）补齐。
static std::string padDisplay(const std::string& s, size_t width) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch < 0x80)      { i += 1; w += 1; }
        else if (ch < 0xE0) { i += 2; w += 1; }
        else if (ch < 0xF0) { i += 3; w += 2; }   // CJK 全角
        else                { i += 4; w += 2; }
    }
    return s + std::string(w < width ? width - w : 0, ' ');
}

static void log(const std::string& tag, const std::string& msg) {
    // 注意：printf 不是原子的，多线程下用一把打印锁保证一行不被撕开。
    // 这是"日志锁"，跟数据库事务无关，纯粹为了输出可读。
    static std::mutex printMtx;
    std::lock_guard<std::mutex> lk(printMtx);
    std::printf("[%7lld ms] %-*s %s\n", nowMs(), 12, padDisplay(tag, 12).c_str(), msg.c_str());
}

/* ---------------------------------------------------------------------
 *  SQLite 异常：把错误码 + errmsg 打包成 C++ 异常
 * ------------------------------------------------------------------- */
class SqlError : public std::runtime_error {
public:
    SqlError(int code, const std::string& msg, const std::string& what)
        : std::runtime_error(what + " : " + msg + " (code=" + std::to_string(code) + ")"),
          code_(code) {}
    int code() const noexcept { return code_; }

private:
    int code_;
};

// 判断是不是"暂时性冲突"——这类错误值得重试，而不是往上抛
static bool isRetryable(int code) {
    // SQLITE_BUSY      : 库被别的连接锁住（我们的主场景）
    // SQLITE_LOCKED    : 同一连接内表级锁冲突（比如 SAVEPOINT 冲突）
    // 注意 SQLITE_BUSY_SNAPSHOT 也归在 busy 家族（值 = 512 | 5）
    return code == SQLITE_BUSY || code == SQLITE_LOCKED;
}

/* ---------------------------------------------------------------------
 *  Conn：一个 SQLite 连接的 RAII 包装
 *  每个线程必须用自己的连接！sqlite3 默认序列化模式虽然能跨线程，
 *  但事务状态是 per-connection 的，跨线程共用连接 = 事务互相踩。
 * ------------------------------------------------------------------- */
class Conn {
public:
    explicit Conn(const std::string& path) {
        // OPEN_FULLMUTEX：让 SQLite 自己上序列化锁，我们图个安心
        int rc = sqlite3_open_v2(
            path.c_str(), &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            std::string m = db_ ? sqlite3_errmsg(db_) : "无法分配连接";
            if (db_) sqlite3_close(db_);
            throw SqlError(rc, m, "打开数据库失败");
        }
        // 关键设置①：外键是 per-connection 的，每个连接都得自己开
        exec("PRAGMA foreign_keys = ON");
        // 关键设置②：WAL 让"一写多读"成为现实（读者不阻塞写者）
        exec("PRAGMA journal_mode = WAL");
        // 关键设置③：busy_timeout = 0，故意让冲突立刻暴露，方便观察
        exec("PRAGMA busy_timeout = 0");
    }
    ~Conn() {
        if (db_) sqlite3_close(db_);
    }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    sqlite3* raw() noexcept { return db_; }

    // 执行不关心结果的语句；出错就抛
    void exec(const std::string& sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string m = err ? err : sqlite3_errmsg(db_);
            sqlite3_free(err);
            throw SqlError(rc, m, "执行失败: " + sql);
        }
    }

    // 查询单值（只取第一行第一列的整数），-1 表示无结果
    long long queryLong(const std::string& sql) {
        long long val = -1;
        auto cb = [](void* p, int argc, char** argv, char**) -> int {
            if (argc > 0 && argv[0]) *static_cast<long long*>(p) = std::atoll(argv[0]);
            return 0;  // 非 0 会让 sqlite3_exec 提前中止，这里只要第一行
        };
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql.c_str(), cb, &val, &err);
        if (rc != SQLITE_OK) {
            std::string m = err ? err : sqlite3_errmsg(db_);
            sqlite3_free(err);
            throw SqlError(rc, m, "查询失败: " + sql);
        }
        return val;
    }

private:
    sqlite3* db_ = nullptr;
};

/* ---------------------------------------------------------------------
 *  ★ 核心：Transaction —— RAII 事务守卫
 *
 *  语义（和 std::lock_guard 一模一样，只是把"锁"换成"事务"）：
 *      构造   → BEGIN（可指定 DEFERRED / IMMEDIATE / EXCLUSIVE）
 *      commit() → 显式提交，之后析构什么都不做
 *      析构   → 没 commit 过？那就 ROLLBACK（异常路径自动走到这里）
 *
 *  为什么"抛异常自动回滚"是免费的？
 *      因为 C++ 保证：栈展开时，栈上对象的析构函数一定被调用
 *      （除非有 noexcept 函数中间的 UB）。这就是 RAII 的全部魔力。
 *      你写 std::mutex 从来不需要"记得 unlock"，事务也一样。
 * ------------------------------------------------------------------- */
class Transaction {
public:
    enum class Kind { Deferred, Immediate, Exclusive };

    explicit Transaction(Conn& conn,
                         Kind kind = Kind::Deferred,
                         std::string label = "txn")
        : conn_(conn), label_(std::move(label)) {
        const char* kw = kind == Kind::Immediate   ? "BEGIN IMMEDIATE"
                         : kind == Kind::Exclusive ? "BEGIN EXCLUSIVE"
                                                   : "BEGIN";
        conn_.exec(kw);
        active_ = true;
        log(label_, std::string("BEGIN (") + (kind == Kind::Deferred ? "DEFERRED" :
                                               kind == Kind::Immediate ? "IMMEDIATE" : "EXCLUSIVE") + ")");
    }

    ~Transaction() {
        if (!active_) return;   // 已经 commit 过，安静退出
        // 走到这里 = 作用域退出但没 commit = 出错或提前 return
        try {
            conn_.exec("ROLLBACK");
            log(label_, "ROLLBACK（自动：未 commit 就离开作用域）");
        } catch (const std::exception& e) {
            // 析构函数里绝不能让异常逃逸，只能吞掉并记录
            log(label_, std::string("ROLLBACK 失败: ") + e.what());
        }
    }

    void commit() {
        if (!active_) throw std::logic_error(label_ + ": 事务已结束，重复 commit");
        conn_.exec("COMMIT");
        active_ = false;
        log(label_, "COMMIT");
    }

    void rollback() {
        if (!active_) return;
        conn_.exec("ROLLBACK");
        active_ = false;
        log(label_, "ROLLBACK（手动）");
    }

    // 部分回滚：保存点。适合"批处理里某条子操作失败，只切除这一段"
    void savepoint(const std::string& name) { conn_.exec("SAVEPOINT " + name); }
    void rollbackTo(const std::string& name) { conn_.exec("ROLLBACK TO " + name); }
    void release(const std::string& name)    { conn_.exec("RELEASE " + name); }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

private:
    Conn&       conn_;
    std::string label_;
    bool        active_ = false;
};

/* ---------------------------------------------------------------------
 *  ★ Retry 包装：把 SQLITE_BUSY 从"报错"变成"退避后重来"
 *
 *  指数退避 + 抖动，是所有"抢共享资源"场景的标准配方：
 *      sleep = base * 2^attempt (+ random jitter)
 *  为什么要有 jitter？两个线程同时撞墙后，若退避时长一样，
 *  它们会"同步地"再次同时撞墙 —— 这叫 convoy / 惊群，加抖动打散它。
 * ------------------------------------------------------------------- */
struct RetryPolicy {
    int  maxAttempts = 8;
    std::chrono::milliseconds baseDelay{5};
    std::chrono::milliseconds maxDelay{200};
};

struct RetryOutcome {
    bool      succeeded = false;
    int       attempts  = 0;
    int       lastCode  = SQLITE_OK;
    std::string lastError;
};

// fn 需要是可重入的：每次重试都从头再执行一遍完整事务。
// 这一点极其重要 —— 重试的单位必须是"整个事务"，不能是事务中间某条语句。
static RetryOutcome runWithRetry(const RetryPolicy& policy,
                                 const std::string& tag,
                                 const std::function<void()>& fn) {
    RetryOutcome out;
    unsigned seed = 1;   // 固定抖动种子，让输出可复现
    for (int attempt = 1; attempt <= policy.maxAttempts; ++attempt) {
        out.attempts = attempt;
        try {
            fn();
            out.succeeded = true;
            return out;
        } catch (const SqlError& e) {
            out.lastCode = e.code();
            out.lastError = e.what();
            if (!isRetryable(e.code())) {
                log(tag, std::string("不可重试的错误，直接上抛：") + e.what());
                throw;                       // 业务错误（约束冲突等）重试没意义
            }
            if (attempt == policy.maxAttempts) break;
            auto delay = policy.baseDelay * (1 << (attempt - 1));
            if (delay > policy.maxDelay) delay = policy.maxDelay;
            // 极简易抖动：拿 xorshift 打散一点
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            auto jitter = std::chrono::milliseconds(seed % 7);
            log(tag, "SQLITE_BUSY，第 " + std::to_string(attempt) + " 次重试，退避 " +
                     std::to_string((delay + jitter).count()) + " ms");
            std::this_thread::sleep_for(delay + jitter);
        }
    }
    log(tag, "重试 " + std::to_string(out.attempts) + " 次仍失败：" + out.lastError);
    return out;
}

/* ---------------------------------------------------------------------
 *  建库、建表
 * ------------------------------------------------------------------- */
static const char* kDbFile = "txn_lab.db";

static void setupSchema() {
    Conn c(kDbFile);
    c.exec("DROP TABLE IF EXISTS account");
    c.exec(R"(CREATE TABLE account (
                id      INTEGER PRIMARY KEY,
                name    TEXT    NOT NULL,
                balance INTEGER NOT NULL CHECK (balance >= 0),
                version INTEGER NOT NULL DEFAULT 0   -- 乐观锁用的版本号，03 号文件要用它
             ))");
    c.exec("INSERT INTO account(id,name,balance,version) VALUES(1,'张三',1000,0),(2,'李四',500,0)");
    log("setup", "表已建好：张三 1000，李四 500");
}

static long long balanceOf(int id) {
    Conn c(kDbFile);
    return c.queryLong("SELECT balance FROM account WHERE id=" + std::to_string(id));
}

/* ---------------------------------------------------------------------
 *  演示 1：正常提交
 * ------------------------------------------------------------------- */
static void demo_NormalCommit() {
    std::printf("\n----- 演示 1：正常路径（作用域退出自动 COMMIT）-----\n");
    Conn c(kDbFile);
    {
        // 明知要写 → 用 IMMEDIATE，开局就抢写锁，避免"升级死锁"
        Transaction txn(c, Transaction::Kind::Immediate, "转账");
        c.exec("UPDATE account SET balance = balance - 200, version = version + 1 WHERE id = 1");
        c.exec("UPDATE account SET balance = balance + 200, version = version + 1 WHERE id = 2");
        txn.commit();   // 忘了写会怎样？答案在演示 2
    }
    std::printf("        结果：张三=%lld 李四=%lld（和=1500，守恒）\n",
                balanceOf(1), balanceOf(2));
}

/* ---------------------------------------------------------------------
 *  演示 2：中途抛异常 → 栈展开 → 析构里自动 ROLLBACK
 *  这里故意模拟"扣了钱，加钱那步业务校验失败"
 * ------------------------------------------------------------------- */
static void demo_ExceptionRollback() {
    std::printf("\n----- 演示 2：异常路径（抛异常自动 ROLLBACK）-----\n");
    Conn c(kDbFile);
    const long long before1 = balanceOf(1), before2 = balanceOf(2);
    try {
        Transaction txn(c, Transaction::Kind::Immediate, "失败转账");
        c.exec("UPDATE account SET balance = balance - 300, version = version + 1 WHERE id = 1");
        log("失败转账", "扣款完成，接下来模拟业务失败……");
        throw std::runtime_error("风控拦截：该笔转账触发反洗钱规则");
        // txn.commit() 永远执行不到
    } catch (const std::exception& e) {
        log("捕获", std::string("业务异常：") + e.what());
    }
    const long long after1 = balanceOf(1), after2 = balanceOf(2);
    std::printf("        异常前：张三=%lld 李四=%lld\n", before1, before2);
    std::printf("        异常后：张三=%lld 李四=%lld  → %s\n",
                after1, after2,
                (after1 == before1 && after2 == before2) ? "完全回滚 ✔" : "有残留 ✘（BUG！）");

    // 反面对照：如果这里用的是"手写 BEGIN/COMMIT"，异常一抛就没人执行 ROLLBACK，
    // 连接上会残留一个打开的事务；连接关闭时才会被隐式回滚，
    // 而如果是连接池复用，下一个拿到这个连接的人就摊上了你的半成品事务。
    // 这就是为什么 RAII 不是"代码洁癖"，是"防止状态泄漏"。
}

/* ---------------------------------------------------------------------
 *  演示 3：RETURN / 提前退出也自动回滚
 * ------------------------------------------------------------------- */
// 这个函数只有一件事：在余额不足时"直接 return"，
// 看它 return 之后 Transaction 的析构有没有把 UPDATE 擦干净。
static bool tryDebit() {
    Conn c(kDbFile);
    Transaction txn(c, Transaction::Kind::Immediate, "提前退出");
    c.exec("UPDATE account SET balance = balance - 1, version = version + 1 WHERE id = 1");
    // 注意：这里用同一个连接读，才能看到"自己事务内的写"（99）；
    // 换成另一个连接读，看到的仍是旧值 100 —— 这就是隔离性。
    const long long mine = c.queryLong("SELECT balance FROM account WHERE id = 1");
    std::printf("        事务内自己看到的余额=%lld\n", mine);
    if (mine < 500) {
        std::printf("        余额不足 → 直接 return（没有手写 ROLLBACK！）\n");
        return false;   // txn 析构 → ROLLBACK
    }
    txn.commit();
    return true;
}

static void demo_EarlyReturn() {
    std::printf("\n----- 演示 3：提前 return（也一样自动回滚）-----\n");
    Conn c(kDbFile);
    c.exec("UPDATE account SET balance = 100 WHERE id = 1");   // 复位到 100，保证走早退分支
    std::printf("        进入前余额=%lld\n", balanceOf(1));
    bool ok = tryDebit();
    std::printf("        调用返回 ok=%d，库里的余额=%lld（-1 被析构里的 ROLLBACK 撤销）\n",
                ok ? 1 : 0, balanceOf(1));
}

/* ---------------------------------------------------------------------
 *  演示 4：保存点部分回滚（C++ 版）
 * ------------------------------------------------------------------- */
static void demo_Savepoint() {
    std::printf("\n----- 演示 4：SAVEPOINT 精确切除失败子操作 -----\n");
    Conn c(kDbFile);
    {
        Transaction txn(c, Transaction::Kind::Immediate, "批处理");
        c.exec("UPDATE account SET balance = balance + 100, version = version + 1 WHERE id = 1");

        txn.savepoint("sp");
        try {
            c.exec("UPDATE account SET balance = -1 WHERE id = 2");   // 违反 CHECK(balance>=0)
        } catch (const SqlError& e) {
            std::printf("        子操作失败（预期）：%s\n", e.what());
            txn.rollbackTo("sp");
            txn.release("sp");
        }
        c.exec("UPDATE account SET balance = balance + 1, version = version + 1 WHERE id = 2");
        txn.commit();
    }
    std::printf("        结果：张三=%lld（+100 保住）李四=%lld（-1 被切除，只 +1）\n",
                balanceOf(1), balanceOf(2));
}

/* ---------------------------------------------------------------------
 *  演示 5：两个线程写同一行 —— 真竞争，不是摆拍
 *  两边 busy_timeout 都是 0，所以后到的那个会立刻拿到 SQLITE_BUSY。
 *  为了把窗口拉大（0.2ms 的临界区太快），我们在事务里插一段纯计算，
 *  模拟"事务里做了点别的事"，这也顺带说明为什么长事务危险。
 * ------------------------------------------------------------------- */
static void busyWork(int us) {
    // 用自旋而不是 sleep：sleep 在 Windows 上粒度约 1~15ms，太粗；
    // 自旋能拿到亚毫秒级、可控的持锁时长。
    auto start = std::chrono::steady_clock::now();
    volatile long long sink = 0;
    while (std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start).count() < us) {
        ++sink;
    }
}

static void demo_WriteWriteConflict() {
    std::printf("\n----- 演示 5：两个线程并发写同一行（观察 SQLITE_BUSY）-----\n");
    Conn c(kDbFile);
    c.exec("UPDATE account SET balance = 1000 WHERE id = 1");   // 复位，方便看增量
    std::printf("        初始 张三=%lld\n", balanceOf(1));

    // 栅栏：两个线程在同一瞬间起跑，最大化交错概率
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> busyCount{0}, okCount{0};

    auto worker = [&](const std::string& tag, int delta) {
        // 每个线程必须独立连接！共用 Conn 会让两个事务互相踩状态。
        try {
            Conn c2(kDbFile);
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();   // 自旋等待发令枪

            Transaction txn(c2, Transaction::Kind::Immediate, tag);
            c2.exec("UPDATE account SET balance = balance + (" + std::to_string(delta) +
                    "), version = version + 1 WHERE id = 1");
            busyWork(1500);   // 持锁 1.5ms，给另一个线程制造撞墙窗口
            txn.commit();
            okCount.fetch_add(1);
        } catch (const SqlError& e) {
            if (isRetryable(e.code())) {
                busyCount.fetch_add(1);
                log(tag, std::string("撞上 SQLITE_BUSY：") + e.what());
            } else {
                log(tag, std::string("其它错误：") + e.what());
            }
        }
    };

    std::thread t1(worker, "写者A", +10), t2(worker, "写者B", +20);
    while (ready.load() < 2) std::this_thread::yield();
    go.store(true);   // 发令
    t1.join(); t2.join();

    std::printf("        成功 %d 个，撞上 BUSY %d 个，最终余额=%lld（期望 %d 或 %d，取决于谁被拒）\n",
                okCount.load(), busyCount.load(), balanceOf(1), 1000 + 10, 1010 + 20);

    // —— 上面就是"没做重试"的世界：有一个事务被拒了，业务得自己兜。
    // —— 下面是"用 runWithRetry"的世界：被拒的那个自动重来，最终两个都成功。
    std::printf("\n        换成 runWithRetry 再跑一遍：\n");
    c.exec("UPDATE account SET balance = 1000 WHERE id = 1");

    RetryPolicy policy;
    policy.maxAttempts = 12;
    policy.baseDelay = std::chrono::milliseconds(2);

    auto workerRetry = [&](const std::string& tag, int delta) {
        Conn c2(kDbFile);
        // 把"整个事务"作为重试单位 —— 注意 fn 里每次都新开一个 Transaction
        RetryOutcome r = runWithRetry(policy, tag, [&] {
            Transaction txn(c2, Transaction::Kind::Immediate, tag);
            c2.exec("UPDATE account SET balance = balance + (" + std::to_string(delta) +
                    "), version = version + 1 WHERE id = 1");
            busyWork(1500);
            txn.commit();
        });
        std::printf("        %s：succeeded=%d，尝试 %d 次\n", tag.c_str(),
                    r.succeeded ? 1 : 0, r.attempts);
    };

    std::atomic<int> ready2{0};
    std::atomic<bool> go2{false};
    auto w2 = [&](std::string tag, int delta) {
        Conn c2(kDbFile);
        ready2.fetch_add(1);
        while (!go2.load()) std::this_thread::yield();
        workerRetry(tag, delta);
    };
    std::thread a(w2, "写者A", +10), b(w2, "写者B", +20);
    while (ready2.load() < 2) std::this_thread::yield();
    go2.store(true);
    a.join(); b.join();
    std::printf("        最终余额=%lld（期望 1030 = 两个都成功）\n", balanceOf(1));
}

/* =====================================================================
 *  main
 * ===================================================================== */
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 关缓冲，多线程日志才实时
    std::printf("=====================================================================\n");
    std::printf(" 事务的 RAII 工程实践 —— 让 ROLLBACK 像 unlock 一样自动化\n");
    std::printf(" SQLite %s\n", sqlite3_libversion());
    std::printf("=====================================================================\n");

    setupSchema();
    demo_NormalCommit();
    demo_ExceptionRollback();
    demo_EarlyReturn();
    demo_Savepoint();
    demo_WriteWriteConflict();

    std::printf("\n=====================================================================\n");
    std::printf(" 老师傅的话\n");
    std::printf("---------------------------------------------------------------------\n");
    std::printf(" 1. 记住这条等价关系：Transaction 之于事务，就是\n");
    std::printf("    std::scoped_lock 之于互斥量。凡是\"成对操作\"都该 RAII 化，\n");
    std::printf("    人肉配对早晚漏一次，漏的那次就是线上事故。\n");
    std::printf(" 2. 析构函数里绝对不能让异常逃逸。所以上面 ROLLBACK 用了 try/catch\n");
    std::printf("    包起来。C++11 起析构默认 noexcept，你抛出去就是 std::terminate。\n");
    std::printf(" 3. commit() 之后要把 active_ 置 false，否则析构会再 ROLLBACK 一次，\n");
    std::printf("    轻则报错，重则把别人的事务回滚了（同连接上）。这叫\"幂等收尾\"。\n");
    std::printf(" 4. 重试的单位是\"整个事务\"，不是事务里的某条语句。这一点想不通，\n");
    std::printf("    你就会写出\"只重试第二条 UPDATE\"的怪物 —— 那已经破坏原子性了。\n");
    std::printf(" 5. 只重试暂时性错误（SQLITE_BUSY / SQLITE_LOCKED / 死锁）。约束冲突、\n");
    std::printf("    语法错误、类型不匹配，重试一万次也是错，往上抛给调用方。\n");
    std::printf(" 6. 退避要加抖动。没有抖动的指数退避会让所有失败者同步重试，\n");
    std::printf("    形成周期性冲击 —— 你在 C++ 里管这叫惊群，在云服务里这叫 thundering herd。\n");
    std::printf(" 7. SQLite 的写锁是库级的，所以\"一个写者\"是硬约束。要真正的行级并发，\n");
    std::printf("    去看 MySQL/PostgreSQL；但\"事务边界怎么管\"这套功夫是通用的。\n");
    std::printf("=====================================================================\n");

    std::printf("\n【练习】\n");
    std::printf(" 1. 把 Transaction 的 Kind::Immediate 改成 Deferred，重跑演示 5，\n");
    std::printf("    看错误码从 SQLITE_BUSY 变成什么（提示：SQLITE_BUSY_SNAPSHOT）。\n");
    std::printf(" 2. 给 Transaction 加一个移动构造，让它能从工厂函数返回：\n");
    std::printf("        auto txn = beginTxn(conn);   // 返回值优化 + 移动语义\n");
    std::printf("    记得把拷贝构造/赋值 delete 掉，并在移动后把源对象的 active_ 置 false，\n");
    std::printf("    否则源对象析构会提前 ROLLBACK —— 这是最经典的 RAII 移动语义 bug。\n");
    std::printf(" 3. 把 busyWork 从 1500us 改成 0，重跑演示 5，看 BUSY 次数是不是变少了。\n");
    std::printf("    由此理解：\"事务越长，冲突概率越高\"不是玄学，是可以量级的。\n");
    std::printf(" 4. 加一个 RAII 类 Savepoint，构造时 SAVEPOINT、析构时 RELEASE，\n");
    std::printf("    异常时 ROLLBACK TO —— 把演示 4 的 try/catch 也 RAII 化。\n");
    std::printf(" 5. 跑完记得删掉 txn_lab.db / -wal / -shm（程序末尾已自动删，\n");
    std::printf("    中途 Ctrl+C 可能留下，PowerShell：Remove-Item txn_lab.db* -ErrorAction SilentlyContinue）\n");

    // 清理实验产物，不给学徒留一地垃圾
    sqlite3_close_v2(nullptr);
    std::remove(kDbFile);
    std::remove("txn_lab.db-wal");
    std::remove("txn_lab.db-shm");
    std::printf("\n[cleanup] 已删除 txn_lab.db / -wal / -shm\n");
    return 0;
}
