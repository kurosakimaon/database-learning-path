/* =====================================================================
 *  04_lock_and_deadlock.cpp
 *  第 3 关 · 心法期 —— 手搓锁管理器 + 等待图找环：把死锁抓个现行
 *
 *  编译（Windows / MSYS2 g++ 15.2，无 CMake，单文件直出，纯 C++ 标准库）：
 *      g++ -std=c++17 -O2 04_lock_and_deadlock.cpp -o 04_lock_and_deadlock.exe
 *  运行：
 *      .\04_lock_and_deadlock.exe
 *
 *  为什么 SQLite 里看不到行锁？
 *      SQLite 的写锁是【库级】的（03 号文件你已经撞过了）。想看行锁、
 *      看共享锁/排他锁、看死锁检测，只能自己写一个 —— 好消息是：
 *      写完之后你会明白，数据库那套东西本质就是
 *          「一张锁表 + 一个等待队列 + 一张等待图 + 一次 DFS 找环」。
 *
 *  C++ 概念对照（本文件的核心类比）：
 *      行锁            ≈  按 key 分片的 std::mutex（这里用「资源名 → 锁记录」实现）
 *      共享锁 S        ≈  shared_mutex 的 lock_shared()
 *      排他锁 X        ≈  shared_mutex 的 lock()
 *      锁升级          ≈  你手里有 shared 又想 lock() —— 别的共享者不走，你就得等
 *      锁等待超时      ≈  try_lock_for(timeout)：等不到就撤，别把线程吊死
 *      死锁检测        ≈  在「事务等待图」上找环（DFS + 路径栈），找到就回滚一个
 *      回滚受害者      ≈  释放它全部锁并唤醒等待者 —— 等价于 RAII 的 unlock_all
 * ===================================================================== */

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

/* =====================================================================
 *  基础设施：毫秒时间戳日志 + CJK 显示宽度对齐
 * ===================================================================== */
static std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();

// printf 的 %-Ns 按【字节】算宽度，一个汉字在 UTF-8 里 3 字节、在终端里占 2 列。
// 所以中文表格必须自己按显示宽度补空格，否则一定错位。
static int displayWidth(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++w; }
        else if (c < 0xC0) { /* 续字节，不计 */ }
        else { ++w; ++i; }   // 两字节序列的首字节；三字节序列的后续字节被当续字节跳过
    }
    return w;
}
static std::string padDisplay(const std::string& s, int width) {
    std::string out = s;
    for (int i = displayWidth(s); i < width; ++i) out += ' ';
    return out;
}
static void log(const std::string& who, const std::string& msg) {
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - g_t0).count();
    char head[64];
    std::snprintf(head, sizeof head, "[%6lld ms] ", ms);
    std::printf("  %s%s| %s\n", head, padDisplay(who, 18).c_str(), msg.c_str());
}

static void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 微秒级自旋：模拟"拿到第一把锁之后还要算一会儿再去要第二把"的业务窗口。
// 这个窗口就是死锁的温床 —— 窗口为 0 的话，事务快到根本撞不上，演示也就没意义了。
static void busyUs(int us) {
    auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    volatile long long sink = 0;
    while (std::chrono::steady_clock::now() < end) ++sink;
}

