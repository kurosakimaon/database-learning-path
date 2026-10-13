// =====================================================================
//  02_lsm_vs_btree.cpp —— 手写简化 LSM-Tree，对拆 B+ 树的写路径
//
//  编译：g++ -std=c++17 -O2 -Wall -Wextra 02_lsm_vs_btree.cpp -o 02_lsm_vs_btree.exe
//  运行：./02_lsm_vs_btree.exe          （会在当前目录建 lsm_tmp_data/，跑完自动删）
//
//  老师傅先给一句话总纲：
//    B+ 树（InnoDB 的路子）是「就地归档」：每条写入都要飞到磁盘上它该待的
//    那一页，改完再放回去——读快（三四次页读直达），但随机写要把磁盘头满
//    仓库乱飞，页还会越改越碎。
//    LSM-Tree（RocksDB/LevelDB 的路子）是「先堆工作台」：写入只进内存
//    memtable，攒满整批「顺序」刷成不可变的有序小文件（SST），之后靠后台
//    compaction 定期「理货」合并。写快（全是顺序 IO），读却要一个文件一个
//    文件地翻——读慢。
//    一个把代价记在写上，一个把代价记在读上，这就是两种引擎分家的根。
//
//  本文件真跑 10 万条写入，把「写放大、读放大、顺序/随机 IO」全部数出来。
// =====================================================================

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <chrono>

// ---------------------------------------------------------------------
// 通用小工具：key/value 的生成、字节可读化
// ---------------------------------------------------------------------
static std::string makeKey(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "user:%07d", i);
    return buf;
}
static const std::string kValue(40, 'v'); // 固定 40 字节 value，方便算账

static std::string mb(long long bytes) {
    std::ostringstream os;
    double m = static_cast<double>(bytes) / (1024.0 * 1024.0);
    os.precision(m < 10 ? 2 : 1);
    os << std::fixed << m << " MB";
    return os.str();
}

// =====================================================================
// 第一部分：简化 LSM-Tree
//   · memtable = std::map（RocksDB 真实用的 skiplist，教学用 map 等价）
//   · flush    = 把 memtable 按序写成一个真实 SST 小文件（顺序写）
//   · compaction = k 路归并多个 SST，重写成下一层的更大有序段
//   · 查询     = memtable → L0 每个段 → L1..Lk（每层内键不重叠，二分即可）
// =====================================================================

// 简化布隆过滤器：判断「这个 key 肯定不在这个 SST 里」。
// RocksDB 每个 SST 都带，它把 LSM 的读放大砍掉一大截（下面会数给你看）。
struct Bloom {
    std::vector<bool> bits;
    size_t m_ = 0;
    Bloom() = default;
    template <class It>
    Bloom(It begin, It end) {
        size_t n = static_cast<size_t>(std::distance(begin, end));
        m_ = std::max<size_t>(n * 10, 64); // 每元素 10 bit，约 1% 误判率
        bits.assign(m_, false);
        for (It it = begin; it != end; ++it) add(it->first);
    }
    static uint64_t h1(const std::string& k) { return std::hash<std::string>{}(k); }
    static uint64_t h2(const std::string& k) { return h1(k) * 0x9E3779B97F4A7C15ull + 12345; }
    void add(const std::string& k) {
        bits[h1(k) % m_] = true;
        bits[h2(k) % m_] = true;
    }
    bool maybe(const std::string& k) const {
        return bits[h1(k) % m_] && bits[h2(k) % m_];
    }
};

struct SSTable {
    std::string path;                                        // 真实磁盘文件
    std::vector<std::pair<std::string, std::string>> data;   // 内存副本（真实引擎靠页缓存+索引块，思路一致）
    std::string minKey, maxKey;
    long long bytes = 0;                                     // 文件实际字节数
    Bloom bloom;
};

class LSMTree {
public:
    // 参数：memtable 条数上限、L0 段数上限、各层字节上限（base * 10^层）
    void init(const std::string& dir, size_t memLimit = 4096, size_t l0Fanout = 4) {
        dir_ = dir;
        memLimit_ = memLimit;
        l0Fanout_ = l0Fanout;
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        levels_.resize(6); // L0..L5，够用
    }
    void shutdown() { std::filesystem::remove_all(dir_); } // 教学程序：跑完清场

