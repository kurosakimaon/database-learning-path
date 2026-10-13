/*
================================================================
  03_thread_pool_db.cpp —— 第 4 关第三站：线程安全的连接池

  编译：  g++ -std=c++17 -O2 03_thread_pool_db.cpp -o 03.exe -lsqlite3
  运行：  .\03.exe

  师傅的话：
    单机 SQLite 是"一个文件"，多线程同时写就会撞锁。
    真数据库（MySQL/PG）也一样：一条 TCP 连接不能两个线程同时用，
    连接握手 + 认证 + 初始化又是几百微秒到几毫秒的贵东西。
    所以工业界的答案永远是那一个词：连接池。

    这个文件干三件事：
      1. 手写一个像样的连接池：固定槽位 + mutex/condition_variable +
         RAII 归还（ConnectionGuard）。池子这东西，说白了就是
         "一个带等待队列的免费对象集合"，一点不神秘。
      2. 并发跑写事务，把 SQLITE_BUSY（database is locked）真的撞出来，
         让你看见"不做重试"是什么样；再用 WAL + busy_timeout + 重试
         把它治住，并把重试次数统计出来。
      3. 三种取连接方式对比：每次新建 / 单连接串行 / 连接池。

    ⚠ 注意：数字会随机器变化，但"database is locked 会不会出现"
      是确定性的，本文件在没重试的那一轮一定报错，这是设计出来的。
================================================================
*/

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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
// 一、极简连接封装
// ===========================================================================
// 02 号文件里有完整版，这里为了单文件自包含，只留必需的部分。
// 生产项目请复用同一份封装，别维护两套。
class SqliteError : public std::runtime_error
{
public:
    SqliteError(int rc, int extended, const std::string &m, std::string ctx)
        : std::runtime_error("[" + ctx + "] rc=" + std::to_string(rc) + " (ext=" +
                             std::to_string(extended) + "): " + m),
          rc_(rc), extended_(extended)
    {}
    [[nodiscard]] int rc() const noexcept { return rc_; }
    [[nodiscard]] int extended_rc() const noexcept { return extended_; }

private:
    int rc_, extended_;
};

// 连接：move-only，析构自动 close。
// 注意 SQLITE_OPEN_NOMUTEX：故意用"非序列化"模式，也就是
// 这条连接同一时刻只能被一个线程使用 —— 这正是"必须有连接池"的前提。
// 如果图省事用 FULLMUTEX，多线程共用一条连接虽然不崩，
// 但所有语句在 SQLite 内部排队，并发度直接归零（见策略 B）。
class Conn
{
public:
    explicit Conn(const std::string &path) : path_(path)
    {
        int rc = sqlite3_open_v2(path.c_str(), &h_,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                 nullptr);
        if (rc != SQLITE_OK)
        {
            std::string m = h_ ? sqlite3_errmsg(h_) : "open failed";
            sqlite3_close(h_);
            h_ = nullptr;
            throw SqliteError(rc, rc, m, "Conn::Conn");
        }
        sqlite3_extended_result_codes(h_, 1);
    }

    Conn(const Conn &) = delete;
    Conn &operator=(const Conn &) = delete;

    Conn(Conn &&o) noexcept : path_(std::move(o.path_)), h_(o.h_), ins_(o.ins_)
    {
        o.h_ = nullptr;
        o.ins_ = nullptr;
    }

    ~Conn()
    {
        if (h_)
            sqlite3_busy_handler(h_, nullptr, nullptr); // 先摘 handler，再 close
        if (ins_)
            sqlite3_finalize(ins_);
        if (h_)
            sqlite3_close(h_);
        delete handler_ctx_;
    }

    // 只在构造后立刻调用一次：设置 WAL / busy_timeout 这类连接级 PRAGMA。
    void pragma(const std::string &sql)
    {
        char *err = nullptr;
        const int rc = sqlite3_exec(h_, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK)
        {
            std::string m = err ? err : "?";
            sqlite3_free(err);
            throw SqliteError(rc, sqlite3_extended_errcode(h_), m, "Conn::pragma");
        }
    }

    void exec(const std::string &sql)
    {
        char *err = nullptr;
        const int rc = sqlite3_exec(h_, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK)
        {
            std::string m = err ? err : "?";
            sqlite3_free(err);
            throw SqliteError(rc, sqlite3_extended_errcode(h_), m, "Conn::exec");
        }
    }

