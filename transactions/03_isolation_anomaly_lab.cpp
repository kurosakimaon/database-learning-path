/* =====================================================================
 *  03_isolation_anomaly_lab.cpp   ★ 本模块最有价值的一份 ★
 *  第 3 关 · 心法期 —— 并发异常实验室：亲眼看脏读 / 不可重复读 / 丢失更新 / 写写冲突
 *
 *  编译（Windows / MSYS2 g++ 15.2，没有 CMake，单文件直出）：
 *      g++ -std=c++17 -O2 03_isolation_anomaly_lab.cpp -o 03_isolation_anomaly_lab.exe -lsqlite3
 *  运行：
 *      .\03_isolation_anomaly_lab.exe
 *
 *  师傅把话说明白：
 *      隔离级别这东西，背十遍表格不如看一次交错。
 *      下面每个实验都是真的两个 std::thread、真的两条 sqlite3 连接、真的同一个 db 文件。
 *      为了让肉眼能看清交错顺序，前几个实验用「命名会合点」把步骤卡成 A-B-A-B；
 *      丢失更新那个实验的第二段则完全放开，300 次真竞争跑统计 —— 那是真打，
 *      丢了多少次就是多少次，糊弄不了你。
 *
 *  C++ 概念对照表（本文件反复用）：
 *      行锁            ≈  按 key 分片的 std::mutex（04 号文件手搓一个完整的）
 *      死锁检测        ≈  在资源等待图上找环（04 号文件）
 *      MVCC 版本链     ≈  写时复制 + 快照迭代器：读时拿一个不可变快照往下走
 *      RC vs RR        ≈  快照建立时机的差别：RC 每条语句重建，RR 事务开始就固定
 *      乐观锁 version  ≈  std::atomic::compare_exchange_strong 的数据库版
 *      悲观锁 FOR UPDATE ≈  先 lock() 再干活
 *      rendezvous      ≈  std::barrier（C++20）/ std::latch，这里手搓一个命名版
 * ===================================================================== */

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

/* =====================================================================
 *  基础设施 1：带毫秒时间戳的日志
 * ===================================================================== */
static auto g_t0 = std::chrono::steady_clock::now();
static long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now() - g_t0).count();
}

// printf 的 %-Ns 按字节算宽度，UTF-8 中文一个字 3 字节 / 2 列宽，会错位，所以手工补齐
static std::string padDisplay(const std::string& s, size_t width) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch < 0x80)      { i += 1; w += 1; }
        else if (ch < 0xE0) { i += 2; w += 1; }
        else if (ch < 0xF0) { i += 3; w += 2; }
        else                { i += 4; w += 2; }
    }
    return s + std::string(w < width ? width - w : 0, ' ');
}

static void log(const std::string& tag, const std::string& msg) {
    static std::mutex printMtx;
    std::lock_guard<std::mutex> lk(printMtx);
    std::printf("[%7lld ms] %-*s | %s\n", nowMs(), 16, padDisplay(tag, 16).c_str(), msg.c_str());
}

/* =====================================================================
 *  基础设施 2：Rendezvous —— 命名会合点（手搓版 std::barrier）
 *
 *  sync(name, n)：到齐 n 个线程才一起放行。
 *  mark/wait   ：单向信号，A 标记，B 等它。
 *
 *  这不是作弊。两条连接、两条线程、真实 SQL、真实锁，全是真的；
 *  我们只是把「谁先走哪一步」变得可复现 —— 0.05ms 的交错靠肉眼抓不住。
 *  想看完全失控的真竞争，去实验 C 的第二段，那里一个同步都不加。
 * ===================================================================== */
class Rendezvous {
public:
    void sync(const std::string& name, int n) {
        std::unique_lock<std::mutex> lk(m_);
        if (++cnt_[name] >= n) {                 // 最后一个到的负责放行
            cv_.notify_all();
        } else {
            cv_.wait(lk, [&] { return cnt_[name] >= n; });
        }
    }
    void mark(const std::string& evt) {
        std::lock_guard<std::mutex> lk(m_);
        flags_[evt] = true;
        cv_.notify_all();
    }
    void wait(const std::string& evt) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return flags_.count(evt) > 0; });
    }
    void reset() {
        std::lock_guard<std::mutex> lk(m_);
        cnt_.clear();
        flags_.clear();
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::string, int> cnt_;
    std::map<std::string, bool> flags_;
};

/* =====================================================================
 *  基础设施 3：极简 RAII 连接
 * ===================================================================== */
class Db {
public:
    enum class Mode { Normal, SharedCache };