    void put(const std::string& k, const std::string& v) {
        mem_[k] = v;                        // ① 只写内存：这条路径零磁盘 IO
        userBytes_ += k.size() + v.size();  //    这也是「用户数据量」的记账口径
        if (mem_.size() >= memLimit_) flush();
    }

    // 查询：memtable → L0（段间键可重叠，逐段试）→ L1..Lk（层内键不重叠）
    // 返回是否命中；顺便把「探测了几段、真读了几个页」记进计数器
    bool get(const std::string& k, std::string* out) {
        auto it = mem_.find(k);
        if (it != mem_.end()) { *out = it->second; return true; } // memtable 命中：零 IO

        for (auto& s : levels_[0]) {                // L0：最上层，越靠前越新
            probes_++;
            if (!useBloom_ || s->bloom.maybe(k)) { // 布隆说「肯定没有」→ 直接跳过，一次盘都不碰
                bloomPassed_++;
                if (probeSST(*s, k, out)) { reads_++; return true; }
            }
        }
        for (size_t lv = 1; lv < levels_.size(); ++lv) {
            for (auto& s : levels_[lv]) {
                if (k < s->minKey || k > s->maxKey) continue; // 键范围先筛（真实引擎是元数据内存态，免费）
                probes_++;
                if (!useBloom_ || s->bloom.maybe(k)) {
                    bloomPassed_++;
                    if (probeSST(*s, k, out)) { reads_++; return true; }
                }
            }
        }
        return false;
    }

    // ---- 统计口径（对外汇报用）----
    long long userBytes() const { return userBytes_; }
    long long bytesWritten() const { return bytesWritten_; }   // 所有落盘写（flush+compaction），全顺序
    long long flushCount() const { return flushCount_; }
    long long compactionCount() const { return compactionCount_; }
    long long segmentProbes() const { return probes_; }        // 查询共探测过多少个段
    long long bloomPassedProbes() const { return bloomPassed_;}// 其中布隆放行、真去翻文件的段数
    long long dataPageReads() const { return reads_; }         // 真「读中数据」的次数
    void setUseBloom(bool b) { useBloom_ = b; }
    size_t levelSegCount(size_t lv) const { return levels_[lv].size(); }

    // 打印当前各层形状（小演示用）
    void dumpShape() const {
        std::cout << "    memtable=" << mem_.size() << " 条";
        for (size_t lv = 0; lv < levels_.size(); ++lv)
            if (!levels_[lv].empty()) {
                std::cout << " | L" << lv << ": " << levels_[lv].size() << " 段 "
                          << mb(levelBytes(lv));
            }
        std::cout << "\n";
    }

private:
    long long levelBytes(size_t lv) const {
        long long b = 0;
        for (auto& s : levels_[lv]) b += s->bytes;
        return b;
    }
    // 层字节上限：L1=4MB，L2=40MB，L3=400MB……（十倍一层，LevelDB 原味）
    long long tierCap(size_t lv) const {
        if (lv <= 1) return 4LL * 1024 * 1024;
        long long c = 4LL * 1024 * 1024;
        for (size_t i = 2; i <= lv; ++i) c *= 10;
        return c;
    }

    static bool probeSST(const SSTable& s, const std::string& k, std::string* out) {
        // 内存二分模拟「在该 SST 的索引块里找键」；找到算读了一个数据页。
        auto it = std::lower_bound(s.data.begin(), s.data.end(), k,
                                   [](const std::pair<std::string, std::string>& p, const std::string& key) {
                                       return p.first < key;
                                   });
        if (it != s.data.end() && it->first == k) { *out = it->second; return true; }
        return false;
    }

    // 写一个 SST 文件（顺序写！），并把字节数记账
    std::unique_ptr<SSTable> writeSST(int level,
                                      std::vector<std::pair<std::string, std::string>>&& data) {
        if (data.empty()) return nullptr;
        auto s = std::make_unique<SSTable>();
        s->path = dir_ + "/L" + std::to_string(level) + "_" + std::to_string(sstSeq_++) + ".sst";
        s->data = std::move(data);
        std::ofstream f(s->path, std::ios::binary);
        for (auto& kv : s->data) f << kv.first << '\t' << kv.second << '\n';
        f.close();
        s->bytes = static_cast<long long>(std::filesystem::file_size(s->path));
        s->minKey = s->data.front().first;
        s->maxKey = s->data.back().first;
        s->bloom = Bloom(s->data.begin(), s->data.end());
        bytesWritten_ += s->bytes; // 全部记成「顺序写」——LSM 落盘永远是整段顺序写
        return s;
    }