// 命名会合点：把线程卡成 A-B-A-B，让交错能被肉眼看清（等价 std::barrier）
class Rendezvous {
public:
    void sync(const std::string& name, int parties) {
        std::unique_lock<std::mutex> lk(m_);
        int& c = cnt_[name];
        ++c;
        if (c >= parties) { c = 0; cv_.notify_all(); return; }
        cv_.wait(lk, [&] { return cnt_[name] == 0; });
    }
    void mark(const std::string& name) {
        std::unique_lock<std::mutex> lk(m_);
        flags_[name] = true;
        cv_.notify_all();
    }
    void wait(const std::string& name) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return flags_[name]; });
    }
    void reset() {
        std::unique_lock<std::mutex> lk(m_);
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
 *  LockManager：共享/排他锁 + 等待队列 + 锁升级 + 超时 + 等待图找环
 *
 *  数据结构就三样，别被"锁管理器"三个字唬住：
 *      table_  : 资源名 → { 共享持有者列表, 排他持有者, 等待队列 }
 *      owned_  : 事务 → 它持有的资源集合（回滚时一次全放）
 *      等待图  : 不常驻内存，需要时从 table_ 现推（谁在等谁）
 * ===================================================================== */
using TxnId = int;
static const TxnId kNoTxn = 0;

enum class Mode { S, X };
static const char* modeName(Mode m) { return m == Mode::S ? "S" : "X"; }

enum class Acquire { Granted, Timeout, Deadlock, Aborted };
static const char* acqName(Acquire a) {
    switch (a) {
        case Acquire::Granted:  return "拿到锁";
        case Acquire::Timeout:  return "等待超时";
        case Acquire::Deadlock: return "死锁·我是受害者";
        case Acquire::Aborted:  return "死锁·我被回滚";
    }
    return "?";
}

struct LockManager {
    explicit LockManager(bool detect) : detectDeadlock(detect) {}

    struct Waiter {
        TxnId txn;
        Mode  mode;
        bool  ready   = false;   // 已授予
        bool  aborted = false;   // 被回滚（含自己做受害者）
    };
    struct Row {
        TxnId xHolder = kNoTxn;
        std::vector<TxnId> sHolders;
        std::vector<std::shared_ptr<Waiter>> waiters;   // FIFO，防饿死
    };

    bool detectDeadlock = true;
    long long statWait = 0, statTimeout = 0, statDeadlock = 0;

    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, Row> table_;
    std::map<TxnId, std::set<std::string>> owned_;
    std::set<TxnId> aborted_;              // 已回滚事务：后续申请一律拒绝
    TxnId lastVictim_ = kNoTxn;
    std::vector<TxnId> lastCycle_;

    /* ---------- 兼容性判定 ---------- */
    bool holdsS(TxnId t, const Row& r) const {
        return std::find(r.sHolders.begin(), r.sHolders.end(), t) != r.sHolders.end();
    }
    // 兼容矩阵（真数据库的第一课，背下来）：
    //        已有 S      已有 X
    //   要 S   兼容       冲突
    //   要 X   冲突       冲突
    // 注意这个函数【只看持有者，不看等待队列】。队列优先级是另一码事，见 below。
    bool compatible(const Row& r, TxnId t, Mode m) const {
        if (r.xHolder != kNoTxn && r.xHolder != t) return false;   // 别人的 X 谁都躲不开
        if (m == Mode::X) {
            // 要 X：必须一个共享持有者都没有（除了自己手上那把 S，那叫升级）
            for (TxnId h : r.sHolders) if (h != t) return false;
        }
        return true;
    }
    // 能不能立刻授予 = 兼容 + 前面没有不兼容的人在排队（FIFO，防饿死）。
    bool canGrant(const Row& r, TxnId t, Mode m) const {
        if (!compatible(r, t, m)) return false;
        for (auto& w : r.waiters) {
            if (w->txn == t) continue;
            if (m == Mode::S && w->mode == Mode::S) continue;   // 同为 S 可以同批授
            return false;                                        // 前面有人挡着，不插队
        }
        return true;
    }
    // 注意 res 必须一路传下来。我第一版图省事用了个成员变量 currentRes_ 记"当前资源"，
    // 结果 wakeAndGrant 里给别人授权时记到了别的资源上，锁表直接烂掉 —— 这类
    // "隐式上下文"的坑，在真正的锁管理器里是致命的，别学。
    void grant(Row& r, TxnId t, Mode m, const std::string& res) {
        if (m == Mode::X) {
            r.xHolder = t;
            // 升级：拿到 X 之后手上那把 S 就没意义了，收掉，别让锁表出现「又 S 又 X」
            r.sHolders.erase(std::remove(r.sHolders.begin(), r.sHolders.end(), t),
                             r.sHolders.end());
        } else {
            if (!holdsS(t, r)) r.sHolders.push_back(t);          // 幂等：别重复登记
        }
        owned_[t].insert(res);
    }

    // 唤醒并授予：先清掉已回滚事务的僵尸等待，再按 FIFO 从队头往下授。
    // S 可以连续授一片，遇到不兼容的队头就停（不插队）。
    void wakeAndGrant(const std::string& res) {
        Row& r = table_[res];
        r.waiters.erase(std::remove_if(r.waiters.begin(), r.waiters.end(),
                                       [&](const std::shared_ptr<Waiter>& w) {
                                           return aborted_.count(w->txn) > 0;
                                       }),
                        r.waiters.end());
        // 队头判定只能用 compatible（只看持有者）。第一版这儿手滑用了 canGrant，
        // 而 canGrant 会把队列里【候选者后面】的人也当成"挡路的人"——队头永远
        // 觉得有人在等，一个都授不出去，整行吞吐直接归零卡死。
        // FIFO 防插队的语义留给 acquire 的快路径：新来的必须看"前面的人"脸色的。
        for (auto it = r.waiters.begin(); it != r.waiters.end();) {
            auto& w = *it;
            if (!compatible(r, w->txn, w->mode)) break;
            w->ready = true;
            grant(r, w->txn, w->mode, res);
            it = r.waiters.erase(it);
        }
    }

    void eraseWaiter(Row& r, const std::shared_ptr<Waiter>& w) {
        r.waiters.erase(std::remove(r.waiters.begin(), r.waiters.end(), w), r.waiters.end());
    }

    /* ---------- 申请锁（带超时，超时或死锁都会干净退出）---------- */
    Acquire acquire(TxnId t, const std::string& res, Mode m, int timeoutMs) {
        std::unique_lock<std::mutex> lk(mu_);
        if (aborted_.count(t)) return Acquire::Aborted;
        Row& row = table_[res];

        if (m == Mode::S && (holdsS(t, row) || row.xHolder == t)) return Acquire::Granted;  // 幂等
        if (m == Mode::X && row.xHolder == t) return Acquire::Granted;

        if (canGrant(row, t, m)) { grant(row, t, m, res); return Acquire::Granted; }

        auto w = std::make_shared<Waiter>();
        w->txn = t; w->mode = m;
        row.waiters.push_back(w);
        ++statWait;

        if (detectDeadlock) {
            std::vector<TxnId> cycle;
            if (findCycle(t, cycle)) {
                TxnId victim = pickVictim(cycle);
                lastCycle_ = cycle;
                lastVictim_ = victim;
                ++statDeadlock;
                if (victim == t) {
                    eraseWaiter(row, w);       // 我是受害者：撤掉自己的等待再回滚
                    abortTxn(victim);
                    return Acquire::Deadlock;
                }
                // 我【不是】受害者：等待项必须留在队列里！
                // abortTxn 会放掉受害者的锁并 wakeAndGrant，那时队头就是我。
                // （第一版我在这里无条件 eraseWaiter，结果自己把自己从队列里
                //   摘了出去，永远等不到唤醒，只能干等到超时 —— 记一笔。）
                abortTxn(victim);
            }
        }

        auto t0 = std::chrono::steady_clock::now();
        bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                               [&] { return w->ready || w->aborted; });
        if (!ok) {
            eraseWaiter(table_[res], w);
            ++statTimeout;
            (void)t0;
            return Acquire::Timeout;
        }
        if (w->aborted) return Acquire::Aborted;
        return Acquire::Granted;
    }

    // 锁升级：手里有 S 想要 X，走同一条队列（这才是真数据库的行为）
    Acquire upgrade(TxnId t, const std::string& res, int timeoutMs) {
        return acquire(t, res, Mode::X, timeoutMs);
    }

    /* ---------- 释放 ---------- */
    void release(TxnId t, const std::string& res) {
        std::unique_lock<std::mutex> lk(mu_);
        auto it = table_.find(res);
        if (it == table_.end()) return;
        Row& r = it->second;
        if (r.xHolder == t) r.xHolder = kNoTxn;
        r.sHolders.erase(std::remove(r.sHolders.begin(), r.sHolders.end(), t), r.sHolders.end());
        owned_[t].erase(res);
        wakeAndGrant(res);
        cv_.notify_all();
    }
    void releaseAll(TxnId t) {
        std::unique_lock<std::mutex> lk(mu_);
        releaseAllLocked(t);
    }
    void releaseAllLocked(TxnId t) {
        auto it = owned_.find(t);
        if (it == owned_.end()) return;
        std::set<std::string> res = it->second;   // 拷一份，下面要改
        owned_[t].clear();
        for (auto& res2 : res) {
            Row& r = table_[res2];
            if (r.xHolder == t) r.xHolder = kNoTxn;
            r.sHolders.erase(std::remove(r.sHolders.begin(), r.sHolders.end(), t), r.sHolders.end());
            wakeAndGrant(res2);
        }
        cv_.notify_all();
    }

    // 回滚一个事务：先定性（aborted + 撤掉它所有等待），再放锁。
    // 顺序很关键：如果先放锁，wakeAndGrant 看见队列里还杵着受害者那个
    // "僵尸等待项"，会认为队头有人排队而不肯授予别人 —— 结果赢的那个
    // 干等到超时才被放行（我第一版就卡在这儿，1000ms 才通）。
    void abortTxn(TxnId t) {
        aborted_.insert(t);
        for (auto& kv : table_) {
            Row& r = kv.second;
            // 先给它的等待项盖上"作废"章并唤醒它本人（否则它干等到超时），
            // 再从队列里摘掉（否则它挡着别人不让授予）。两件事都得做，缺一不可。
            for (auto& w : r.waiters) if (w->txn == t) w->aborted = true;
            r.waiters.erase(std::remove_if(r.waiters.begin(), r.waiters.end(),
                                           [&](const std::shared_ptr<Waiter>& w) {
                                               return w->txn == t;
                                           }),
                            r.waiters.end());
        }
        cv_.notify_all();
        releaseAllLocked(t);   // 内部会 wakeAndGrant + notify_all
    }
    bool isAborted(TxnId t) {
        std::unique_lock<std::mutex> lk(mu_);
        return aborted_.count(t) > 0;
    }
    void forget(TxnId t) {   // 重试前把"案底"清掉，相当于开一个新事务号
        std::unique_lock<std::mutex> lk(mu_);
        aborted_.erase(t);
    }

    /* ---------- 死锁检测：在事务等待图上 DFS 找环 ---------- */
    // 边定义：T 在资源 R 上排队，R 被 T' 持有 ⇒ 有向边 T → T'（"T 等 T'"）
    bool findCycle(TxnId start, std::vector<TxnId>& path) {
        std::set<TxnId> inPath;
        return dfs(start, path, inPath);
    }
    bool dfs(TxnId cur, std::vector<TxnId>& path, std::set<TxnId>& inPath) {
        path.push_back(cur);
        inPath.insert(cur);
        for (auto& kv : table_) {
            Row& r = kv.second;
            bool waitsHere = false;
            for (auto& w : r.waiters) if (w->txn == cur) waitsHere = true;
            if (!waitsHere) continue;
            std::vector<TxnId> holders = r.sHolders;
            if (r.xHolder != kNoTxn) holders.push_back(r.xHolder);
            for (TxnId h : holders) {
                if (h == cur) continue;
                if (inPath.count(h)) { path.push_back(h); return true; }   // 回到路径上 ⇒ 有环
                if (dfs(h, path, inPath)) return true;
            }
        }
        inPath.erase(cur);
        path.pop_back();
        return false;
    }
    // 受害者选择：InnoDB 的做法是"回滚 undo 量最小的那个"。
    // 这里用"持有的锁最少的那个，打平取事务号大的（较新的）"。
    TxnId pickVictim(const std::vector<TxnId>& cycle) {
        TxnId best = cycle.front();
        size_t bestN = 1u << 30;
        for (size_t i = 0; i + 1 < cycle.size(); ++i) {
            TxnId t = cycle[i];
            size_t n = owned_[t].size();
            if (n < bestN || (n == bestN && t > best)) { best = t; bestN = n; }
        }
        return best;
    }

    /* ---------- 可观测性：打印锁表与等待图 ---------- */
    void dump(const std::string& tag) {
        std::unique_lock<std::mutex> lk(mu_);
        std::printf("\n  ── 锁表快照：%s ──\n", tag.c_str());
        std::printf("    %-16s %-10s %-14s %s\n", "资源", "排他持有", "共享持有", "等待队列");
        for (auto& kv : table_) {
            Row& r = kv.second;
            std::string sh;
            for (TxnId h : r.sHolders) sh += "T" + std::to_string(h) + " ";
            std::string wt;
            for (auto& w : r.waiters) wt += std::string(w->aborted ? "(废)" : "") +
                                            "T" + std::to_string(w->txn) +
                                            modeName(w->mode) + " ";
            std::printf("    %-16s %-10s %-14s %s\n",
                        padDisplay(kv.first, 16).c_str(),
                        padDisplay(r.xHolder ? "T" + std::to_string(r.xHolder) : "-", 10).c_str(),
                        padDisplay(sh.empty() ? "-" : sh, 14).c_str(),
                        wt.empty() ? "-" : wt.c_str());
        }
        std::string g;
        for (auto& kv : table_) {
            Row& r = kv.second;
            std::vector<TxnId> holders = r.sHolders;
            if (r.xHolder != kNoTxn) holders.push_back(r.xHolder);
            for (auto& w : r.waiters)
                for (TxnId h : holders)
                    if (h != w->txn)
                        g += "T" + std::to_string(w->txn) +
                             " -" + modeName(w->mode) + "等@" + kv.first + "-> T" +
                             std::to_string(h) + "   ";
        }
        std::printf("    等待图：%s\n", g.empty() ? "（空，没人等锁）" : g.c_str());
    }
    void printCycle() {
        std::unique_lock<std::mutex> lk(mu_);
        std::string s;
        for (size_t i = 0; i < lastCycle_.size(); ++i) {
            if (i) s += " -> ";
            s += "T" + std::to_string(lastCycle_[i]);
        }
        std::printf("    ★ 等待图找到环：%s\n", s.c_str());
        std::printf("      选 T%d 当受害者（持有锁最少），回滚它、放掉它全部锁\n", lastVictim_);
    }
};