    // 单条 INSERT，成功返回 true。
    // 返回 false 时把 rc 带出去，让调用方决定"是重试还是放弃"。
    // 这里刻意不用异常表达 busy：busy 是并发下的常态，不是异常。
    //
    // 语句只 prepare 一次然后反复 reset —— 这是吞吐的基本功。
    // 每条 INSERT 都 prepare 的话，解析+规划的开销会盖住真实成本，
    // 你的基准数据就全废了。
    bool try_insert(int64_t wid, double amount, const std::string &memo, int &rc_out, int &ext_out)
    {
        if (!ins_)
        {
            const char *sql = "INSERT INTO jobs(worker,amount,memo) VALUES(?,?,?)";
            const int rc = sqlite3_prepare_v2(h_, sql, -1, &ins_, nullptr);
            if (rc != SQLITE_OK)
            {
                rc_out = rc;
                ext_out = sqlite3_extended_errcode(h_);
                return false;
            }
        }
        sqlite3_reset(ins_);
        sqlite3_bind_int64(ins_, 1, wid);
        sqlite3_bind_double(ins_, 2, amount);
        sqlite3_bind_text(ins_, 3, memo.data(), static_cast<int>(memo.size()), SQLITE_TRANSIENT);
        const int rc = sqlite3_step(ins_);
        if (rc == SQLITE_DONE)
        {
            rc_out = SQLITE_OK;
            ext_out = SQLITE_OK;
            return true;
        }
        rc_out = rc;
        ext_out = sqlite3_extended_errcode(h_);
        return false;
    }

    // 不抛异常的 exec：把返回码原样交出去。
    // 为什么需要它：BEGIN IMMEDIATE 抢不到写锁时会返回 SQLITE_BUSY。
    // 如果用会抛异常的 exec()，busy 就掉进 catch(...) 里被当成"其他错误"，
    // 于是你统计到的"撞锁次数"永远是 0 —— 这个坑我第一次写就踩了，
    // 现象是"明明失败一大堆，BUSY 计数却是 0"，排查半天。
    int try_exec(const std::string &sql)
    {
        char *err = nullptr;
        const int rc = sqlite3_exec(h_, sql.c_str(), nullptr, nullptr, &err);
        if (err)
            sqlite3_free(err);
        return rc;
    }

    [[nodiscard]] sqlite3 *raw() const noexcept { return h_; }

    // 自定义 busy handler：把"SQLite 内部重试"变成可数的数字。
    //
    // 语义要说清楚（这是 SQLite C API 最容易记反的地方）：
    //   回调返回 0 → "我再等一会儿，请重试那条失败的语句"
    //   回调返回非 0 → "别试了，直接给我 SQLITE_BUSY"
    // 装了自定义 handler 之后，PRAGMA busy_timeout 装的内置 handler 就被顶掉了，
    // 所以下面自己实现一个"睡 1ms、最多 N 次"的退避，语义等价但可统计。
    struct HandlerCtx
    {
        std::atomic<uint64_t> *counter = nullptr;
        int max_tries = 100;
    };

    void install_counting_busy_handler(std::atomic<uint64_t> *counter, int max_tries)
    {
        auto *box = new HandlerCtx{counter, max_tries};
        sqlite3_busy_handler(h_,
                             [](void *p, int nTry) -> int
                             {
                                 auto *ctx = static_cast<HandlerCtx *>(p);
                                 if (ctx->counter)
                                     ctx->counter->fetch_add(1);
                                 if (nTry >= ctx->max_tries)
                                     return 1; // 放弃 → 上层收到 SQLITE_BUSY
                                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                 return 0; // 继续重试
                             },
                             box);
        handler_ctx_ = box;
    }

private:
    std::string path_;
    sqlite3 *h_ = nullptr;
    sqlite3_stmt *ins_ = nullptr;       // 复用的 INSERT 语句
    HandlerCtx *handler_ctx_ = nullptr; // busy handler 的统计上下文
};

// ===========================================================================
// 二、连接池
// ===========================================================================
//
// 设计说明（这段是重点，面试也常问）：
//   * 池大小固定 = 最多同时有 M 个连接在外面。为什么固定？
//     因为"连接数"本身是数据库侧的资源，放开上限等于把压力推给 DB。
//   * 等待队列用 deque + condition_variable，FIFO 申请、FIFO 唤醒，
//     避免饿死。真要严格 LIFO（复用热连接）也行，那是另一种权衡。
//   * 归还放进析构（RAII）：哪怕业务抛异常、提前 return，连接必回池。
//     池子泄漏的排查看起来像"偶发卡死"，八成就是某条路径没还。
//   * acquire 有超时：宁可快速失败让上层降级，也不要无限期挂住线程。
//     超时时间是"用户体验"和"资源利用率"之间的旋钮。
class ConnectionPool
{
public:
    struct Options
    {
        size_t size = 4;             // 池大小（连接槽位数）
        int busy_timeout_ms = 0;     // >0：装 busy handler（内部重试，1ms 一次，次数=该值）
        bool wal = true;             // 是否开 WAL
        int acquire_timeout_ms = 3000; // 等连接的最长时间
    };

