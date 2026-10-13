// =====================================================================
//  03_concurrency_cpp.cpp —— 分布式一致性谱系：从强到最终
//
//  编译：g++ -std=c++17 -O2 -Wall -Wextra 03_concurrency_cpp.cpp -o 03_concurrency_cpp.exe
//  运行：./03_concurrency_cpp.exe
//
//  老师傅导读：上一个文件说 LSM 是「把写的账记到读上」，今天说分布式是
//  「把一致性的账记到哪一头」的事。网络一旦分区、复制一旦异步，「数据库
//  里到底是什么」这个问题就没有全局唯一的答案了——只剩下一串承诺，从贵到
//  便宜排成一列，学名叫一致性谱系（consistency spectrum）：
//    强一致（线性化）：所有读者看到同一个单一时间线。贵：写要过全局关卡。
//    因果一致：只保证「有因果关系的写」按序可见，无关的写各看各的。
//    最终一致：不承诺过程，只承诺「不再写入后终将收敛」。最便宜也最常见。
//  本文件全部用 std::thread + std::atomic 真并发跑，三种异常都会真实发生
//  在输出里——不是我编的故事，是线程自己跑出来的。
// =====================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// =====================================================================
// 演示一：强一致 = 全局关卡 + 单一提交顺序（线性化）
//   先把「自以为安全」的坑踩给你看，再上正确做法。
// =====================================================================

// 坑：变量是 atomic 的，但「读-改-写」三步不是原子的——
// 这是 C++ 程序员最常带进分布式的第一课：安全不等于原子。
// 为了把丢失更新演示成"必然发生"而不是"看运气"，barrier 让所有线程
// 在「读完还没写」的那一秒全员集合，然后再写——写与写必然互相覆盖。
// 现实里不需要栅栏：网络复制的毫秒延迟，天然把"读→写"窗口拉得老宽。
struct LaggyBank {
    static constexpr int kThreads = 8;
    std::atomic<long long> balance{0};
    std::atomic<int> arrived{0};
    std::atomic<bool> gate{false};
    void bump() {
        long long cur = balance.load(); // ①读
        // ②到齐 8 个才放行（模拟复制延迟那种超宽的「读→写」窗口）
        if (arrived.fetch_add(1) + 1 == kThreads) gate.store(true);
        while (!gate.load()) std::this_thread::yield();
        balance.store(cur + 1); // ③写：8 家读到的都是同一个旧值，写全互相覆盖
    }
};

// 正解：全局互斥 + 单调提交序号。所有写穿过同一道门，
// 门后记一本流水账——这本账就是「线性化顺序」：任何观察者看到的
// 都是同一个总顺序，读到的永远是账本最新态。
struct StrongBank {
    std::mutex m;
    long long balance = 0;
    uint64_t seq = 0;
    std::vector<std::string> commitLog; // 线性化顺序的流水账

    void adjust(long long delta, int tid) {
        std::lock_guard<std::mutex> lk(m);
        balance += delta;
        seq++;
        commitLog.push_back("seq " + std::to_string(seq) + ": T" + std::to_string(tid) +
                            " 改 balance → " + std::to_string(balance));
    }
    long long read() {
        std::lock_guard<std::mutex> lk(m);
        return balance; // 读也走这道门 → 永远读到已提交的最新值
    }
};

static void demo1_strong() {
    std::cout << "[演示一] 强一致：全局互斥 + 线性化顺序\n\n";
    std::cout << "  先看坑：atomic 变量 + 非原子的读改写。8 个线程各做 1 次 +1，\n";
    std::cout << "        用栅栏把「读完还没写」的窗口撑开（模拟复制延迟）：\n";
    {
        LaggyBank bank;
        std::vector<std::thread> ts;
        for (int t = 0; t < LaggyBank::kThreads; t++) ts.emplace_back([&] { bank.bump(); });
        for (auto& t : ts) t.join();
        std::cout << "    期望 8，实际 " << bank.balance.load() << " —— 丢了 "
                  << LaggyBank::kThreads - bank.balance.load() << " 次更新（8 次写全踩着同一个旧值）\n";
        std::cout << "    ↑ 单变量是原子的，「看再改」不是。分布式里的丢失更新，\n";
        std::cout << "      和这个竞态在本质上是同一道题（第 3 关事务课的老朋友）。\n\n";
    }
    std::cout << "  正解：所有写穿过同一把全局锁，顺带得到一本单调流水账\n";
    {
        StrongBank bank;
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; t++)
            ts.emplace_back([&, t] { for (int i = 0; i < 250; i++) bank.adjust(1, t); });
        for (auto& t : ts) t.join();
        std::cout << "    期望 1000，实际 " << bank.read() << " ✔\n";
        std::cout << "    流水账片段（看线程是怎么交错进同一序列的）：\n";
        for (size_t i = 0; i < 6 && i < bank.commitLog.size(); i++)
            std::cout << "      " << bank.commitLog[i] << "\n";
        std::cout << "      ...（共 " << bank.commitLog.size() << " 条，序号严格单调、无空洞）\n";
        std::cout << "    ↑ 这就是线性化（linearizability）的全部含义：并发操作多乱，\n";
        std::cout << "      外界看到的都等价于「按这本账的某个顺序一个一个执行」。\n";
        std::cout << "      ZooKeeper 的 zxid、Raft 的 (term, index)、MySQL Group Replication\n";
        std::cout << "      的 certification index，全是这本账的不同方言。\n";
        std::cout << "      代价：写全过一道门，吞吐有上限、延迟看门人——CP 阵营的日常。\n\n";
    }
}