/* =====================================================================
 *  演示 1：共享/排他/升级/超时 —— 先把语义看清楚
 * ===================================================================== */
static void demo1_semantics() {
    std::printf("\n===== 演示 1：共享锁 / 排他锁 / 锁升级 / 等待超时 =====\n");
    LockManager lm(false);            // 这一节先关掉死锁检测，纯净地看语义
    const std::string R = "row_user_42";

    // 主线程扮演 T1，另开线程扮演 T2/T3，才有真并发
    lm.acquire(1, R, Mode::S, 100);
    std::printf("  T1 申请 S(%s) → %s\n", R.c_str(), "拿到锁");

    std::thread th([&] {
        auto a = lm.acquire(2, R, Mode::S, 100);
        std::printf("  T2 申请 S(%s) → %s   << S 与 S 兼容，不用等 ✔\n", R.c_str(), acqName(a));
    });
    th.join();

    auto t0 = std::chrono::steady_clock::now();
    Acquire a3 = lm.acquire(3, R, Mode::X, 200);   // X 与 S 冲突，必须等
    long long waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
    std::printf("  T3 申请 X(%s) → %s，实际阻塞 %lld ms  << X 被 S 挡住，等不到就超时撤\n",
                R.c_str(), acqName(a3), waited);
    lm.dump("T1/T2 持 S，T3 刚超时退出");

    lm.releaseAll(1);
    lm.releaseAll(2);
    a3 = lm.acquire(3, R, Mode::X, 100);
    std::printf("  释放 T1/T2 后 T3 再申请 X → %s\n", acqName(a3));
    lm.dump("只剩 T3 持 X");

    // 锁升级：T5 手里有 S，想要 X
    lm.releaseAll(3);
    lm.acquire(5, R, Mode::S, 100);
    auto up = lm.upgrade(5, R, 100);
    std::printf("  T5 持 S → 升级为 X → %s   << 没有别的持有者，升级直接成功\n", acqName(up));
    lm.dump("T5 升级完成");
    lm.releaseAll(5);
    std::printf("  类比：S=lock_shared()，X=lock()，升级=手里握着 shared 再去 lock()。\n");
    std::printf("        别的共享者不撒手，你就得等 —— 这就是升级死锁的温床，见演示 3。\n");
}