    void flush() {
        std::vector<std::pair<std::string, std::string>> sorted;
        sorted.reserve(mem_.size());
        for (auto& kv : mem_) sorted.emplace_back(kv.first, kv.second); // map 本身有序
        mem_.clear();
        auto s = writeSST(0, std::move(sorted));
        levels_[0].insert(levels_[0].begin(), std::move(s)); // 新的插到最前（查询先撞最新的）
        flushCount_++;
        maybeCompact(0);
    }

    void maybeCompact(size_t lv) {
        bool need = (lv == 0) ? levels_[0].size() >= l0Fanout_
                              : levelBytes(lv) > tierCap(lv);
        if (!need || lv + 1 >= levels_.size()) return; // 顶层塞不下就让它超着（真实引擎会限速写入而非无限压）
        compact(lv, lv + 1);
        maybeCompact(lv + 1); // 连锁反应：下一层被撑爆就继续往下压
    }

    // 把 src 层的全部段 + dst 层与之键范围重叠的段，k 路归并成新的有序段写进 dst。
    // 「理货」：老段全部报废（这些被删的旧文件字节，就是写放大的大头之一）。
    void compact(size_t src, size_t dst) {
        std::vector<SSTable*> inputs;
        std::unordered_set<std::string> drop; // 待报废文件
        std::string lo, hi;
        for (auto& s : levels_[src]) {
            inputs.push_back(s.get());
            if (inputs.size() == 1) { lo = s->minKey; hi = s->maxKey; }
            else { lo = std::min(lo, s->minKey); hi = std::max(hi, s->maxKey); }
        }
        // L0 段间键可重叠；src>=1 时层内不重叠，取全部即全量合并（教学简化）
        std::vector<std::unique_ptr<SSTable>> keepDst;
        for (auto& s : levels_[dst]) {
            if (s->maxKey < lo || s->minKey > hi) keepDst.push_back(std::move(s)); // 范围不相交，不动它
            else inputs.push_back(s.get());
        }

        // ---- k 路归并：小顶堆，(key 小的先出；同 key 时输入序靠前的先出 = 新数据赢) ----
        struct Item {
            size_t i;      // 在该 SST 内的下标
            size_t order;  // 输入序：L0 越靠前越新，层越低越旧
            SSTable* s;
        };
        struct CmpGreater {
            bool operator()(const Item& a, const Item& b) const {
                const std::string& ka = a.s->data[a.i].first;
                const std::string& kb = b.s->data[b.i].first;
                if (ka != kb) return ka > kb;   // 小顶堆
                return a.order > b.order;       // 同键取更新的
            }
        };
        std::priority_queue<Item, std::vector<Item>, CmpGreater> pq;
        for (size_t o = 0; o < inputs.size(); ++o)
            if (!inputs[o]->data.empty()) pq.push({0, o, inputs[o]});

        std::vector<std::pair<std::string, std::string>> merged;
        std::string lastKey;
        bool hasLast = false;
        while (!pq.empty()) {
            Item it = pq.top(); pq.pop();
            const auto& kv = it.s->data[it.i];
            // 归并要读遍所有输入段——这些字节是 compaction 的「读账」
            if (!hasLast || kv.first != lastKey) {
                merged.push_back(kv);
                lastKey = kv.first;
                hasLast = true;
            }
            it.i++;
            if (it.i < it.s->data.size()) pq.push(it);
        }
        // 旧文件报废（真实引擎是延迟删除，等没人引用再删——这里直接从账面扣掉）
        for (auto* s : inputs) drop.insert(s->path);
        levels_[src].clear();
        std::vector<std::unique_ptr<SSTable>> newDst;
        for (auto& s : keepDst) newDst.push_back(std::move(s));

        // 输出按 dst 层容量切段，保持层内键不重叠
        long long segLimit = (dst == 0) ? tierCap(1) : tierCap(dst);
        std::vector<std::pair<std::string, std::string>> buf;
        long long bufBytes = 0;
        auto flushBuf = [&]() {
            if (buf.empty()) return;
            auto s = writeSST(static_cast<int>(dst), std::move(buf));
            newDst.push_back(std::move(s));
            buf.clear();
            bufBytes = 0;
        };
        for (auto& kv : merged) {
            buf.emplace_back(kv);
            bufBytes += static_cast<long long>(kv.first.size() + kv.second.size() + 2);
            if (bufBytes >= segLimit) flushBuf();
        }
        flushBuf();
        std::sort(newDst.begin(), newDst.end(),
                  [](const std::unique_ptr<SSTable>& a, const std::unique_ptr<SSTable>& b) {
                      return a->minKey < b->minKey;
                  });
        levels_[dst] = std::move(newDst);
        for (const auto& p : drop) std::filesystem::remove(p);
        compactionCount_++;
    }