    struct Stats
    {
        std::atomic<uint64_t> acquire_total{0};    // 申请次数
        std::atomic<uint64_t> acquire_timeout{0};  // 等待超时次数
        std::atomic<uint64_t> created{0};          // 实际建连次数
        std::atomic<uint64_t> wait_us_total{0};    // 累计等待微秒（算平均等待用）
        std::atomic<uint64_t> internal_retries{0}; // SQLite 内部 busy handler 重试次数
    };

    ConnectionPool(std::string path, Options opt) : path_(std::move(path)), opt_(opt)
    {
        for (size_t i = 0; i < opt_.size; ++i)
        {
            auto c = std::make_unique<Conn>(path_);
            // WAL：写不阻塞读、读写并发能力大幅提升。
            // 但注意：WAL 下依然只有一个 writer，只是"排队更优雅"。
            if (opt_.wal)
                c->pragma("PRAGMA journal_mode=WAL;");
            // busy 处理：>0 时装自定义 handler（可统计），=0 时不装（撞锁立刻 SQLITE_BUSY）。
            // 这里没有用 PRAGMA busy_timeout，是为了能把"内部重试次数"打印出来；
            // 效果等价：每次睡 1ms，最多睡 busy_timeout_ms 次。
            if (opt_.busy_timeout_ms > 0)
                c->install_counting_busy_handler(&stats_.internal_retries, opt_.busy_timeout_ms);
            else
                c->pragma("PRAGMA busy_timeout=0;");
            // synchronous=NORMAL：WAL 下兼顾安全与速度的常用档位。
            // FULL 最安全但每次提交都 fsync；OFF 快但可能丢最后几笔（生产别乱用）。
            c->pragma("PRAGMA synchronous=NORMAL;");
            stats_.created.fetch_add(1);
            free_.push_back(std::move(c));
        }
    }

    ConnectionPool(const ConnectionPool &) = delete;
    ConnectionPool &operator=(const ConnectionPool &) = delete;

    // 借一个连接。拿不到就等待，超过 acquire_timeout_ms 返回 nullptr。
    // 返回 unique_ptr：所有权转移给调用方，天然不会忘记归还——
    // 但"忘记还"的后果是连接永久减少，所以我们更推荐下面的 Guard。
    std::unique_ptr<Conn> acquire()
    {
        std::unique_lock<std::mutex> lk(mu_);
        stats_.acquire_total.fetch_add(1);
        const auto t0 = std::chrono::steady_clock::now();

        const bool got = cv_.wait_for(lk, std::chrono::milliseconds(opt_.acquire_timeout_ms),
                                      [this] { return !free_.empty(); });
        const auto t1 = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        stats_.wait_us_total.fetch_add(static_cast<uint64_t>(us > 0 ? us : 0));

        if (!got)
        {
            stats_.acquire_timeout.fetch_add(1);
            return nullptr; // 超时：上层降级/报错，别硬等
        }
        auto c = std::move(free_.front());
        free_.pop_front();
        return c;
    }

    void release(std::unique_ptr<Conn> c)
    {
        if (!c)
            return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            free_.push_back(std::move(c));
        }
        cv_.notify_one(); // 只唤醒一个：一个连接给一个人，全员唤醒是惊群
    }

    [[nodiscard]] size_t free_count() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return free_.size();
    }

    [[nodiscard]] const Options &options() const noexcept { return opt_; }
    Stats &stats() noexcept { return stats_; }

private:
    std::string path_;
    Options opt_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<Conn>> free_;
    Stats stats_;
    std::atomic<uint64_t> busy_hits_{0};
    std::atomic<uint64_t> sleep_calls_{0};
    std::atomic<uint64_t> sleep_us_total_{0};
};

// RAII 归还器：拿到连接就上锁（借出），离开作用域自动还回池子。
// 有了它，"异常路径忘记归还"这类 bug 在结构上不存在。
class ConnectionGuard
{
public:
    ConnectionGuard(ConnectionPool &pool, std::unique_ptr<Conn> conn)
        : pool_(pool), conn_(std::move(conn))
    {}

    ConnectionGuard(const ConnectionGuard &) = delete;
    ConnectionGuard &operator=(const ConnectionGuard &) = delete;

    ConnectionGuard(ConnectionGuard &&o) noexcept : pool_(o.pool_), conn_(std::move(o.conn_)) {}

    ~ConnectionGuard()
    {
        if (conn_)
            pool_.release(std::move(conn_));
    }

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(conn_); }
    [[nodiscard]] Conn *operator->() const noexcept { return conn_.get(); }
    [[nodiscard]] Conn &operator*() const noexcept { return *conn_; }

private:
    ConnectionPool &pool_;
    std::unique_ptr<Conn> conn_;
};