/* =====================================================================
 *  演示 2：不开检测的死锁 —— 两个人互相等，只能靠超时收尸
 * ===================================================================== */
static void demo2_deadlock_timeout() {
    std::printf("\n===== 演示 2：反序加锁造死锁（关闭检测，靠超时兜底）=====\n");
    LockManager lm(false);
    const std::string A = "row_account_A", B = "row_account_B";
    const int kTimeout = 400;
    std::atomic<int> done{0}, to{0};
    Rendezvous rv;

    auto worker = [&](TxnId t, const std::string& first, const std::string& second) {
        auto a1 = lm.acquire(t, first, Mode::X, kTimeout);
        if (a1 != Acquire::Granted) { ++to; return; }
        log("T" + std::to_string(t), "拿到 " + first + "，准备去要 " + second);
        rv.sync("hold", 2);                    // 会合：确保双方都攥着第一把锁
        auto t0 = std::chrono::steady_clock::now();
        auto a2 = lm.acquire(t, second, Mode::X, kTimeout);
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
        log("T" + std::to_string(t), "要 " + second + " → " + acqName(a2) +
                                     "，白等了 " + std::to_string(ms) + " ms");
        if (a2 == Acquire::Timeout) ++to; else ++done;
        lm.releaseAll(t);
    };

    // 中途抓一张"正卡着"的现场：两边各持一把 X、各在对方那行上排队
    std::thread dumper([&] {
        sleepMs(kTimeout / 2);
        lm.dump("★ 死锁现场（还没超时，两边正卡着）");
    });
    std::thread ta(worker, 1, A, B), tb(worker, 2, B, A);
    ta.join(); tb.join();
    dumper.join();
    lm.dump("双方都超时退出之后");
    std::printf("  结果：成功 %d 个，超时 %d 个。\n", done.load(), to.load());
    std::printf("  两边都在等对方的第二把锁，谁也不撒手；没有死锁检测的话，"
                "只能干等超时来收尸（两个都超时也是可能的）。\n");
    std::printf("  也就是说：死锁期间【两个人一起卡住】%d ms，而且第三个事务想进来得陪着卡。\n",
                kTimeout);
    std::printf("  超时法的毛病：短了误伤正常等待（锁本来一会儿就放），长了响应慢。\n");
}