    explicit Db(const char* path, Mode mode = Mode::Normal) {
        int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        flags |= (mode == Mode::SharedCache) ? SQLITE_OPEN_SHAREDCACHE : SQLITE_OPEN_PRIVATECACHE;
        // 坑：FULLMUTEX 的连接会让 shared-cache 进入"排他表锁"模式，
        // 那个模式下 read_uncommitted 直接失效，脏读怎么都复现不出来。
        // 所以共享缓存连接【故意不加】FULLMUTEX，与探针程序保持同一组条件。
        if (mode == Mode::Normal) flags |= SQLITE_OPEN_FULLMUTEX;
        int rc = sqlite3_open_v2(path, &db_, flags, nullptr);
        if (rc != SQLITE_OK) throw std::runtime_error(std::string("open 失败: ") + sqlite3_errmsg(db_));
    }
    ~Db() { if (db_) sqlite3_close(db_); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    sqlite3* raw() noexcept { return db_; }

    // 执行但不抛异常，把错误码交回调用方 —— 实验里我们就是要看错误码
    int tryExec(const std::string& sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
        lastError_ = err ? err : "";
        sqlite3_free(err);
        return rc;
    }
    void exec(const std::string& sql) {
        int rc = tryExec(sql);
        if (rc != SQLITE_OK) throw std::runtime_error("SQL 失败[" + sql + "]: " + lastError_);
    }
    long long queryLong(const std::string& sql, long long fallback = -1) {
        long long v = fallback;
        auto cb = [](void* p, int argc, char** argv, char**) -> int {
            if (argc > 0 && argv[0]) *static_cast<long long*>(p) = std::atoll(argv[0]);
            return 0;
        };
        char* err = nullptr;
        sqlite3_exec(db_, sql.c_str(), cb, &v, &err);
        sqlite3_free(err);
        return v;
    }
    const std::string& lastError() const noexcept { return lastError_; }

private:
    sqlite3* db_ = nullptr;
    std::string lastError_;
};

static const char* kLab = "iso_lab.db";

// 注意返回 std::string 而不是 const char*：带码分支要拼字符串，
// 返回 const char* 会给出一个临时对象的悬垂指针 —— 这是真 bug，别学。
static std::string rcName(int rc) {
    switch (rc & 0xFF) {
        case SQLITE_OK:         return "SQLITE_OK";
        case SQLITE_BUSY:       return "SQLITE_BUSY";
        case SQLITE_LOCKED:     return "SQLITE_LOCKED";
        case SQLITE_CONSTRAINT: return "SQLITE_CONSTRAINT";
        default:                return "SQLITE_ERR(" + std::to_string(rc) + ")";
    }
}

// busy_timeout 显式设 0：让锁冲突立刻暴露，别被"隐式等待"糊过去
static void noWait(Db& d) { d.exec("PRAGMA busy_timeout = 0"); }

// 遇 BUSY/LOCKED 就重试，直到拿到结果。
// 记住这条铁律：SQLITE_BUSY 不是"失败"，是"待会儿再来"。
// 吞掉它 = 悄悄丢一次更新，这比丢一次更新更可怕，因为你连日志都看不到。
static int execRetry(Db& d, const std::string& sql, std::atomic<long long>* retries) {
    for (int i = 0; i < 100000; ++i) {
        int rc = d.tryExec(sql);
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) return rc;
        if (retries) retries->fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
    }
    return SQLITE_BUSY;
}

// 自旋式持锁模拟。不用 sleep_for：Windows 上 sleep 粒度约 1~15ms，太粗，
// 自旋能拿到稳定的亚毫秒级时长。
static void holdMs(int ms) {
    auto t = std::chrono::steady_clock::now();
    volatile long long sink = 0;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t).count() < ms) { ++sink; }
}

static void resetTable(Db& d, long long v) {
    d.exec("DROP TABLE IF EXISTS t");
    d.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER NOT NULL, version INTEGER NOT NULL DEFAULT 0)");
    d.exec("INSERT INTO t(id, v, version) VALUES(1, " + std::to_string(v) + ", 0)");
}

// 取两列的小工具（SELECT balance, version ...）
static std::pair<long long, long long> queryPair(Db& d, const std::string& sql) {
    auto cb = [](void* p, int argc, char** argv, char**) -> int {
        auto* o = static_cast<long long*>(p);
        if (argc > 0) o[0] = argv[0] ? std::atoll(argv[0]) : -1;
        if (argc > 1) o[1] = argv[1] ? std::atoll(argv[1]) : -1;
        return 0;
    };
    long long out[2] = {-1, -1};
    char* err = nullptr;
    sqlite3_exec(d.raw(), sql.c_str(), cb, out, &err);
    sqlite3_free(err);
    return {out[0], out[1]};
}

/* =====================================================================
 *  实验 A：脏读（Dirty Read）
 *
 *  定义：T2 读到 T1 尚未提交的数据；T1 随后回滚 → T2 用的是一个从未存在过的值。
 *
 *  SQLite 只有 SERIALIZABLE，哪来的脏读？答案是 shared-cache 模式下的
 *  PRAGMA read_uncommitted = 1：两条连接共享页缓存时，读连接会直接看见
 *  写连接未提交的页 —— 这就是教科书定义的脏读，而且真能跑出来。
 *  它正好对应 MySQL 的 READ UNCOMMITTED。
 * ===================================================================== */