// ===========================================================================
// 三、并发统计
// ===========================================================================
struct WorkloadStats
{
    std::atomic<uint64_t> tx_ok{0};           // 成功提交的事务数
    std::atomic<uint64_t> tx_failed{0};       // 最终失败的事务数
    std::atomic<uint64_t> busy_errors{0};     // 遇到 SQLITE_BUSY 的次数
    std::atomic<uint64_t> app_retries{0};     // 应用层重试次数
    std::atomic<uint64_t> pool_timeouts{0};   // 等连接超时次数
    std::atomic<uint64_t> other_errors{0};    // 其他错误
    std::atomic<uint64_t> rows_written{0};    // 成功写入行数
};

// ===========================================================================
// 四、工作线程：跑写事务
// ===========================================================================
//
// 每个线程做 rounds 轮，每轮 = 一个事务，事务里 batch 条 INSERT。
// 事务粒度是"锁的粒度"：粒度越大并发冲突越少但单次持锁越久。
// 这是并发调优里最朴素也最有效的一对矛盾。
// ===========================================================================
static void worker(ConnectionPool &pool, int worker_id, int rounds, int batch,
                   bool retry_on_busy, int max_retries, WorkloadStats &st)
{
    for (int r = 0; r < rounds; ++r)
    {
        // 1. 借连接
        auto conn = pool.acquire();
        if (!conn)
        {
            st.pool_timeouts.fetch_add(1);
            continue;
        }
        ConnectionGuard guard(pool, std::move(conn));

        // 2. 在连接上跑事务；busy 就重试（如果开了重试）
        //
        // 关键纪律：busy 必须走"返回值"这条路，绝不能让它变成异常。
        // 因为异常是"分类处理"的， busy 一旦落进 catch(...) 就会被归到
        // "其他错误"里，你看到的统计就永远是"撞锁 0 次"，
        // 于是你以为系统没有锁竞争，实际上你的程序在疯狂失败。
        // 我第一次写这份代码就栽在这儿，见文件末尾的师傅点评。
        int attempt = 0;
        while (true)
        {
            bool busy = false;
            std::string hard_error;
            const char *stage = "begin";

            // 先记住"撞锁之前那条语句的错误原文"，再动连接。
            // 顺序反了就会被后面的 ROLLBACK 把 errmsg 覆盖成
            // "cannot rollback - no transaction is active"，
            // 学徒看到的永远是错的报错 —— 这个顺序坑我吃过。
            auto grab_err = [&]() { return std::string(sqlite3_errmsg(guard->raw())); };
            // 连接上是否真的挂着事务，只有一个可靠判据：sqlite3_get_autocommit()。
            // 返回 1 = 不在事务里（此时 ROLLBACK 会报"no transaction is active"），
            // 返回 0 = 在事务里（此时必须 ROLLBACK，否则脏连接还池给别人）。
            auto in_txn = [&]() { return sqlite3_get_autocommit(guard->raw()) == 0; };
            auto safe_rollback = [&]()
            {
                if (in_txn())
                    guard->try_exec("ROLLBACK");
            };

            // 一上来就抢写锁（BEGIN IMMEDIATE），避免"读事务中途升级失败"。
            // 升级失败的典型症状是 SQLITE_BUSY 出现在事务中间，
            // 那时候你已经改了一半的数据，回滚成本高得多。
            const int brc = guard->try_exec("BEGIN IMMEDIATE");
            if (brc == SQLITE_BUSY || brc == SQLITE_LOCKED)
            {
                busy = true;
                hard_error = grab_err(); // BEGIN 失败 = 事务根本没开，直接失败
            }
            else if (brc != SQLITE_OK)
            {
                hard_error = "begin: " + grab_err();
            }

            if (!busy && hard_error.empty())
            {
                stage = "insert";
                for (int i = 0; i < batch; ++i)
                {
                    int rc = SQLITE_OK, ext = SQLITE_OK;
                    if (!guard->try_insert(worker_id, 1.0 + i, "w" + std::to_string(worker_id), rc, ext))
                    {
                        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
                        {
                            busy = true;
                            hard_error = grab_err();
                            break;
                        }
                        hard_error = "insert: rc=" + std::to_string(rc) + " " + grab_err();
                        break;
                    }
                }
                if (!busy && hard_error.empty())
                {
                    stage = "commit";
                    const int crc = guard->try_exec("COMMIT");
                    if (crc != SQLITE_OK)
                        hard_error = "commit: rc=" + std::to_string(crc) + " " + grab_err();
                }
            }

            // 统一收尾：出错的分支（不管 busy 还是硬错误）都在这里把连接清干净。
            if (!hard_error.empty())
            {
                // 事务里出了错，SQLite 不会自动回滚整个事务（只回滚那条语句）。
                // 不 ROLLBACK 的话这条连接挂着半个事务被归还进池，
                // 后面拿到它的人全都写不进去 —— 并发代码里最阴的一类线上事故。
                safe_rollback();

                const bool is_busy = busy;
                if (is_busy)
                {
                    st.busy_errors.fetch_add(1);
                    static std::atomic_flag first_busy = ATOMIC_FLAG_INIT;
                    if (!first_busy.test_and_set())
                        printf("  [worker %d] 在 %s 阶段抓到撞锁，原文：\"%s\"\n", worker_id,
                               stage, hard_error.c_str());
                }
                else
                {
                    st.other_errors.fetch_add(1);
                    static std::atomic_flag first_hard = ATOMIC_FLAG_INIT;
                    if (!first_hard.test_and_set())
                        printf("  [worker %d] 硬错误示例：%s\n", worker_id, hard_error.c_str());
                }

                if (!is_busy || !retry_on_busy)
                {
                    st.tx_failed.fetch_add(1);
                    break;
                }
                if (++attempt > max_retries)
                {
                    st.tx_failed.fetch_add(1);
                    break;
                }
                st.app_retries.fetch_add(1);
                // 指数退避 + 抖动。为什么带随机？
                // 固定退避会让所有撞锁的线程"同时醒来再撞一次"，
                // 这叫 convoy（车队效应）。加抖动把它们错开。
                const int backoff_us = 500 * (1 << std::min(attempt, 6));
                std::this_thread::sleep_for(std::chrono::microseconds(backoff_us + (rand() % 300)));
                continue;
            }

            // 走到这里一定是"事务干净提交"：busy 的分支在上面要么 break 要么 continue。
            st.tx_ok.fetch_add(1);
            st.rows_written.fetch_add(static_cast<uint64_t>(batch));
            break;
        }
    }
}