/* =====================================================================
 *  演示 3：开等待图找环 —— 死锁当场被抓，回滚一个，另一个立刻跑通
 * ===================================================================== */
static void demo3_deadlock_detected() {
    std::printf("\n===== 演示 3：等待图找环检测死锁（开启检测）=====\n");
    LockManager lm(true);
    const std::string A = "row_account_A", B = "row_account_B";
    const int kTimeout = 5000;              // 有检测就不怕等：真死锁 1ms 内就被掐了
    std::atomic<int> committed{0}, victims{0};
    std::atomic<long long> victimElapsed{-1};
    Rendezvous rv;

    auto worker = [&](TxnId t, const std::string& first, const std::string& second) {
        auto t0 = std::chrono::steady_clock::now();
        lm.acquire(t, first, Mode::X, kTimeout);
        log("T" + std::to_string(t), "拿到 " + first);
        rv.sync("hold", 2);
        auto a2 = lm.acquire(t, second, Mode::X, kTimeout);
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
        if (a2 == Acquire::Deadlock || a2 == Acquire::Aborted) {
            victims.fetch_add(1);
            victimElapsed.store(ms);   // 谁最后被记为受害者，就报谁的时间
            log("T" + std::to_string(t), "检测到死锁，我是受害者 → 回滚（总耗时 " +
                                         std::to_string(ms) + " ms）");
            lm.releaseAll(t);
            lm.forget(t);
            // 重试一次：这次对方已经提交，锁是空的
            auto b1 = lm.acquire(t, first, Mode::X, kTimeout);
            auto b2 = lm.acquire(t, second, Mode::X, kTimeout);
            if (b1 == Acquire::Granted && b2 == Acquire::Granted) {
                log("T" + std::to_string(t), "重试 → 两把锁都拿到，提交 ✔");
                ++committed;
            }
            lm.releaseAll(t);
        } else {
            log("T" + std::to_string(t), "两把锁都拿到，提交 ✔（总耗时 " + std::to_string(ms) + " ms）");
            ++committed;
            lm.releaseAll(t);
        }
    };

    std::thread ta(worker, 1, A, B), tb(worker, 2, B, A);
    ta.join(); tb.join();
    lm.printCycle();
    std::printf("  统计：提交成功 %d 个，被回滚 %d 个，受害者从发起到被掐死耗时 %lld ms。\n",
                committed.load(), victims.load(), victimElapsed.load());
    std::printf("  对比演示 2：同样时序，检测法没有 400 ms 的死等，环一形成立刻破。\n");
    std::printf("  注意【重试单位是整个事务】：受害者不能只补第二把锁，得从头再来。\n");

    // 升级死锁：两边都持 S，两边都要升级 X —— 环出现在"升级等待"上
    std::printf("\n  【彩蛋】升级死锁：T1/T2 同时持 S 又同时想升 X\n");
    LockManager lm2(true);
    Rendezvous rv2;
    std::atomic<int> upVictims{0};
    auto upgrader = [&](TxnId t) {
        lm2.acquire(t, "row_cache_key", Mode::S, 1000);
        rv2.sync("S_held", 2);                       // 确保两边 S 都拿到了
        auto up = lm2.upgrade(t, "row_cache_key", 1000);
        if (up == Acquire::Deadlock || up == Acquire::Aborted) {
            ++upVictims;
            log("T" + std::to_string(t), "升级 S→X 时发现死锁，被回滚 ✔");
        } else {
            log("T" + std::to_string(t), "升级成功（对方被回滚后只剩我）");
        }
        lm2.releaseAll(t);
    };
    std::thread u1(upgrader, 1), u2(upgrader, 2);
    u1.join(); u2.join();
    lm2.printCycle();
    std::printf("  升级死锁抓到受害者 %d 个。MySQL 的 S→X 升级死锁真在生产里常见，别当段子看。\n",
                upVictims.load());
}