static void experimentA_DirtyRead() {
    std::printf("\n=========== 实验 A：脏读（T2 读到 T1 未提交的数据）===========\n");
    // 全局线程模式退到 MULTITHREAD：这是 shared-cache 脏读生效的前提之一。
    // （每个连接仍可用 OPEN_FULLMUTEX 单独升回 SERIALIZED，见 Db 构造的注释）
    sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
    sqlite3_enable_shared_cache(1);          // 全局开关：允许共享页缓存
    {
        // 两条连接都必须是 SHAREDCACHE：私有缓存的连接压根不和读连接共享页，
        // read_uncommitted 自然永远读不到脏数据（我第一版就栽在这儿）。
        Db w(kLab, Db::Mode::SharedCache), r(kLab, Db::Mode::SharedCache);
        noWait(w); noWait(r);
        w.exec("PRAGMA journal_mode=WAL");
        resetTable(w, 100);
        r.exec("PRAGMA read_uncommitted = 1");   // 关键：允许读未提交
        std::printf("  设置：shared-cache + read_uncommitted=1（等价 MySQL 的 READ UNCOMMITTED）\n");

        Rendezvous rv;
        long long r1 = -1, r2 = -1;

        std::thread t1([&] {
            w.exec("BEGIN");
            w.exec("UPDATE t SET v = 999 WHERE id = 1");
            log("T1 写者", "已 UPDATE v=999，但故意【不 COMMIT】，等你先读");
            rv.mark("A_uncommitted");
            rv.wait("A_read_done");
            holdMs(1);
            w.exec("ROLLBACK");
            log("T1 写者", "ROLLBACK —— 刚才那个 999 从来没有存在过");
            rv.mark("A_rolled_back");
        });

        std::thread t2([&] {
            rv.wait("A_uncommitted");
            r1 = r.queryLong("SELECT v FROM t WHERE id = 1");
            log("T2 读者", "T1 未提交时读到 v = " + std::to_string(r1) +
                           (r1 == 999 ? "   << ★脏读发生了！" : "   （干净）"));
            rv.mark("A_read_done");
            rv.wait("A_rolled_back");
            r2 = r.queryLong("SELECT v FROM t WHERE id = 1");
            log("T2 读者", "T1 回滚之后再读 v = " + std::to_string(r2));
        });

        t1.join(); t2.join();
        std::printf("  结论：T2 先拿到 %lld，回滚后变成 %lld。\n", r1, r2);
        std::printf("        T2 拿着 %lld 做了业务决策，可这个值【从未被提交过】。\n", r1);
        std::printf("        如果 T2 基于它发货、扣款、发短信，你就把脏数据扩散出去了。\n");

        std::printf("\n  【对照组】换成正常连接（不开 read_uncommitted），同一时序再读一次：\n");
        // 注意：这里另开一条 PRIVATECACHE 的普通连接，而不是把上面那条读连接的
        // read_uncommitted 关掉。原因：shared-cache 里关了这个开关，读连接不是
        // "读旧值"，而是直接吃 SQLITE_LOCKED 报错（查询失败，连旧值都拿不到）。
        // 这才是真实世界的样子：要么给你已提交的数据，要么干脆不让你读。
        {
            Db c(kLab);
            noWait(c);
            w.exec("BEGIN");
            w.exec("UPDATE t SET v = 555 WHERE id = 1");
            long long r3 = c.queryLong("SELECT v FROM t WHERE id = 1");
            log("普通读者", "T1 未提交时读到 v = " + std::to_string(r3) +
                           (r3 == 555 ? "   仍然脏 ✘" : "   << 只读到已提交的旧值，脏读消失 ✔"));
            w.exec("ROLLBACK");
        }
        sqlite3_enable_shared_cache(0);
    }
    std::printf("  类比：read_uncommitted ≈ 你直接去读另一个线程还没写完的裸内存，\n");
    std::printf("        没有 mutex、没有 release/acquire，读到什么全凭运气。\n");
    std::printf("        数据库把它做成开关是给极少数分析场景用的，OLTP 开它等于自杀。\n");
}

/* =====================================================================
 *  实验 B：不可重复读（Non-Repeatable Read）
 *
 *  SQLite 这里的教学价值极高：
 *    · 自动提交（每条语句自带一个事务）→ 快照在每条语句开始时重建
 *      → 表现 = READ COMMITTED → 不可重复读【会发生】
 *    · BEGIN...COMMIT 包住（DEFERRED + WAL）→ 快照在事务内第一条读语句固定
 *      → 表现 ≈ REPEATABLE READ → 不可重复读【消失】
 *  RC 与 RR 的全部差别 = 快照建立时机，代码上就差一个 BEGIN。
 * ===================================================================== */