    std::string dir_;
    size_t memLimit_ = 4096, l0Fanout_ = 4;
    int sstSeq_ = 0;
    bool useBloom_ = true;
    std::map<std::string, std::string> mem_;
    std::vector<std::vector<std::unique_ptr<SSTable>>> levels_;
    long long userBytes_ = 0, bytesWritten_ = 0;
    long long flushCount_ = 0, compactionCount_ = 0;
    long long probes_ = 0, bloomPassed_ = 0, reads_ = 0;
};

// =====================================================================
// 第二部分：简化 B+ 树（InnoDB 的骨架）
//   只实现 insert/get，但把「写路径碰了几页」一笔笔记账：
//   · 下降时每经过一个节点 = 读一个页（除非命中「热点页缓存」）
//   · 叶子写回 = 随机写一个页；分裂时新页 + 父节点改键 = 又写两页
//   顺序写优化：维护 lastLeaf 热点指针——递增主键一路沿右脊下行，
//   右脊页全在缓存里，这就是 InnoDB 对自增主键友好、对随机 UUID 头疼的
//   底层原因。
// =====================================================================
class BPlusTree {
public:
    static constexpr size_t kMaxKeys = 32; // 一页 32 键（InnoDB 16KB 页大约装几百键，量级即可）

    struct Page {
        bool leaf = true;
        std::vector<std::string> keys;
        std::vector<std::string> vals;          // 叶子专用，和 keys 平行
        std::vector<Page*> kids;                // 内部节点：kids.size() == keys.size()+1
        Page* next = nullptr;                   // 叶子链表：范围扫描顺藤摸瓜（B+ 树读快的一半原因）
    };

    BPlusTree() { root_ = new Page(); }
    ~BPlusTree() { destroy(root_); }

    struct Stats { long long reads = 0, writes = 0, splits = 0, cacheHits = 0; };

    void insert(const std::string& k, const std::string& v) {
        // 热点页快速通道：键比最右叶子的最大值还大，且那页没满 → 免下降直插。
        // next==nullptr 这个条件不能省：分裂后 lastLeaf_ 可能指在左半页上，
        // 少了它会把更大的键错插进左半页，键就"丢"在错误的叶子里了（第一版真丢）。
        if (lastLeaf_ && lastLeaf_->leaf && lastLeaf_->next == nullptr &&
            lastLeaf_->keys.size() < kMaxKeys &&
            (!lastLeaf_->keys.empty() && k > lastLeaf_->keys.back())) {
            lastLeaf_->keys.push_back(k);
            lastLeaf_->vals.push_back(v);
            st_.writes++;
            st_.cacheHits++; // 页还在「缓存」里，一次磁盘读都没花
            return;
        }
        Page* splitRight = nullptr;
        std::string upKey;
        insertInto(root_, k, v, &splitRight, &upKey);
        if (splitRight) { // 根分裂 → 树长高一层
            Page* nr = new Page();
            nr->leaf = false;
            nr->keys.push_back(upKey);
            nr->kids = {root_, splitRight};
            root_ = nr;
            st_.writes++;
        }
    }

    bool get(const std::string& k, std::string* out) const {
        Page* p = root_;
        while (true) {
            st_.reads++;
            if (p->leaf) {
                auto it = std::lower_bound(p->keys.begin(), p->keys.end(), k);
                size_t i = it - p->keys.begin();
                if (it != p->keys.end() && *it == k) { *out = p->vals[i]; return true; }
                return false;
            }
            size_t i = std::upper_bound(p->keys.begin(), p->keys.end(), k) - p->keys.begin();
            p = p->kids[i];
        }
    }