/* =====================================================================
 *  演示 4：加锁顺序 —— 随机序 vs 全局排序，死锁次数差多少
 *  （这是"预防"路线：数据库里的 sort key / 单一加锁顺序就这一招）
 * ===================================================================== */
static void demo4_lock_ordering() {
    std::printf("\n===== 演示 4：加锁顺序决定死锁频率（4 线程 × 80 次转账）=====\n");
    const int kAccts = 5, kThreads = 4, kRounds = 80;

    auto runOnce = [&](bool sortedOrder) {
        LockManager lm(true);
        std::atomic<TxnId> nextTxn{1};
        std::atomic<int> deadlocks{0}, okTxn{0}, retries{0};
        long long total[5] = {100, 100, 100, 100, 100};   // 只在线程退出后读，无需加锁
        std::vector<std::thread> ts;

        for (int k = 0; k < kThreads; ++k) {
            ts.emplace_back([&, k] {
                std::mt19937 rng(1234 + k);
                for (int r = 0; r < kRounds; ++r) {
                    int i = static_cast<int>(rng() % kAccts);
                    int j = static_cast<int>(rng() % kAccts);
                    if (i == j) j = (j + 1) % kAccts;
                    for (int attempt = 0;; ++attempt) {
                        TxnId t = nextTxn.fetch_add(1);
                        int a = i, b = j;
                        if (sortedOrder) { if (a > b) std::swap(a, b); }   // 全局固定顺序
                        std::string ra = "acct_" + std::to_string(a);
                        std::string rb = "acct_" + std::to_string(b);
                        auto x1 = lm.acquire(t, ra, Mode::X, 800);
                        if (x1 == Acquire::Granted) busyUs(120);   // 业务窗口：边算边攥着锁
                        auto x2 = (x1 == Acquire::Granted) ? lm.acquire(t, rb, Mode::X, 800)
                                                           : x1;
                        if (x1 == Acquire::Granted && x2 == Acquire::Granted) {
                            total[a] -= 1; total[b] += 1;
                            lm.releaseAll(t);
                            ++okTxn;
                            break;
                        }
                        lm.releaseAll(t);
                        lm.forget(t);
                        ++deadlocks;
                        if (attempt > 0) ++retries;
                        // 死锁/超时后重试，最多 50 次防止理论上的活锁
                        if (attempt > 50) { std::printf("  ✘ 重试上限，退出\n"); break; }
                    }
                }
            });
        }
        for (auto& th : ts) th.join();
        long long sum = 0;
        for (int i = 0; i < kAccts; ++i) sum += total[i];
        std::printf("  %-10s：提交 %4d 次，死锁回滚 %3d 次，死锁率 %5.2f%%，总额 %lld %s\n",
                    sortedOrder ? "排序加锁" : "随机加锁",
                    okTxn.load(), deadlocks.load(),
                    100.0 * deadlocks.load() / (okTxn.load() + deadlocks.load()),
                    sum, sum == 500 ? "守恒 ✔" : "不守恒 ✘");
        return deadlocks.load();
    };

    int rnd = runOnce(false);
    int srt = runOnce(true);
    std::printf("  结论：随机序死锁 %d 次，排序后 %d 次。\n", rnd, srt);
    std::printf("  一招致胜：所有事务按【同一全序】取锁（资源名排序、按 id 排序、分片路由）。\n");
    std::printf("  等待图找环是【事后抢救】，固定加锁顺序是【事前预防】，能预防就别抢救。\n");
    std::printf("  C++ 同款：std::scoped_lock(m1, m2) 内部就用死锁避免算法统一加锁顺序，\n");
    std::printf("          而手写 lock(m1); lock(m2) 反序就会死 —— 和转账一模一样。\n");
}