static void experimentB_NonRepeatableRead() {
    std::printf("\n=========== 实验 B：不可重复读（同一连接两次读，值变了）===========\n");
    {
        Db a(kLab), b(kLab);
        noWait(a); noWait(b);
        a.exec("PRAGMA journal_mode=WAL");
        resetTable(a, 100);
        std::printf("  两个连接都跑在 WAL 上：读者读快照，不会被写者阻塞（一写多读）\n");

        Rendezvous rv;
        long long first = -1, second = -1, s1 = -1, s2 = -1;

        std::thread tReader([&] {
            // 场景 1：自动提交模式 —— 没有 BEGIN 包裹
            first = a.queryLong("SELECT v FROM t WHERE id = 1");
            log("T2 读者", "[RC] 第 1 次读 v = " + std::to_string(first));
            rv.mark("B_read1");
            rv.wait("B_write1");
            second = a.queryLong("SELECT v FROM t WHERE id = 1");
            log("T2 读者", "[RC] 第 2 次读 v = " + std::to_string(second) +
                           (second != first ? "   << ★不可重复读发生了！" : "   （可重复）"));
            rv.mark("B_read2");

            // 场景 2：同样的时序，只是用 BEGIN 把两次读包起来
            rv.wait("B_write2");
            a.exec("BEGIN");                    // DEFERRED：快照在第一条读语句时固定
            s1 = a.queryLong("SELECT v FROM t WHERE id = 1");
            log("T2 读者", "[RR] BEGIN 后第 1 次读 v = " + std::to_string(s1));
            rv.mark("C_read1");
            rv.wait("C_write1");
            s2 = a.queryLong("SELECT v FROM t WHERE id = 1");
            log("T2 读者", "[RR] 别人提交后再读 v = " + std::to_string(s2) +
                           (s2 == s1 ? "   << 快照生效，值没变 ✔" : "   << 还是变了 ✘"));
            a.exec("COMMIT");
            rv.mark("C_done");
        });

        std::thread tWriter([&] {
            rv.wait("B_read1");
            b.exec("BEGIN IMMEDIATE");
            b.exec("UPDATE t SET v = 200 WHERE id = 1");
            b.exec("COMMIT");
            log("T1 写者", "提交：v 100 -> 200");
            rv.mark("B_write1");
            rv.wait("B_read2");

            b.exec("BEGIN IMMEDIATE");
            b.exec("UPDATE t SET v = 777 WHERE id = 1");
            b.exec("COMMIT");
            log("T1 写者", "提交：v 200 -> 777");
            rv.mark("B_write2");

            rv.wait("C_read1");
            b.exec("BEGIN IMMEDIATE");
            b.exec("UPDATE t SET v = 888 WHERE id = 1");
            b.exec("COMMIT");
            log("T1 写者", "在 T2 的事务进行中提交了：v = 888");
            rv.mark("C_write1");
            rv.wait("C_done");
        });

        tReader.join(); tWriter.join();
        std::printf("  结论：RC（每条语句一个快照）会不可重复读；RR（事务级固定快照）不会。\n");
        std::printf("        RR 的代价：可能读到过期快照，且需要保留旧版本（undo / 版本链）。\n");
        std::printf("  类比：RC ≈ 每次用之前都 load() 一次重新取快照；\n");
        std::printf("        RR ≈ 开头 const auto snap = 一份本地副本，之后再也不看外面。\n");
    }
}

/* =====================================================================
 *  实验 C：丢失更新（Lost Update）—— 重头戏
 *
 *  两个事务都「读-改-写」同一行，后写的把先写的覆盖掉，净效果只体现一次。
 *  数据库一条错都不报，数据却悄悄错了。超卖、少扣款，九成是它。
 *
 *  这就是 C++ 里的
 *      x = g; x += 1; g = x;      // 非原子 read-modify-write
 *  而不是 g.fetch_add(1)。区别只在于"数据库不会替你保证跨语句原子性"。
 * ===================================================================== */