// =====================================================================
// 演示二：最终一致 = 多副本 + 异步复制
//   写只落本地，后台慢慢传播。写完立刻去「别的副本」读，
//   read-your-writes（自己写的自己得先看见）就会真的被违背。
// =====================================================================
class EventualCluster {
    struct Task {
        std::string k, v;
    };
    struct Replica {
        std::mutex m;
        std::map<std::string, std::string> data;
        void put(const std::string& k, const std::string& v) {
            std::lock_guard<std::mutex> lk(m);
            data[k] = v;
        }
        std::string get(const std::string& k) {
            std::lock_guard<std::mutex> lk(m);
            auto it = data.find(k);
            return it == data.end() ? "(nil)" : it->second;
        }
    };
    struct Channel { // 副本 i 的收件箱：一个线程专职「慢吞吞地抄送」
        std::mutex m;
        std::condition_variable cv;
        std::queue<Task> q;
        bool stop = false;
    };

public:
    EventualCluster(int n, unsigned seed) : rng_(seed), delayDist_(0, 10) {
        for (int i = 0; i < n; i++) reps_.push_back(std::make_unique<Replica>());
        for (int i = 0; i < n; i++) chans_.push_back(std::make_unique<Channel>());
        for (int i = 0; i < n; i++) workers_.emplace_back([this, i] { pump(i); });
    }
    ~EventualCluster() {
        for (auto& c : chans_) {
            {
                std::lock_guard<std::mutex> lk(c->m);
                c->stop = true;
            }
            c->cv.notify_all();
        }
        for (auto& t : workers_) t.join();
    }

    // 写：只写本地 + 塞进别人的收件箱，立刻返回——不等副本确认！
    void write(int to, const std::string& k, const std::string& v) {
        reps_[to]->put(k, v);
        for (int j = 0; j < (int)reps_.size(); j++) {
            if (j == to) continue;
            {
                std::lock_guard<std::mutex> lk(chans_[j]->m);
                chans_[j]->q.push({k, v});
            }
            chans_[j]->cv.notify_one();
        }
    }
    std::string read(int from, const std::string& k) { return reps_[from]->get(k); }

    // 收敛检查：所有副本对 k 的值一致吗？返回已一致的副本数
    int agreeCount(const std::string& k) {
        std::string first = reps_[0]->get(k);
        int n = 0;
        for (auto& r : reps_)
            if (r->get(k) == first) n++;
        return n;
    }

private:
    void pump(int i) {
        while (true) {
            Task t;
            {
                std::unique_lock<std::mutex> lk(chans_[i]->m);
                chans_[i]->cv.wait(lk, [&] { return chans_[i]->stop || !chans_[i]->q.empty(); });
                if (chans_[i]->q.empty()) return; // stop 且队列排空
                t = chans_[i]->q.front();
                chans_[i]->q.pop();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delayDist_(rng_))); // 网络/落盘延迟
            reps_[i]->put(t.k, t.v);
        }
    }

    std::mt19937 rng_;
    std::uniform_int_distribution<int> delayDist_;
    std::vector<std::unique_ptr<Replica>> reps_;
    std::vector<std::unique_ptr<Channel>> chans_;
    std::vector<std::thread> workers_;
};