/* =====================================================================
 *  演示 5：锁表可观测性 —— 线上排查死锁就是看这两张表
 * ===================================================================== */
static void demo5_observability() {
    std::printf("\n===== 演示 5：把「卡住在等锁」的样子打出来（线上排查同款）=====\n");
    LockManager lm(true);
    Rendezvous rv;
    std::atomic<bool> stop{false};

    // T1 长期持有 X(row_hot_key)，模拟"一个大事务拿着热点行不放"
    lm.acquire(1, "row_hot_key", Mode::X, 100);
    log("T1", "持有 row_hot_key 的 X 锁不放（模拟长事务/未提交）");

    std::vector<std::thread> ts;
    for (int i = 0; i < 3; ++i) {
        ts.emplace_back([&, i] {
            lm.acquire(10 + i, "row_hot_key", Mode::S, 600);   // 都会排进等待队列
        });
    }
    sleepMs(60);
    lm.dump("T1 持 X，T10/T11/T12 排队 —— 这就是 information_schema 里看到的样子");
    std::printf("  线上对应：MySQL 的 performance_schema.data_locks / SHOW ENGINE INNODB STATUS，\n");
    std::printf("           PostgreSQL 的 pg_locks + pg_stat_activity（看 wait_event）。\n");
    std::printf("  排查套路：先找【持锁最久】的那个（这里是 T1），再看谁在等它。\n");
    for (auto& t : ts) t.join();
    lm.releaseAll(1);
    stop.store(true);
    (void)stop;
}