static void experimentC_LostUpdate() {
    std::printf("\n=========== 实验 C：丢失更新（read-modify-write 不原子）===========\n");

    /* ---- C.1 编排版：保证看得见一次丢失 ---- */
    std::printf("\n  --- C.1 编排交错（会合点控制，稳定复现）---\n");
    {
        Db a(kLab), b(kLab);
        noWait(a); noWait(b);
        a.exec("PRAGMA journal_mode=WAL");
        resetTable(a, 100);
        std::printf("  初始 v=100，T1 要 +10，T2 要 +20，正确答案 130。\n");

        Rendezvous rv;
        const int kBoth = 2;

        auto worker = [&](Db& d, const std::string& tag, int delta) {
            long long cur = d.queryLong("SELECT v FROM t WHERE id = 1");
            log(tag, "读到 v = " + std::to_string(cur));
            rv.sync("C_read", kBoth);          // 等到两个事务都读完了再往下走
            long long newVal = cur + delta;
            log(tag, "在内存里算 " + std::to_string(cur) + " + " + std::to_string(delta) +
                     " = " + std::to_string(newVal) + "（手里这个 cur 已经过期）");
            d.tryExec("UPDATE t SET v = " + std::to_string(newVal) + " WHERE id = 1");
            log(tag, "写回 v = " + std::to_string(newVal) + "（自动提交，毫无报错）");
        };

        std::thread t1(worker, std::ref(a), "T1(+10)", 10);
        std::thread t2(worker, std::ref(b), "T2(+20)", 20);
        t1.join(); t2.join();

        long long finalVal = a.queryLong("SELECT v FROM t WHERE id = 1");
        std::printf("  最终 v = %lld。正确答案 130 → %s\n", finalVal,
                    finalVal == 130 ? "这次侥幸没丢（把顺序反一下必然丢）"
                                    : "★丢失更新：有一次 +10/+20 被无声吞掉了");
        std::printf("  全程 SQLITE_OK。这才是它可怕的地方 —— 监控上一条错误都没有。\n");
    }

    /* ---- C.2 真·自由竞争：跑 300 次统计丢失率，再上三种修法 ---- */
    std::printf("\n  --- C.2 放开一切同步跑 300 次（真竞争 + 三种修法）---\n");
    {
        Db init(kLab);
        init.exec("PRAGMA journal_mode=WAL");
        init.exec("DROP TABLE IF EXISTS counter");
        init.exec("CREATE TABLE counter(id INTEGER PRIMARY KEY, v INTEGER NOT NULL, version INTEGER NOT NULL DEFAULT 0)");
        init.exec("INSERT INTO counter VALUES(1, 0, 0)");

        const int kIters = 150;                 // 每线程 150 次，共 300 次 +1
        const long long kExpected = 2LL * kIters;

        // 版本 0：裸 read-modify-write
        auto bumpNaive = [](void) {
            Db d(kLab);
            noWait(d);
            for (int i = 0; i < kIters; ++i) {
                long long cur = d.queryLong("SELECT v FROM counter WHERE id = 1");
                // 读写之间让出 CPU：真实业务里这个窗口是"序列化/网络往返/业务判断"
                std::this_thread::yield();
                d.tryExec("UPDATE counter SET v = " + std::to_string(cur + 1) + " WHERE id = 1");
            }
        };
        std::thread x1(bumpNaive), y1(bumpNaive);
        x1.join(); y1.join();
        long long naive = init.queryLong("SELECT v FROM counter WHERE id = 1");
        std::printf("  裸读改写    ：期望 %lld，实际 %lld → 丢了 %lld 次（丢失率 %.1f%%）\n",
                    kExpected, naive, kExpected - naive, 100.0 * (kExpected - naive) / kExpected);

        // 修法一：把读改写压成一条 SQL
        init.exec("UPDATE counter SET v = 0, version = 0");
        std::atomic<long long> sqlBusy{0};
        auto bumpSql = [&](void) {
            Db d(kLab);
            noWait(d);   // 故意不用 busy_timeout，改用显式重试，看清 BUSY 有多频繁
            for (int i = 0; i < kIters; ++i)
                // 单语句天然原子，但 BUSY 一样会来 —— 原子性解决"丢更新"，
                // 不解决"锁竞争"。这里必须重试，否则 BUSY 被吞，又是一次丢失更新。
                execRetry(d, "UPDATE counter SET v = v + 1 WHERE id = 1", &sqlBusy);
        };
        std::thread x2(bumpSql), y2(bumpSql);
        x2.join(); y2.join();
        long long v2 = init.queryLong("SELECT v FROM counter WHERE id = 1");
        std::printf("  修法①单条 SQL：实际 %lld → %s（BUSY 重试 %lld 次）\n", v2,
                    v2 == kExpected ? "分毫不差 ✔" : "还是丢 ✘", sqlBusy.load());

        // 修法二：乐观锁 version + CAS 重试
        init.exec("UPDATE counter SET v = 0, version = 0");
        std::atomic<long long> totalRetries{0};
        std::atomic<long long> maxRetrySeen{0};
        auto bumpCas = [&](void) {
            Db d(kLab);
            noWait(d);
            for (int i = 0; i < kIters; ++i) {
                long long attempt = 0;
                for (;; ++attempt) {
                    auto pr = queryPair(d, "SELECT v, version FROM counter WHERE id=1");
                    long long v = pr.first, ver = pr.second;
                    char sql[256];
                    std::snprintf(sql, sizeof sql,
                        "UPDATE counter SET v = %lld, version = %lld WHERE id = 1 AND version = %lld",
                        v + 1, ver + 1, ver);          // ≈ compare_exchange_strong(ver, ver+1)
                    d.tryExec(sql);
                    if (sqlite3_changes(d.raw()) == 1) break;   // CAS 成功
                    ++totalRetries;                             // 等价于 CAS 的 spurious failure
                    std::this_thread::yield();
                }
                long long prev = maxRetrySeen.load();
                while (attempt > prev && !maxRetrySeen.compare_exchange_weak(prev, attempt)) {}
            }
        };
        std::thread x3(bumpCas), y3(bumpCas);
        x3.join(); y3.join();
        long long v3 = init.queryLong("SELECT v FROM counter WHERE id = 1");
        std::printf("  修法②乐观锁  ：实际 %lld，CAS 重试共 %lld 次，单条最多重试 %lld 次 → %s\n",
                    v3, totalRetries.load(), maxRetrySeen.load(), v3 == kExpected ? "正确 ✔" : "错 ✘");
        std::printf("               等价于：while(!cas(v, v+1)) v = load();  —— 你写 atomic 的标配自旋\n");

        // 修法三：悲观锁（行锁 ≈ 按 key 分片的 mutex）
        init.exec("UPDATE counter SET v = 0, version = 0");
        std::mutex rowLock;                        // 模拟 id=1 这一行的行锁
        std::atomic<long long> serialCount{0};
        auto bumpPessimistic = [&](void) {
            Db d(kLab);
            for (int i = 0; i < kIters; ++i) {
                std::lock_guard<std::mutex> lk(rowLock);   // 「SELECT ... FOR UPDATE」的本地等价物
                ++serialCount;
                long long cur = d.queryLong("SELECT v FROM counter WHERE id = 1");
                std::this_thread::yield();                 // 持锁期干点活：真库里就是持锁时长
                d.tryExec("UPDATE counter SET v = " + std::to_string(cur + 1) +
                          ", version = version + 1 WHERE id = 1");
            }
        };
        std::thread x4(bumpPessimistic), y4(bumpPessimistic);
        x4.join(); y4.join();
        long long v4 = init.queryLong("SELECT v FROM counter WHERE id = 1");
        std::printf("  修法③悲观锁  ：实际 %lld，串行化了 %lld 次 → %s\n",
                    v4, serialCount.load(), v4 == kExpected ? "正确 ✔" : "错 ✘");
    }
    std::printf("\n  【三种修法怎么选】\n");
    std::printf("   ①压成一条 SQL —— 首选。能一条语句解决就别开事务，冲突概率直接归零。\n");
    std::printf("   ②乐观锁 version —— 冲突少时最优（绝大多数 Web 业务）。冲突激烈时退化成活锁，\n");
    std::printf("     重试次数暴涨，所以必须有最大重试次数 + 指数退避 + 抖动。\n");
    std::printf("   ③悲观锁 FOR UPDATE —— 冲突激烈或必须独占时用。代价：吞吐下降 + 可能死锁\n");
    std::printf("     （见 04 号文件），而且必须在事务里，autocommit 下锁语句一结束就放掉了。\n");
}