    const Stats& stats() const { return st_; }
    size_t depth() const { size_t d = 1; Page* p = root_; while (!p->leaf) { p = p->kids[0]; d++; } return d; }

private:
    // 返回 true = 本页分裂，splitRight/upKey 带回「右半页」和「上推键」
    bool insertInto(Page* p, const std::string& k, const std::string& v,
                    Page** splitRight, std::string* upKey) {
        if (p->leaf) {
            st_.reads++;
            auto it = std::lower_bound(p->keys.begin(), p->keys.end(), k);
            size_t i = it - p->keys.begin();
            if (it != p->keys.end() && *it == k) { p->vals[i] = v; st_.writes++; return false; } // 覆盖更新
            p->keys.insert(it, k);
            p->vals.insert(p->vals.begin() + i, v);
            st_.writes++;
            lastLeaf_ = p;
            if (p->keys.size() <= kMaxKeys) return false;
            return splitLeaf(p, splitRight, upKey);
        }
        st_.reads++;
        size_t i = std::upper_bound(p->keys.begin(), p->keys.end(), k) - p->keys.begin();
        Page* child = p->kids[i];
        Page* right = nullptr;
        std::string up;
        if (insertInto(child, k, v, &right, &up)) {
            p->keys.insert(p->keys.begin() + i, up);
            p->kids.insert(p->kids.begin() + i + 1, right);
            st_.writes++;
            if (p->keys.size() <= kMaxKeys) return false;
            return splitInternal(p, splitRight, upKey);
        }
        return false;
    }

    bool splitLeaf(Page* p, Page** splitRight, std::string* upKey) {
        st_.splits++;
        Page* r = new Page();
        size_t mid = p->keys.size() / 2;
        r->keys.assign(p->keys.begin() + mid, p->keys.end());
        r->vals.assign(p->vals.begin() + mid, p->vals.end());
        p->keys.resize(mid);
        p->vals.resize(mid);
        r->next = p->next;
        p->next = r;
        lastLeaf_ = r; // 新晋最右叶子（r->next 为空才是真的右脊末端）
        *upKey = r->keys.front();
        *splitRight = r;
        st_.writes += 2; // 左右两页都得落盘
        return true;
    }

    bool splitInternal(Page* p, Page** splitRight, std::string* upKey) {
        st_.splits++;
        size_t mid = p->keys.size() / 2;
        *upKey = p->keys[mid];
        Page* r = new Page();
        r->leaf = false;
        r->keys.assign(p->keys.begin() + mid + 1, p->keys.end());
        r->kids.assign(p->kids.begin() + mid + 1, p->kids.end());
        p->keys.resize(mid);
        p->kids.resize(mid + 1);
        *splitRight = r;
        st_.writes += 2;
        return true;
    }

    static void destroy(Page* p) {
        if (!p) return;
        if (!p->leaf) for (auto* c : p->kids) destroy(c);
        delete p;
    }

    Page* root_ = nullptr;
    Page* lastLeaf_ = nullptr; // 「热点页」：顺序写时一直钉在最右叶子
    mutable Stats st_;         // get() 是 const，但计数照记
};

// =====================================================================
// 跑测：10 万条写入 × (LSM / B+ 树) × (顺序 / 随机)
// =====================================================================
using clk = std::chrono::steady_clock;