/* =====================================================================
 *  main
 * ===================================================================== */
int main() {
    g_t0 = std::chrono::steady_clock::now();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("================================================================================\n");
    std::printf(" 锁管理器与死锁实验室 —— 行锁 ≈ 按 key 分片的 mutex，死锁检测 ≈ 等待图找环\n");
    std::printf("================================================================================\n");

    demo1_semantics();
    demo2_deadlock_timeout();
    demo3_deadlock_detected();
    demo4_lock_ordering();
    demo5_observability();

    std::printf("\n================================================================================\n");
    std::printf(" 老师傅的话\n");
    std::printf("================================================================================\n");
    std::printf(" 1. 今天你写了一个 200 行的锁管理器，就再也不会怕「行锁」这个词了。\n");
    std::printf("    真数据库里多了什么？多粒度锁（表/页/行）、意向锁（IS/IX，用来快速判断\n");
    std::printf("    表级冲突）、gap lock（防幻读）、MVCC（大部分读根本不加锁）。\n");
    std::printf("    骨架就是你今天写的这张锁表。\n\n");
    std::printf(" 2. 死锁不是 bug，是并发业务的常态。真正的设计目标是：\n");
    std::printf("    ① 让它尽量不发生（固定加锁顺序、缩小事务、热点拆分/队列化）；\n");
    std::printf("    ② 发生了能秒级发现（等待图/超时）；\n");
    std::printf("    ③ 发现了能自动恢复（回滚受害者 + 【整事务】重试）。\n");
    std::printf("    三条缺一条，线上就是一个拖垮全站的锁等待雪崩。\n\n");
    std::printf(" 3. 最常见的死锁成因，背下来：\n");
    std::printf("    · 两个入口反序更新同一批账户（今天演示 2）；\n");
    std::printf("    · 一个事务里 for 循环更新一批 id，另一个倒序（顺序不一致）；\n");
    std::printf("    · 二级索引反查：走索引 A 的行序和走索引 B 的行序天然不同；\n");
    std::printf("    · S→X 升级（今天演示 3 彩蛋）。\n");
    std::printf("    解法都指向同一句：让所有路径以同一顺序取锁。\n\n");
    std::printf(" 4. 和 C++ 的对应关系记牢，面试和干活都省事：\n");
    std::printf("    行锁 ↔ 分片 mutex；死锁 ↔ 反序 lock；等待图 ↔ 资源分配图找环；\n");
    std::printf("    锁超时 ↔ try_lock_for；受害者回滚 ↔ 释放全部锁后整段重试。\n\n");
    std::printf(" 练习：\n");
    std::printf("  1. 把演示 4 的 kAccts 从 5 改成 50，死锁率怎么变？为什么？\n");
    std::printf("  2. 给 LockManager 加一个 wait-die / wound-wait 策略（用事务号当时间戳），\n");
    std::printf("     和「检测+回滚」对比：谁的重试次数少？\n");
    std::printf("  3. 加意向锁 IS/IX：模拟「一个事务锁全表、另一个锁某一行」的冲突判定。\n");
    std::printf("  4. 把 waiters 的 FIFO 改成「写者优先」，观察读者会不会被饿死。\n");
    std::printf("================================================================================\n");
    return 0;
}