/* =====================================================================
 *  实验 D：写写冲突与「锁升级失败」
 * ===================================================================== */
static void experimentD_WriteWrite() {
    std::printf("\n=========== 实验 D：写写冲突 & 锁升级失败 ===========\n");

    /* D.1 两个 BEGIN IMMEDIATE：后到者立刻 SQLITE_BUSY */
    std::printf("\n  --- D.1 两个 IMMEDIATE 抢写锁 ---\n");
    {
        Db a(kLab), b(kLab);
        noWait(a); noWait(b);
        a.exec("PRAGMA journal_mode=WAL");
        resetTable(a, 1);
        Rendezvous rv;
        int rb = SQLITE_OK;

        std::thread t1([&] {
            rv.sync("D1_start", 2);           // 等 T2 就绪，确保 T1 先拿到锁
            a.exec("BEGIN IMMEDIATE");
            log("T1", "BEGIN IMMEDIATE 成功，拿到库级写锁");
            a.exec("UPDATE t SET v = v + 1 WHERE id = 1");
            rv.mark("D1_locked");
            holdMs(8);                        // 持锁 8ms，足够让 T2 撞上
            a.exec("COMMIT");
            log("T1", "COMMIT，释放写锁");
            rv.mark("D1_released");
        });
        std::thread t2([&] {
            rv.sync("D1_start", 2);
            rv.wait("D1_locked");             // 明确等到 T1 持锁，再去撞
            rb = b.tryExec("BEGIN IMMEDIATE");
            log("T2", std::string("BEGIN IMMEDIATE -> ") + rcName(rb) +
                      (rb == SQLITE_BUSY ? "   << ★写写冲突，被拒" : ""));
            holdMs(4);                        // 退避
            rb = b.tryExec("BEGIN IMMEDIATE");
            log("T2", std::string("退避 4ms 后重试 -> ") + rcName(rb));
            if (rb == SQLITE_OK) b.exec("COMMIT");
            rv.wait("D1_released");
        });
        t1.join(); t2.join();
        std::printf("  结论：SQLite 只有一个写者（库级写锁）。要行级并发去 MySQL/PostgreSQL，\n");
        std::printf("        但「写冲突了就退避重试」这套逻辑放到哪个库上都一样。\n");
    }

    /* D.2 两个 DEFERRED 都想"读后升级成写" */
    std::printf("\n  --- D.2 DEFERRED：先读后写，都想把共享快照升级成写锁 ---\n");
    {
        Db a(kLab), b(kLab);
        noWait(a); noWait(b);
        a.exec("PRAGMA journal_mode=WAL");
        resetTable(a, 10);
        Rendezvous rv;
        int ra = SQLITE_OK, rb = SQLITE_OK;

        std::thread t1([&] {
            a.exec("BEGIN");                              // DEFERRED：此刻一把锁都不拿
            long long v = a.queryLong("SELECT v FROM t WHERE id = 1");
            log("T1", "读 v=" + std::to_string(v) + "（建立了读快照）");
            rv.sync("D2_read", 2);
            ra = a.tryExec("UPDATE t SET v = v + 1 WHERE id = 1");   // 想升级成写
            log("T1", std::string("升级写锁 -> ") + rcName(ra) + (ra ? "   << 被拒" : "   成功"));
            if (ra == SQLITE_OK) a.exec("COMMIT"); else a.exec("ROLLBACK");
        });
        std::thread t2([&] {
            b.exec("BEGIN");
            long long v = b.queryLong("SELECT v FROM t WHERE id = 1");
            log("T2", "读 v=" + std::to_string(v) + "（也建立了读快照）");
            rv.sync("D2_read", 2);
            rb = b.tryExec("UPDATE t SET v = v + 1 WHERE id = 1");
            log("T2", std::string("升级写锁 -> ") + rcName(rb) + (rb ? "   << 被拒" : "   成功"));
            if (rb == SQLITE_OK) b.exec("COMMIT"); else b.exec("ROLLBACK");
        });
        t1.join(); t2.join();
        std::printf("  T1=%s / T2=%s（两个都想升级，必有一个拿不到）\n", rcName(ra).c_str(), rcName(rb).c_str());
        std::printf("  教训：明知要写的事务，一开始就 BEGIN IMMEDIATE，别等升级。\n");
        std::printf("  类比：两个人各持 shared_lock 想 upgrade 成 unique_lock —— 经典升级死锁。\n");
        std::printf("        C++ 里的解法（要么一开始 unique_lock，要么 try_lock_for 失败即退）\n");
        std::printf("        和数据库的解法一模一样，这不是巧合。\n");
    }
}