// ===========================================================================
// 五、准备表
// ===========================================================================
static void reset_database(const std::string &path)
{
    std::remove(path.c_str());
    std::remove((path + "-journal").c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());

    Conn c(path);
    c.pragma("PRAGMA journal_mode=WAL;");
    c.pragma("PRAGMA synchronous=NORMAL;");
    c.exec("CREATE TABLE jobs(id INTEGER PRIMARY KEY, worker INTEGER, amount REAL, memo TEXT)");
    // 收尾：把建表这件事 checkpoint 进主库文件，保持后面测量的干净起点。
    c.exec("PRAGMA wal_checkpoint(TRUNCATE);");
}

// ===========================================================================
// 六、三种取连接方式的对比
// ===========================================================================
struct ScenarioResult
{
    std::string name;
    double ms = 0.0;
    uint64_t tx_ok = 0;
    uint64_t tx_failed = 0;
    uint64_t busy = 0;
    uint64_t retries = 0;
    uint64_t pool_timeouts = 0;
    double avg_wait_ms = 0.0;
    uint64_t handshakes = 0; // 建连次数
};

static ScenarioResult run_pooled(const std::string &tag, const std::string &path,
                                 size_t pool_size, int threads, int rounds, int batch,
                                 bool retry, bool wal, int busy_timeout_ms)
{
    std::printf("\n【%s】池=%zu 线程=%d 每人%d轮 每轮%d条 WAL=%s busy_timeout=%dms 重试=%s\n",
                tag.c_str(), pool_size, threads, rounds, batch, wal ? "ON" : "OFF",
                busy_timeout_ms, retry ? "ON" : "OFF");

    reset_database(path);
    ConnectionPool::Options opt;
    opt.size = pool_size;
    opt.busy_timeout_ms = busy_timeout_ms;
    opt.wal = wal;
    opt.acquire_timeout_ms = 5000;
    ConnectionPool pool(path, opt);

    WorkloadStats st;
    std::vector<std::thread> ts;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < threads; ++i)
        ts.emplace_back(worker, std::ref(pool), i, rounds, batch, retry, 12, std::ref(st));
    for (auto &t : ts)
        t.join();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    ScenarioResult res;
    res.name = tag;
    res.ms = ms;
    res.tx_ok = st.tx_ok.load();
    res.tx_failed = st.tx_failed.load();
    res.busy = st.busy_errors.load();
    res.retries = st.app_retries.load();
    res.pool_timeouts = st.pool_timeouts.load();
    res.handshakes = pool.stats().created.load();
    res.avg_wait_ms = pool.stats().wait_us_total.load() / 1000.0 /
                      std::max<uint64_t>(1, pool.stats().acquire_total.load());

    const uint64_t total_tx = res.tx_ok + res.tx_failed;
    std::printf("   耗时 %.1f ms │ 成功事务 %llu/%llu │ 失败 %llu │ 撞锁 %llu 次 │ 应用重试 %llu 次 │ 内部重试 %llu 次\n",
                ms, (unsigned long long)res.tx_ok, (unsigned long long)total_tx,
                (unsigned long long)res.tx_failed, (unsigned long long)res.busy,
                (unsigned long long)res.retries,
                (unsigned long long)pool.stats().internal_retries.load());
    std::printf("   写入行数 %llu │ 吞吐 %.0f 行/秒 │ 平均等连接 %.3f ms │ 建连次数 %llu\n",
                (unsigned long long)st.rows_written.load(),
                res.ms > 0 ? st.rows_written.load() * 1000.0 / res.ms : 0.0,
                res.avg_wait_ms, (unsigned long long)res.handshakes);
    if (res.pool_timeouts)
        std::printf("   ⚠ 等连接超时 %llu 次 → 池太小或事务太长，该扩容或缩短持锁时间\n",
                    (unsigned long long)res.pool_timeouts);

    // 校验：库里行数必须等于成功写入的行数，这证明"并发写没写丢"。
    Conn check(path);
    sqlite3_stmt *st2 = nullptr;
    sqlite3_prepare_v2(check.raw(), "SELECT COUNT(*) FROM jobs", -1, &st2, nullptr);
    long long db_rows = 0;
    if (sqlite3_step(st2) == SQLITE_ROW)
        db_rows = sqlite3_column_int64(st2, 0);
    sqlite3_finalize(st2);
    std::printf("   库里实际行数 %lld（应等于写入行数 %llu）→ %s\n",
                db_rows, (unsigned long long)st.rows_written.load(),
                db_rows == static_cast<long long>(st.rows_written.load()) ? "一致 ✔" : "不一致 ✘");
    return res;
}