static void demo2_eventual() {
    std::cout << "\n[演示二] 最终一致：异步复制下，「自己写的自己看不见」是真的\n\n";
    EventualCluster c(3, 7);
    for (int i = 0; i < 3; i++) c.write(i, "k", "v7");        // 三副本先各有初值
    while (c.agreeCount("k") < 3) std::this_thread::sleep_for(5ms); // 等初始值收敛
    std::cout << "  开局三副本都收敛到 k=v7。此后客户端持续「写节点A → 立刻读节点B」\n";

    std::mt19937 g(7);
    // 客户端节奏 0~25ms，和复制延迟（0~10ms）撞车：读得赶得上就看见，赶不上就旧值
    std::uniform_int_distribution<int> nap(0, 25);
    int rounds = 300, violations = 0, shown = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::string last = "v7";
    for (int r = 1; r <= rounds; r++) {
        std::string val = "v" + std::to_string(100 + r);
        c.write(0, "k", val); // 写节点 A（本地成功 = 写「成功」）
        std::this_thread::sleep_for(std::chrono::milliseconds(nap(g)));
        std::string got = c.read(1, "k"); // 立刻读节点 B
        if (got != val) {
            violations++;
            if (shown < 3) {
                std::cout << "    违背样例：刚写 k=" << val << " 到 A，B 上读到 " << got
                          << (got == last ? "（上一版的旧值）" : "（更早的旧值）") << "\n";
                shown++;
            }
        }
        last = val;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "  共 " << rounds << " 轮「写 A 读 B」：" << violations << " 轮没看见自己的写（"
              << (double)violations * 100 / rounds << "%），耗时 " << ms << " ms\n";

    // 最终收敛
    auto tc = std::chrono::steady_clock::now();
    while (c.agreeCount("k") < 3) std::this_thread::sleep_for(5ms);
    double conv = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tc).count();
    std::cout << "  停止写入后 " << conv << " ms，三副本收敛到同一值 " << c.read(2, "k")
              << " ✔ 这就是「最终一致」兑现的承诺：不管过程，只管终点。\n";
    std::cout << "  ↑ 生产对应物：MySQL 主从异步复制（CHANGE MASTER 不带 semisync）、\n";
    std::cout << "    DynamoDB 最终一致读、Cassandra CL=1。电商「下单成功，刷新列表却没有」\n";
    std::cout << "    的工单，根子就是这一行「写 A 读 B」。缓解手段叫会话保证：\n";
    std::cout << "    read-your-writes（写后短时间读主/读自己副本）、monotonic-reads\n";
    std::cout << "    （别从新副本跳回旧副本）——都是客户端打的补丁，不是集群的承诺。\n";
}

// =====================================================================
// 演示三：因果一致 = 给每个写盖单调版本号，旧值休想翻新值
//   复制通道会乱序（网络重排、多路径），没有版本号的副本
//   「谁后到谁说了算」，值会时光倒流；有版本号，旧值到站直接拦下。
// =====================================================================
struct Versioned {
    uint64_t ver = 0;
    std::string val;
};

class CausalReplica {
public:
    explicit CausalReplica(bool useVersion) : useVersion_(useVersion) {}

    // 返回 true = 应用；false = 识别为旧值、拦下
    bool apply(uint64_t v, const std::string& val, std::string* reason) {
        std::lock_guard<std::mutex> lk(m_);
        applied_++;
        if (useVersion_ && v <= cur_.ver) {
            if (reason)
                *reason = "拦下旧版本 v" + std::to_string(v) + "（本机已有 v" +
                          std::to_string(cur_.ver) + " = " + cur_.val + "）";
            blocked_++;
            return false;
        }
        cur_ = {v, val}; // 无版本号模式：直接覆盖 = 「按到达顺序 last-writer-wins」
        return true;
    }
    Versioned snapshot() {
        std::lock_guard<std::mutex> lk(m_);
        return cur_;
    }
    long long blocked() const { return blocked_; }

private:
    bool useVersion_;
    std::mutex m_;
    Versioned cur_;
    long long applied_ = 0, blocked_ = 0;
};