/* =====================================================================
 *  实验 E：综合 —— 并发转账，最终守恒
 * ===================================================================== */
static void experimentE_Transfer() {
    std::printf("\n=========== 实验 E：综合 —— 并发转账（写锁 + 版本号，保证守恒）===========\n");
    {
        Db init(kLab);
        init.exec("PRAGMA journal_mode=WAL");
        init.exec("DROP TABLE IF EXISTS acct");
        init.exec("CREATE TABLE acct(id INTEGER PRIMARY KEY, balance INTEGER NOT NULL CHECK(balance>=0), version INTEGER NOT NULL DEFAULT 0)");
        init.exec("INSERT INTO acct VALUES(1, 1000, 0), (2, 1000, 0)");
        std::printf("  初始 A=1000 B=1000，总和必须恒等于 2000。4 个线程各转 60 笔 ×5 元。\n");

        const int kEach = 60;
        std::atomic<int> okCnt{0}, busyCnt{0}, casCnt{0}, shortCnt{0};

        auto transfer = [&](int from, int to, int amt) {
            Db d(kLab);
            noWait(d);
            for (int attempt = 1; attempt <= 20; ++attempt) {
                int rc = d.tryExec("BEGIN IMMEDIATE");   // 悲观：先拿写锁（SQLite 是库级）
                if (rc != SQLITE_OK) { ++busyCnt; std::this_thread::yield(); continue; }

                auto pf = queryPair(d, "SELECT balance, version FROM acct WHERE id=" + std::to_string(from));
                auto pt = queryPair(d, "SELECT balance, version FROM acct WHERE id=" + std::to_string(to));
                if (pf.first < amt) { d.exec("ROLLBACK"); ++shortCnt; return; }   // 余额不足：业务性失败

                char s1[200], s2[200];
                std::snprintf(s1, sizeof s1, "UPDATE acct SET balance=%lld, version=%lld WHERE id=%d AND version=%lld",
                              pf.first - amt, pf.second + 1, from, pf.second);
                std::snprintf(s2, sizeof s2, "UPDATE acct SET balance=%lld, version=%lld WHERE id=%d AND version=%lld",
                              pt.first + amt, pt.second + 1, to, pt.second);
                d.tryExec(s1); bool c1 = sqlite3_changes(d.raw()) == 1;
                d.tryExec(s2); bool c2 = sqlite3_changes(d.raw()) == 1;
                if (c1 && c2) { d.exec("COMMIT"); ++okCnt; return; }
                d.exec("ROLLBACK");                       // CAS 失败 → 整个事务作废重来
                ++casCnt;
                std::this_thread::yield();
            }
        };

        std::vector<std::thread> ts;
        for (int i = 0; i < 4; ++i)
            ts.emplace_back([&, i] { for (int k = 0; k < kEach; ++k) transfer(i % 2 ? 1 : 2, i % 2 ? 2 : 1, 5); });
        for (auto& t : ts) t.join();

        long long a = init.queryLong("SELECT balance FROM acct WHERE id=1");
        long long bb = init.queryLong("SELECT balance FROM acct WHERE id=2");
        std::printf("  成功 %d 笔，等写锁重试 %d 次，CAS 重试 %d 次，余额不足 %d 次\n",
                    okCnt.load(), busyCnt.load(), casCnt.load(), shortCnt.load());
        std::printf("  最终 A=%lld B=%lld 总和=%lld → %s\n", a, bb, a + bb,
                    (a + bb == 2000) ? "守恒 ✔（并发下钱没有凭空增减）" : "不守恒 ✘（有 BUG）");
        std::printf("  注：这里用了 BEGIN IMMEDIATE，所以事务内 CAS 必然成功；\n");
        std::printf("      如果你换成 DEFERRED 并且不加 version 条件，守恒性就会破 —— 那是练习 3。\n");
    }
}