// 每次新建连接（无池）：连接成本每笔都付一遍。
static ScenarioResult run_per_request_connection(const std::string &path, int threads, int rounds, int batch)
{
    std::printf("\n【策略A：每次新建连接】线程=%d 每人%d轮 每轮%d条\n", threads, rounds, batch);
    reset_database(path);

    std::atomic<uint64_t> rows{0}, ok{0}, fail{0}, busy{0}, retries{0}, handshakes{0};
    auto body = [&](int wid)
    {
        for (int r = 0; r < rounds; ++r)
        {
            int attempt = 0;
            while (true)
            {
                // 每笔都握手：open + 三条 PRAGMA 的成本，每次事务重付一遍。
                // 注意 busy 一律走返回值（try_exec），理由和 worker() 里一样。
                try
                {
                    auto c = std::make_unique<Conn>(path); // ← 每笔都握手
                    handshakes.fetch_add(1);
                    // 这三条 PRAGMA 里的 journal_mode=WAL 是需要拿锁的：
                    // 别的连接正在写时它会直接报 "database is locked"。
                    // 也就是说，"建连接"这件事本身在并发下也会失败 ——
                    // 这正是每次新建连接策略最要命的地方，光看代码看不出来。
                    c->pragma("PRAGMA journal_mode=WAL;");
                    c->pragma("PRAGMA busy_timeout=5000;");
                    c->pragma("PRAGMA synchronous=NORMAL;");

                    bool done = false;
                    const int brc = c->try_exec("BEGIN IMMEDIATE");
                    if (brc == SQLITE_OK)
                    {
                        done = true;
                        for (int i = 0; i < batch && done; ++i)
                        {
                            int rc = SQLITE_OK, ext = SQLITE_OK;
                            if (!c->try_insert(wid, 1.0, "no-pool", rc, ext))
                            {
                                done = false;
                                if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
                                    busy.fetch_add(1);
                            }
                        }
                        if (done && c->try_exec("COMMIT") != SQLITE_OK)
                            done = false;
                    }
                    else if (brc == SQLITE_BUSY || brc == SQLITE_LOCKED)
                    {
                        busy.fetch_add(1);
                    }

                    if (done)
                    {
                        rows.fetch_add(static_cast<uint64_t>(batch));
                        ok.fetch_add(1);
                        break; // c 在这里析构 → close，这就是"每次新建连接"的代价
                    }

                    if (sqlite3_get_autocommit(c->raw()) == 0)
                        c->try_exec("ROLLBACK"); // 示范必须正确：归还/关闭前清干净事务
                }
                catch (const SqliteError &e)
                {
                    static std::atomic_flag first = ATOMIC_FLAG_INIT;
                    if (!first.test_and_set())
                        std::printf("   [worker %d] 建连阶段就撞墙了：%s\n", wid, e.what());
                }

                if (++attempt > 12)
                {
                    fail.fetch_add(1);
                    break;
                }
                retries.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(500 + (rand() % 300)));
            }
        }
    };

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i)
        ts.emplace_back(body, i);
    for (auto &t : ts)
        t.join();

    ScenarioResult res;
    res.name = "每次新建连接";
    res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    res.tx_ok = ok.load();
    res.tx_failed = fail.load();
    res.busy = busy.load();
    res.retries = retries.load();
    res.handshakes = handshakes.load();
    std::printf("   耗时 %.1f ms │ 成功事务 %llu │ 失败 %llu │ BUSY %llu │ 建连次数 %llu\n",
                res.ms, (unsigned long long)res.tx_ok, (unsigned long long)res.tx_failed,
                (unsigned long long)res.busy, (unsigned long long)res.handshakes);
    std::printf("   写入行数 %llu │ 吞吐 %.0f 行/秒\n", (unsigned long long)rows.load(),
                res.ms > 0 ? rows.load() * 1000.0 / res.ms : 0.0);
    return res;
}