// 模拟「两条复制通道、快慢不同」：v 小的走慢通道，v 大的走快通道，
// 到站顺序必然颠倒——网络重排就这么复现，不用碰运气。
static void demo3_causal() {
    std::cout << "\n[演示三] 因果一致：单调版本号挡住旧值（复现网络重排）\n\n";

    // 对照组：无版本号副本
    CausalReplica naive(false), guarded(true);
    std::atomic<uint64_t> clock{0};

    struct Job {
        uint64_t v;
        std::string val;
        int slowMs;
    };
    // 三个写按 v1<v2<v3 的顺序「提交」，但复制延迟 60/10/30ms → 到站顺序 v2,v3,v1
    std::vector<Job> jobs = {{0, "draft-A", 60}, {0, "draft-B", 10}, {0, "draft-C", 30}};
    for (auto& j : jobs) j.v = ++clock; // 提交序号递增

    static std::mutex printMtx; // 多线程打印串行化，别让三行日志绞成一团
    auto replay = [&](CausalReplica& r, const char* name) {
        std::vector<std::thread> ts;
        std::atomic<int> blockedLogged{0};
        for (auto& j : jobs) {
            ts.emplace_back([&, j, name] {
                std::this_thread::sleep_for(std::chrono::milliseconds(j.slowMs)); // 通道快慢
                std::string why;
                bool ok = r.apply(j.v, j.val, &why);
                if (!ok && blockedLogged < 3) {
                    std::lock_guard<std::mutex> lk(printMtx);
                    std::cout << "    [" << name << "] " << why << "\n";
                    blockedLogged++;
                }
            });
        }
        for (auto& t : ts) t.join();
    };

    std::cout << "  三个写 v1→v2→v3 依次提交，复制通道延迟 60/10/30ms（乱序到站）\n";
    std::cout << "  对照组：副本不查版本（谁后到谁赢）：\n";
    replay(naive, "无版本");
    auto nv = naive.snapshot();
    std::cout << "    最终值 = " << nv.val << "（v" << nv.ver << "）"
              << (nv.ver == 1 ? "  ← 时光倒流！最新的 v3 被迟到的 v1 覆盖了" : "") << "\n";
    std::cout << "  实验组：副本带单调版本检查：\n";
    replay(guarded, "有版本");
    auto gv = guarded.snapshot();
    std::cout << "    最终值 = " << gv.val << "（v" << gv.ver << "）✔ 最新值守住了；"
              << "共拦下旧值 " << guarded.blocked() << " 次\n";
    std::cout << "  ↑ 版本号就是「逻辑时钟」：Lamport 1978 年的论文，今天还在\n";
    std::cout << "    DynamoDB 的 @current-version、etcd 的 revision、Spanner 的 commit timestamp\n";
    std::cout << "    里活着。单计数器只能保证「新值不被旧值覆盖」；要判断两个写\n";
    std::cout << "    谁因果在先谁并发无关，得上向量时钟——那是 cap_visualizer.html\n";
    std::cout << "    里分区恢复后合并冲突用的东西，去动画里亲手捏一次就懂。\n";
}

// =====================================================================
int main() {
    std::cout << "================================================================\n";
    std::cout << "  一致性谱系：强一致 / 最终一致 / 因果一致（全部真并发复现）\n";
    std::cout << "================================================================\n\n";
    demo1_strong();
    demo2_eventual();
    demo3_causal();

    std::cout << "\n================================================================\n"
                 "  老师傅的话\n"
                 "================================================================\n"
                 " 1. 把三个演示排成一行，就是一条「承诺 vs 代价」的滑梯：\n"
                 "      线性化：单一时间线，写走全局关卡 → 分区时宁可拒写（CP）；\n"
                 "      因果：相关写按序可见，版本号挡住旧值 → 中间档；\n"
                 "      最终一致：写本地即返，异步收敛 → 分区时照常写（AP），\n"
                 "             但 read-your-writes 违背会像演示二那样真实发生。\n"
                 " 2. 演示一那个 atomic 坑请刻在腕上：线程安全的最小单位是\n"
                 "    「操作」不是「变量」。分布式的丢失更新、CAS 重试、乐观锁\n"
                 "    版本号，全是在给这条规律打补丁。\n"
                 " 3. 工程上的真实系统几乎都是混档的：主库强一致 + 从库最终一致，\n"
                 "    会话内 read-your-writes，关键路径用版本号/CAS 兜底。\n"
                 "    「你们系统什么一致性级别」是句病句，该问的是：\n"
                 "    「每条关键读写路径，我承诺了什么、愿意为什么买单」。\n"
                 " 4. 下一步打开 cap_visualizer.html：把「分区」这一维加进来，\n"
                 "    看 CP/AP 在网络断掉的那一刻各自怎么做人，向量时钟怎么合并\n"
                 "    两边各写各的账。演示三里被拦下的旧值，动画里会再来一遍。\n";
    return 0;
}