/* =====================================================================
 *  main
 * ===================================================================== */
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("================================================================================\n");
    std::printf(" 并发异常实验室 —— 背十遍隔离级别表格，不如亲眼看一次交错\n");
    std::printf(" SQLite %s | 每个实验都是真实多线程 + 真实多连接 + 同一个 db 文件\n", sqlite3_libversion());
    std::printf("================================================================================\n");

    experimentA_DirtyRead();
    experimentB_NonRepeatableRead();
    experimentC_LostUpdate();
    experimentD_WriteWrite();
    experimentE_Transfer();

    std::printf("\n================================================================================\n");
    std::printf(" 老师傅的话\n");
    std::printf("--------------------------------------------------------------------------------\n");
    std::printf(" 1. 最危险的不是报错，是「没报错但数据错了」。实验 C 全程 SQLITE_OK，\n");
    std::printf("    更新照样丢。数据库错误码是兜底用的，不是你免责的凭据。\n");
    std::printf(" 2. 丢失更新 = read-modify-write 不原子。这句话在 C++ 里叫非原子自增，\n");
    std::printf("    在数据库里叫并发扣库存，是同一个 bug 的两副面孔。\n");
    std::printf("    识别它只需问一句：我的读和写之间，别人能不能插进来？\n");
    std::printf(" 3. RC 与 RR 的全部差别就是快照什么时候建立：RC 每条语句一次，\n");
    std::printf("    RR 事务开始一次。想成 const auto snap = load() 放循环里还是循环外。\n");
    std::printf(" 4. 幻读是丢失更新的亲戚，但它针对的不是某一行的值变了，而是\n");
    std::printf("    符合某条件的行集合变了。所以防它要靠范围锁（MySQL 间隙锁），\n");
    std::printf("    单行版本号挡不住 —— 新插入的行根本没有旧版本可比。\n");
    std::printf("    动手：把 isolation_playground.html 的统计快照场景切到 Serializable 看效果。\n");
    std::printf(" 5. 生产建议（按优先级）：\n");
    std::printf("    a. 能用一条 UPDATE 解决的，别开事务读改写；\n");
    std::printf("    b. 必须读改写的，加 version 乐观锁 + 最多 3~5 次重试 + 指数退避 + 抖动；\n");
    std::printf("    c. 冲突激烈或必须独占的，用 SELECT ... FOR UPDATE，并固定加锁顺序；\n");
    std::printf("    d. 加锁顺序全局统一（比如永远按主键升序），死锁概率能砍掉九成；\n");
    std::printf("    e. 事务里绝不做 RPC/IO/等用户输入 —— 窗口越宽，丢得越多。\n");
    std::printf("================================================================================\n");

    std::printf("\n【练习】\n");
    std::printf(" 1. 把实验 C.1 的 rv.sync 那行注释掉重跑 20 次，记录最终值分布。\n");
    std::printf("    你会看到 110 / 120 / 130 都可能出现 —— 这就是竞态的指纹。\n");
    std::printf(" 2. 实验 C.2 的 yield() 删掉，丢失率怎么变？换成 sleep_for(1ms) 又怎么变？\n");
    std::printf("    由此理解窗口宽度决定冲突概率，这不是玄学是可以量级的。\n");
    std::printf(" 3. 把实验 E 的 BEGIN IMMEDIATE 改成 BEGIN 并去掉 version 条件，\n");
    std::printf("    跑 10 次看总和是否还等于 2000 —— 亲手把守恒性弄坏一次比看十遍有用。\n");
    std::printf(" 4. 给实验 C.2 的乐观锁加最大重试次数，并把 kIters 提到 2000，\n");
    std::printf("    观察重试暴涨（活锁征兆）—— 那就是乐观锁的失效边界。\n");
    std::printf(" 5. 打开 mvcc_animator.html，把实验 B 的时序在里面重放一遍，\n");
    std::printf("    看 T2 的 read view 在 RC 与 RR 下分别圈住了哪些版本。\n");

    sqlite3_enable_shared_cache(0);
    std::remove(kLab);
    std::remove("iso_lab.db-wal");
    std::remove("iso_lab.db-shm");
    std::printf("\n[cleanup] 已删除 iso_lab.db / -wal / -shm\n");
    return 0;
}