// 单连接串行：所有活儿挤一根连接，"安全但慢"。
static ScenarioResult run_single_connection(const std::string &path, int total_rounds, int batch)
{
    std::printf("\n【策略B：单连接串行】共%d轮 每轮%d条（多线程只是在排队）\n", total_rounds, batch);
    reset_database(path);

    Conn c(path);
    c.pragma("PRAGMA journal_mode=WAL;");
    c.pragma("PRAGMA synchronous=NORMAL;");
    c.pragma("PRAGMA busy_timeout=5000;");

    std::mutex m; // 一根连接必须串行使用，否则两个线程同时 step 直接未定义行为
    uint64_t rows = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < total_rounds; ++r)
    {
        std::lock_guard<std::mutex> lk(m);
        c.exec("BEGIN IMMEDIATE");
        for (int i = 0; i < batch; ++i)
        {
            int rc = SQLITE_OK, ext = SQLITE_OK;
            if (!c.try_insert(0, 1.0, "single", rc, ext))
                throw SqliteError(rc, ext, "insert failed", "single-conn");
        }
        c.exec("COMMIT");
        rows += static_cast<uint64_t>(batch);
    }
    ScenarioResult res;
    res.name = "单连接串行";
    res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    res.tx_ok = static_cast<uint64_t>(total_rounds);
    std::printf("   耗时 %.1f ms │ 写入行数 %llu │ 吞吐 %.0f 行/秒 │ BUSY 0（串行不冲突）\n",
                res.ms, (unsigned long long)rows, res.ms > 0 ? rows * 1000.0 / res.ms : 0.0);
    std::printf("   永远不撞锁，但吞吐被一根连接锁死 —— 这就是\"安全但慢\"。\n");
    return res;
}

// ===========================================================================
// 七、把 database is locked 真的打出来
// ===========================================================================
// 用 busy_timeout=0 + 关掉重试，让冲突直接暴露成错误。
// 这一步是给学徒看的"病历"：不治它之前，先确认它真的会发作。
// ===========================================================================
static void demo_locked_error(const std::string &path, int threads, int rounds, int batch)
{
    std::printf("\n================ 对照组：不做任何重试会发生什么 ================\n");
    std::printf("设置：WAL=ON、busy_timeout=0（不等待）、应用层重试=OFF\n");
    auto res = run_pooled("反面教材", path, static_cast<size_t>(threads), threads, rounds, batch,
                          /*retry=*/false, /*wal=*/true, /*busy_timeout_ms=*/0);
    if (res.busy > 0)
    {
        std::printf("   ★ 实测抓到 %llu 次 SQLITE_BUSY，最终失败 %llu 个事务。\n",
                    (unsigned long long)res.busy, (unsigned long long)res.tx_failed);
        std::printf("   ★ 对应的报错文本就是用户看到的 \"database is locked\"。\n");
        // 把错误原文取一次出来，让学徒知道长什么样
        Conn c(path);
        c.pragma("PRAGMA busy_timeout=0;");
        c.exec("BEGIN EXCLUSIVE"); // 独占锁，另一个连接必然拿不到
        Conn c2(path);
        c2.pragma("PRAGMA busy_timeout=0;");
        const int rc = sqlite3_exec(c2.raw(), "INSERT INTO jobs(worker,amount,memo) VALUES(9,9,'x')",
                                    nullptr, nullptr, nullptr);
        std::printf("   ★ 原文：rc=%d，errmsg=\"%s\"\n", rc, sqlite3_errmsg(c2.raw()));
        c.exec("COMMIT");
    }
    else
    {
        std::printf("   本轮没抓到 BUSY（调度太顺），把线程数调大再试。\n");
    }
}