static void runLSM(const std::vector<int>& order, LSMTree& lsm, double& ms) {
    auto t0 = clk::now();
    for (int i : order) lsm.put(makeKey(i), kValue);
    ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

static void runBTree(const std::vector<int>& order, BPlusTree& bt, double& ms) {
    auto t0 = clk::now();
    for (int i : order) bt.insert(makeKey(i), kValue);
    ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

int main() {
    std::cout << std::fixed;
    std::cout << "================================================================\n";
    std::cout << "  LSM-Tree vs B+ 树：把「写快读慢 / 读快写碎」数出来\n";
    std::cout << "================================================================\n\n";

    // -----------------------------------------------------------------
    // 演示一：先看 LSM 的形状怎么长——写 120 条，memtable 30 条一刷
    // -----------------------------------------------------------------
    std::cout << "[演示一] LSM 的三层流水：memtable 攒满→flush→L0 攒够→compaction\n";
    {
        LSMTree lsm;
        lsm.init("lsm_tmp_data/demo", 30, 4);
        for (int i = 0; i < 120; i++) lsm.put(makeKey(i), kValue);
        std::cout << "  写完 120 条后: "; lsm.dumpShape();
        std::cout << "  flush " << lsm.flushCount() << " 次（每次整批顺序写一个 SST），"
                  << "compaction " << lsm.compactionCount() << " 次（把小段归并下沉）\n";
        std::string v;
        bool hit = lsm.get(makeKey(7), &v);   // 7 早被 flush 走了，得往下层翻
        std::cout << "  查 user:0000007 → " << (hit ? "命中" : "没有")
                  << "（memtable 已清空，实际是翻到盘上某段才找到的）\n";
        std::cout << "  查询账本：共探测 " << lsm.segmentProbes() << " 个段，"
                  << "布隆放行 " << lsm.bloomPassedProbes() << " 个，真读中数据 "
                  << lsm.dataPageReads() << " 次\n";
        lsm.shutdown();
    }

    // -----------------------------------------------------------------
    // 演示二：10 万条真写入，四个赛道
    // -----------------------------------------------------------------
    const int N = 100000;
    std::vector<int> seq(N), rnd(N);
    for (int i = 0; i < N; i++) seq[i] = rnd[i] = i;
    std::mt19937 g(42); // 固定种子，结果可复现
    std::shuffle(rnd.begin(), rnd.end(), g);

    std::cout << "\n[演示二] 10 万条写入（key=user:%07d，value 40B，用户数据约 "
              << mb(static_cast<long long>(N) * (makeKey(0).size() + kValue.size()))
              << "）\n\n";

    // ---- LSM：顺序键 ----
    double msLsmSeq = 0, msLsmRnd = 0, msBtSeq = 0, msBtRnd = 0;
    LSMTree lsmSeq, lsmRnd;
    lsmSeq.init("lsm_tmp_data/seq");
    lsmRnd.init("lsm_tmp_data/rnd");
    runLSM(seq, lsmSeq, msLsmSeq);
    runLSM(rnd, lsmRnd, msLsmRnd);

    // ---- B+ 树：顺序键 / 随机键 ----
    BPlusTree btSeq, btRnd;
    runBTree(seq, btSeq, msBtSeq);
    runBTree(rnd, btRnd, msBtRnd);

    std::cout << "  LSM（写入全走内存+顺序刷盘，键的顺序还是随机，几乎不吃亏）\n";
    std::cout << "    顺序键: " << msLsmSeq << " ms | flush " << lsmSeq.flushCount()
              << " 次 | compaction " << lsmSeq.compactionCount() << " 次 | 落盘 "
              << mb(lsmSeq.bytesWritten()) << " | 写放大 "
              << (double)lsmSeq.bytesWritten() / lsmSeq.userBytes() << "x\n";
    std::cout << "    随机键: " << msLsmRnd << " ms | flush " << lsmRnd.flushCount()
              << " 次 | compaction " << lsmRnd.compactionCount() << " 次 | 落盘 "
              << mb(lsmRnd.bytesWritten()) << " | 写放大 "
              << (double)lsmRnd.bytesWritten() / lsmRnd.userBytes() << "x\n";
    std::cout << "    ↑ 注意：落盘字节全部是【顺序写】——这就是 RocksDB 敢把写 QPS\n";
    std::cout << "      做到机械盘也飞起的根本原因。写放大约 2~4x：compaction 把同一字节\n";
    std::cout << "      反刍重写了好几遍，代价从「随机 IO」换成了「多写几遍+后台抖动」。\n\n";

    std::cout << "  B+ 树（每次写入 = 下降到目标页 + 就地改 + 落盘）\n";
    auto& bs = btSeq.stats();
    auto& br = btRnd.stats();
    std::cout << "    顺序键: " << msBtSeq << " ms | 页读 " << bs.reads << "（热点页命中 "
              << bs.cacheHits << " 次免下降）| 页写 " << bs.writes << " | 分裂 " << bs.splits
              << " 次 | 树高 " << btSeq.depth() << " 层\n";
    std::cout << "    随机键: " << msBtRnd << " ms | 页读 " << br.reads << "（全是随机 IO）| 页写 "
              << br.writes << " | 分裂 " << br.splits << " 次 | 树高 " << btRnd.depth() << " 层\n";
    std::cout << "    ↑ 真实磁盘上一页 16KB：随机键的 10 万次插入 ≈ "
              << br.reads << " 次随机读 + " << br.writes
              << " 次随机回写。机械盘顺序写能飙到几百 MB/s，随机 IO 却只有\n";
                 std::cout << "      每秒一两百次（一次 ~5-10ms），\n";
    std::cout << "      这笔账在内存里看不出来，上了盘就是天壤之别——\n";
    std::cout << "      这也是 InnoDB 苦劝你别用 UUID 做主键的原因（随机主键=页碎+缓存炸）。\n\n";

    // -----------------------------------------------------------------
    // 演示三：读放大对账——写完之后点查 2 万次（键均匀随机）
    // -----------------------------------------------------------------
    std::cout << "[演示三] 写完之后点查 20000 次（键均匀随机，都查已存在的键）\n";
    std::vector<int> qkeys(N);
    for (int i = 0; i < N; i++) qkeys[i] = i;
    std::shuffle(qkeys.begin(), qkeys.end(), g);
    const int Q = 20000;
    std::string v;

    long long lsmProbeBefore = lsmRnd.segmentProbes();
    long long lsmPassBefore = lsmRnd.bloomPassedProbes();
    long long lsmReadBefore = lsmRnd.dataPageReads();
    auto t0 = clk::now();
    int hitsLsm = 0;
    for (int i = 0; i < Q; i++) if (lsmRnd.get(makeKey(qkeys[i]), &v)) hitsLsm++;
    double msQLsm = std::chrono::duration<double, std::milli>(clk::now() - t0).count();

    long long btReadBefore = btRnd.stats().reads;
    t0 = clk::now();
    int hitsBt = 0;
    for (int i = 0; i < Q; i++) if (btRnd.get(makeKey(qkeys[i]), &v)) hitsBt++;
    double msQBt = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    long long btPageReads = btRnd.stats().reads - btReadBefore;

    std::cout << "  LSM  : 命中 " << hitsLsm << "/" << Q << " | " << msQLsm
              << " ms | 平均探测 " << (double)(lsmRnd.segmentProbes() - lsmProbeBefore) / Q
              << " 个段 | 布隆放行后平均真翻 " << (double)(lsmRnd.bloomPassedProbes() - lsmPassBefore) / Q
              << " 个文件 | 平均读 " << (double)(lsmRnd.dataPageReads() - lsmReadBefore) / Q << " 页/次\n";
    std::cout << "  B+ 树: 命中 " << hitsBt << "/" << Q << " | " << msQBt
              << " ms | 平均读 " << (double)btPageReads / Q << " 页/次（=树高，根页常驻缓存还能再少）\n";
    std::cout << "  ↑ 先说公道话：compaction 跟得上、查的键又存在时，LSM 靠键范围+布隆也没慢多少。\n";
    std::cout << "    读放大的真账在下面两笔：查【不存在】的键，和 compaction 跟不上写的时侯。\n\n";

    // ---- B) 点查 2 万个【不存在】的键（读放大最疼的路径）----
    std::cout << "  —— 同一批键加 '#' 后缀（库里没有这种键），再查 " << Q << " 次 ——\n";
    auto ghostKey = [](int i) { return makeKey(i) + "#"; };
    lsmRnd.setUseBloom(false);
    long long fB = lsmRnd.bloomPassedProbes();
    t0 = clk::now();
    for (int i = 0; i < Q; i++) lsmRnd.get(ghostKey(qkeys[i]), &v);
    double msG1 = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    long long passGhost1 = lsmRnd.bloomPassedProbes() - fB;
    lsmRnd.setUseBloom(true);
    long long fB2 = lsmRnd.bloomPassedProbes();
    t0 = clk::now();
    for (int i = 0; i < Q; i++) lsmRnd.get(ghostKey(qkeys[i]), &v);
    double msG2 = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    long long passGhost2 = lsmRnd.bloomPassedProbes() - fB2;
    long long btReadB2 = btRnd.stats().reads;
    t0 = clk::now();
    for (int i = 0; i < Q; i++) btRnd.get(ghostKey(qkeys[i]), &v);
    double msG3 = std::chrono::duration<double, std::milli>(clk::now() - t0).count();

    std::cout << "  LSM 无布隆: " << msG1 << " ms | 平均真翻 "
              << (double)passGhost1 / Q << " 个文件/次（每个都要打开验明正身）\n";
    std::cout << "  LSM 有布隆: " << msG2 << " ms | 平均真翻 "
              << (double)passGhost2 / Q << " 个文件/次  << 几乎全被拦在门外\n";
    std::cout << "  B+ 树     : " << msG3 << " ms | 平均读 "
              << (double)(btRnd.stats().reads - btReadB2) / Q
              << " 页/次（查不存在也是树高直达，代价恒定——这是 B+ 树的稳）\n";

    // ---- C) compaction 跟不上写：L0 堆了几个键重叠的段 ----
    {
        LSMTree burst;
        burst.init("lsm_tmp_data/burst", 4096, 4); // L0 攒满 4 段才触发 compaction
        for (int i = 0; i < 12288; i++) burst.put(makeKey(i), kValue); // 恰好 3 次 flush，都堆在 L0
        std::cout << "\n  —— compaction 跟不上写的时侯（L0 堆了 " << burst.levelSegCount(0)
                  << " 个键范围互相重叠的段）——\n";
        long long p0 = burst.segmentProbes();
        for (int i = 0; i < 2000; i++) burst.get(makeKey(i), &v); // 查最早落盘那批
        std::cout << "  查最早落盘的键: 平均探测 "
                  << (double)(burst.segmentProbes() - p0) / 2000
                  << " 个段（compaction 干完活后只要 ~1.8 个）\n";
        std::cout << "  ↑ 这就是 RocksDB 的软肋现场：L0 段数一高读立刻变慢，\n";
        std::cout << "    所以它有 write stall——L0 文件超过阈值直接暂停你的写入，\n";
        std::cout << "    宁可让你卡一下，也不能让读放大失控。\n";
        burst.shutdown();
    }

    lsmSeq.shutdown();
    lsmRnd.shutdown();

    // -----------------------------------------------------------------
    // 老师傅收尾
    // -----------------------------------------------------------------
    std::cout << "\n================================================================\n"
                 "  老师傅的话\n"
                 "================================================================\n"
                 " 1. 两种结构本质是「把代价记在哪个科目」的选择：\n"
                 "      B+ 树：写要就地更新（随机 IO + 页碎片 + 分裂），读直达；\n"
                 "      LSM  ：写延迟结算（先攒后刷，全顺序 IO），代价摊到\n"
                 "             写放大（数据被反刍重写 2~10 遍）和读放大（多层找）。\n"
                 " 2. 为什么写多的系统（RocksDB/Cassandra/HBase）全投了 LSM？\n"
                 "      盘的特性：顺序写吞吐是随机写的百倍级；写还能被压缩摊薄。\n"
                 "      读放大有现代补丁：布隆过滤器（本文件演示了它的省钱效果）、\n"
                 "      分层索引、页缓存、后台限速——读慢可以救，写碎没好药。\n"
                 " 3. InnoDB 为什么守着 B+ 树不走？\n"
                 "      它卖的不是写吞吐，是「任意查询都要稳」：二级索引、范围扫描、\n"
                 "      点查延迟稳定（树高 3~4 层封顶）。而且 WAL 已经把 redo 的\n"
                 "      顺序写账记过了——「先日志后页」和 LSM 的 memtable 异曲同工，\n"
                 "      下一个文件（03）讲分布式并发之前，你可以回头看 storage_index 里\n"
                 "      WAL 那一课，会发现两家其实互相抄了作业。\n"
                 " 4. compaction 不是免费的午餐：它抢 CPU、抢带宽、造成延迟毛刺，\n"
                 "      还可能在删旧段时留下 tombstone 拖慢读——真实引擎一半的\n"
                 "      调优参数（level/size-tiered、限速、并行度）都在伺候这个理货过程。\n";

    std::filesystem::remove_all("lsm_tmp_data"); // 教学程序：清场
    return 0;
}