int main(int argc, char **argv)
{
    setup_utf8_console();

    // 默认规模：4 线程 × 120 轮 × 20 行 = 9600 行。留参数口子方便学徒改。
    int threads = 4;
    int rounds = 120;
    int batch = 20;
    if (argc > 1) threads = std::atoi(argv[1]);
    if (argc > 2) rounds = std::atoi(argv[2]);
    if (argc > 3) batch = std::atoi(argv[3]);
    if (threads < 1) threads = 1;
    if (rounds < 1) rounds = 1;
    if (batch < 1) batch = 1;

    const std::string path = "03_pool.db";

    std::printf("\n================================================================\n");
    std::printf("  03 · 连接池与并发写：把 database is locked 治明白\n");
    std::printf("================================================================\n");
    std::printf("SQLite 版本 %s，SQLite 是进程内库；这里的\"连接\"就是打开同一个 .db 文件。\n",
                sqlite3_libversion());
    std::printf("并发规模：线程 %d │ 每线程 %d 个事务 │ 每事务 %d 条 INSERT │ 合计 %d 行\n",
                threads, rounds, batch, threads * rounds * batch);

    std::vector<ScenarioResult> results;

    // --- 1. 反面教材：不重试 ---
    demo_locked_error(path, threads, rounds, batch);

    // --- 2. 策略 B：单连接串行 ---
    results.push_back(run_single_connection(path, threads * rounds, batch));

    // --- 3. 策略 A：每次新建连接 ---
    results.push_back(run_per_request_connection(path, threads, rounds, batch));

    // --- 4. 策略 C：连接池 + busy_timeout（SQLite 内部重试）---
    results.push_back(run_pooled("连接池+busy_timeout", path,
                                 static_cast<size_t>(std::max(2, threads / 2)), threads, rounds, batch,
                                 /*retry=*/true, /*wal=*/true, /*busy_timeout_ms=*/5000));

    // --- 5. 策略 C2：连接池 + busy_timeout=0 + 应用层重试（看得到自己的重试次数）---
    results.push_back(run_pooled("连接池+应用层重试", path,
                                 static_cast<size_t>(std::max(2, threads / 2)), threads, rounds, batch,
                                 /*retry=*/true, /*wal=*/true, /*busy_timeout_ms=*/0));

    // --- 6. 小池压力测试：池比线程小很多，看排队 ---
    results.push_back(run_pooled("小池(2)压力测试", path, 2, threads, rounds, batch,
                                 /*retry=*/true, /*wal=*/true, /*busy_timeout_ms=*/5000));

    std::printf("\n================================================================\n");
    std::printf("  汇总对比（本机实测）\n");
    std::printf("================================================================\n");
    std::printf("%-26s %10s %12s %10s %10s %10s\n",
                "策略", "耗时ms", "行/秒", "成功事务", "BUSY次数", "建连次数");
    for (const auto &r : results)
    {
        // 行/秒用汇总数据估算：策略名里的 rounds/batch 已打印在上面，这里用 tx_ok*batch
        char line[256];
        std::snprintf(line, sizeof(line), "%-26s %10.1f %12.0f %10llu %10llu %10llu",
                      r.name.c_str(), r.ms,
                      r.ms > 0 ? r.tx_ok * static_cast<uint64_t>(batch) * 1000.0 / r.ms : 0.0,
                      (unsigned long long)r.tx_ok, (unsigned long long)r.busy,
                      (unsigned long long)r.handshakes);
        std::printf("%s\n", line);
    }

    std::printf("\n师傅点评：\n");
    std::printf("1. 连接池的价值不在\"快一点点\"，在于把建连成本从 O(请求数) 降到 O(池大小)。\n"
                "   真数据库里建一条连接是 TCP+TLS+认证，5~50ms 很常见；不池化，QPS 上限就是它的倒数。\n");
    std::printf("2. 单连接串行永远不报 database is locked —— 因为它根本没并发。\n"
                "   别把\"没报错\"当成\"解决了\"，那是掩盖。\n");
    std::printf("3. 治 SQLITE_BUSY 三板斧，顺序不能颠倒：\n"
                "     a) WAL 模式（读写并发、锁粒度更细）\n"
                "     b) PRAGMA busy_timeout（让 SQLite 替你退避，比自己睡更准）\n"
                "     c) 应用层指数退避重试（处理真正冲突的场景，必须有重试上限）\n");
    std::printf("4. 事务失败必须 ROLLBACK。不 ROLLBACK 的连接会一直挂着事务，\n"
                "   于是\"一个线程出错，全服写不进去\"。这是最阴的一类线上事故。\n");
    std::printf("5. 事务越小、持锁越短，并发越好；但事务太小，fsync 成本爆炸。\n"
                "   02 号文件教了你批处理，这里教了你并发，两者是同一枚硬币的两面。\n");
    std::printf("6. 池大小不是越大越好：SQLite 只有一个 writer，多出来的连接只是排队；\n"
                "   真数据库里连接数超过 CPU 核数后吞吐反而下降（上下文切换 + 锁竞争）。\n");
    std::printf("7. 生产环境还要加的：连接健康检查（空闲多久回收）、\n"
                "   借出超时强制回收（防泄漏）、指标暴露（等待时间 P99、超时数）。\n");

    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
    std::remove((path + "-journal").c_str());
    return 0;
}
