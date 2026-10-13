// ================================================================
//  02_btree_vs_sorted_array.cpp —— 量化对比：B+ 树到底赢在哪
//  本模块核心代码：100 万数据，四种查找，比较次数与 I/O 次数摆桌上
// ================================================================
//
//  徒弟，上一课你知道了“磁盘一次只能按页搬 4KB”。
//  今天的问题就是：100 万个数，我要找一个，怎么找最省？
//
//  你作为熟练 C++ 开发者，脑子里大概有三件武器：
//    (a) std::vector 从头扫到尾             —— 线性扫描
//    (b) std::lower_bound 在有序数组里二分   —— 二分查找
//    (c) std::map / std::set                 —— 红黑树
//  数据库选的是第三种的东西：B+ 树。为什么？
//  今天不比抽象复杂度（三个都是 O(log n)，比了等于没比），
//  咱们比两笔真实的账：
//      比较次数    → CPU 的账
//      页读取次数  → 磁盘的账（一次随机页读约 100μs，
//                     一次内存比较约 1ns，差十万倍）
//  这笔账算完，你就懂“为什么数据库不用红黑树”了。
//
//  编译： g++ -std=c++17 -O2 02_btree_vs_sorted_array.cpp -o 02_btree_vs_sorted_array.exe
//

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;
using std::chrono::high_resolution_clock;
using std::chrono::duration;

static void setup_console_utf8() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
}

// 一页多大、一个节点最多几个 key —— 这两行是全篇的题眼
static constexpr uint32_t PAGE_SIZE    = 4096;   // 和真实数据库一个量级（SQLite 默认 4KB）
static constexpr int      KEY_PER_NODE = 100;    // 一个节点（=一页）最多 100 个 key
// 真实算一下：4096 字节 / (8 字节 key + 8 字节页号) ≈ 250 个，
// 这里取 100 是为了让分裂过程少一点、看得清。你自己改成 4 试试。

// 页内查找要比较几次：一页里对 100 个 key 做二分，约 log2(100)+1 = 7 次
static uint64_t intra_page_cmp(size_t n) {
    uint64_t c = 0;
    for (size_t len = n; len > 0; len /= 2) ++c;
    return c + 1;                                  // 最后一次判等
}

// ================================================================
//  方案 A：无序数组，线性扫描
// ================================================================
//  就是你手写 CRUD 时最朴素的写法，数据库里叫全表扫描（full table scan）。
//  它不丢人 —— 要取 30% 以上的行时，全表扫往往比走索引还快。
//
static uint64_t linear_scan(const vector<int>& arr, const vector<int>& queries, uint64_t& found) {
    uint64_t cmp = 0;
    found        = 0;
    for (int q : queries) {
        for (int v : arr) {                        // 每个元素都得读进来比一下
            ++cmp;
            if (v == q) { ++found; break; }
        }
    }
    return cmp;
}

// ================================================================
//  方案 B：有序数组 + 二分
// ================================================================
//  比较次数很漂亮（log2 1e6 约 20 次）。
//  但注意：本表里它“不计 I/O”，因为整份数组摊在内存里。
//  真实场景这份数据放不进内存，那 20 次比较的落点分别在
//  数组的头、中、四分之三……每次跨到另一个磁盘页 = 20 次随机 I/O。
//  这就是“比较次数”和“I/O 次数”必须分两栏记的原因。
//
static uint64_t binary_search_count(const vector<int>& arr, const vector<int>& queries,
                                    uint64_t& found) {
    uint64_t cmp = 0;
    found        = 0;
    for (int q : queries) {
        size_t lo = 0, hi = arr.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;       // 顺便记住：这么写才不溢出
            ++cmp;
            if (arr[mid] < q)      lo = mid + 1;
            else if (arr[mid] > q) hi = mid;
            else { ++found; break; }
        }
    }
    return cmp;
}

// ================================================================
//  方案 C：手写简化 B+ 树（阶 = KEY_PER_NODE+1，叶子有链表，只插不删）
// ================================================================
//
//  先把结构说清楚，你对着 C++ 的容器想：
//
//    std::map ：一个节点 1 个 key、2 个孩子。靠旋转保持平衡，
//               节点在堆上东一块西一块，指针跳来跳去。
//    B+ 树    ：一个节点（=一页）装 100 个 key、101 个孩子。
//               树又矮又宽，像“分叉 101 倍的 std::map”。
//
//  三个关键设计，每一个都来自上一课那条矛盾：
//    1) 节点大小 = 一页。一次页读取 = 换回一批 key 和一批分叉。
//    2) 数据全在叶子，非叶子只存 key 当路标、不存数据。
//       于是内部节点扇出极大，树高被压到 3~4 层。
//    3) 叶子之间用链表串起来。范围扫描不用回到根重走，
//       顺链表一页一页往后读 —— 随机 I/O 变顺序 I/O。
//       这一条是 B+ 树完胜红黑树的决定性原因。
//
struct BPlusNode {
    bool        is_leaf  = true;
    vector<int> keys;                              // 叶子：数据本身；内部节点：分叉路标
    vector<int> children;                          // 内部节点：子节点编号，size == keys.size()+1
    int         id       = -1;
    int         parent   = -1;
    int         next     = -1;                     // 叶子链表：下一个叶子页
};

class BPlusTree {
public:
    struct FindStat {
        uint64_t cmp     = 0;
        uint64_t page_io = 0;
        uint64_t depth   = 0;
        bool     hit     = false;
    };

    // ---------------- 点查：返回比较次数 + 页读取次数 ----------------
    FindStat find(int key) const {
        FindStat st;
        if (root_ < 0) return st;

        int cur = root_;
        st.page_io = 1;                            // 读根页
        while (true) {
            const BPlusNode& n = nodes_[static_cast<size_t>(cur)];
            st.depth++;
            st.cmp += intra_page_cmp(n.keys.size());
            if (n.is_leaf) {
                auto it  = lower_bound(n.keys.begin(), n.keys.end(), key);
                st.hit   = (it != n.keys.end() && *it == key);
                return st;
            }
            size_t idx = upper_bound(n.keys.begin(), n.keys.end(), key) - n.keys.begin();
            cur = n.children[idx];
            st.page_io++;                          // 下降一层 = 一次页读取
        }
    }

    // ---------------- 范围扫描：定位一次叶子，然后沿叶子链表走 ----------------
    struct RangeStat {
        uint64_t cmp     = 0;
        uint64_t page_io = 0;
        uint64_t rows    = 0;
    };
    RangeStat range_scan(int lo, int hi) const {
        RangeStat rs;
        if (root_ < 0) return rs;
        uint64_t walk = 0;
        int cur = locate_leaf(lo, walk);            // walk 已经含了定位路上所有页（含这片叶子）
        rs.page_io += walk;
        bool counted = true;                        // 这片叶子已经在 walk 里算过了
        while (cur >= 0) {
            const BPlusNode& n = nodes_[static_cast<size_t>(cur)];
            if (!counted) rs.page_io++;             // 顺着链表新走到的页才算新的 I/O
            counted = false;
            for (int k : n.keys) {
                rs.cmp++;
                if (k > hi) return rs;
                if (k >= lo) rs.rows++;
            }
            cur = n.next;                           // 关键：不回根，顺着链表走
        }
        return rs;
    }

    // ---------------- 插入（含分裂、上提、必要时增高） ----------------
    void insert(int key) {
        if (root_ < 0) {
            root_ = new_node(true);
            nodes_[static_cast<size_t>(root_)].keys.push_back(key);
            return;
        }
        uint64_t walk = 0;
        size_t   leaf = static_cast<size_t>(locate_leaf(key, walk));

        auto&      ln = nodes_[leaf];
        auto       p  = lower_bound(ln.keys.begin(), ln.keys.end(), key);
        size_t     at = static_cast<size_t>(p - ln.keys.begin());
        ln.keys.insert(ln.keys.begin() + static_cast<ptrdiff_t>(at), key);

        if (static_cast<int>(ln.keys.size()) > KEY_PER_NODE) split_leaf(static_cast<int>(leaf));
    }

    int height() const { return height_ + 1; }    // 层数：只有根叶子时算 1 层
    int node_count() const { return static_cast<int>(nodes_.size()); }
    int leaf_count() const {
        int c = 0;
        for (const auto& n : nodes_) if (n.is_leaf) ++c;
        return c;
    }
    int internal_fill_pct() const {                // 平均填充率，用来解释“理论 3 层实际 4 层”
        size_t tot = 0, cnt = 0;
        for (const auto& n : nodes_) {
            if (n.is_leaf) continue;
            tot += n.keys.size();
            ++cnt;
        }
        return cnt ? static_cast<int>(100.0 * static_cast<double>(tot) / static_cast<double>(cnt) /
                                   static_cast<double>(KEY_PER_NODE))
                   : 0;
    }

    int leaf_chain_length() const {                // 验证叶子链表没断
        int cur = leftmost_leaf(), c = 0;
        while (cur >= 0) { ++c; cur = nodes_[static_cast<size_t>(cur)].next; }
        return c;
    }
    bool verify_sorted() const {                   // 验证链表走出来的 key 全局有序
        int  cur  = leftmost_leaf();
        bool first = true;
        int  prev  = numeric_limits<int>::min();
        while (cur >= 0) {
            for (int k : nodes_[static_cast<size_t>(cur)].keys) {
                if (!first && k <= prev) return false;
                prev  = k;
                first = false;
            }
            cur = nodes_[static_cast<size_t>(cur)].next;
        }
        return true;
    }

private:
    int new_node(bool is_leaf) {
        BPlusNode n;
        n.id      = static_cast<int>(nodes_.size());
        n.is_leaf = is_leaf;
        nodes_.push_back(std::move(n));
        return nodes_.back().id;
    }

    int leftmost_leaf() const {
        if (root_ < 0) return -1;
        size_t cur = static_cast<size_t>(root_);
        while (!nodes_[cur].is_leaf) cur = static_cast<size_t>(nodes_[cur].children[0]);
        return static_cast<int>(cur);
    }

    // 从根下降到叶子，walk 里返回走过的页数
    int locate_leaf(int key, uint64_t& walk) const {
        size_t cur = static_cast<size_t>(root_);
        walk       = 1;
        while (!nodes_[cur].is_leaf) {
            const auto& ks = nodes_[cur].keys;
            size_t     idx = upper_bound(ks.begin(), ks.end(), key) - ks.begin();
            cur            = static_cast<size_t>(nodes_[cur].children[idx]);
            walk++;
        }
        return static_cast<int>(cur);
    }

    // 注意：new_node() 里 nodes_ 可能扩容，之前拿到的引用/BPlusNode& 会全部失效。
    // 所以下面这几个函数一律"先取值、再建新节点、之后重新用下标取"，别留引用过夜。

    // ---------------- 叶子分裂：一分为二，右半最小 key 复制上提 ----------------
    void split_leaf(int leaf) {
        size_t mid = nodes_[static_cast<size_t>(leaf)].keys.size() / 2;

        vector<int> right_keys(nodes_[static_cast<size_t>(leaf)].keys.begin() +
                                   static_cast<ptrdiff_t>(mid),
                               nodes_[static_cast<size_t>(leaf)].keys.end());
        int prev_next = nodes_[static_cast<size_t>(leaf)].next;
        nodes_[static_cast<size_t>(leaf)].keys.resize(mid);

        int sib = new_node(true);                  // 新叶子页（= 新的一页）
        nodes_[static_cast<size_t>(sib)].keys  = std::move(right_keys);
        nodes_[static_cast<size_t>(sib)].next  = prev_next;   // 链表接上：leaf -> sib -> 原来的 next
        nodes_[static_cast<size_t>(leaf)].next = sib;

        int promote = nodes_[static_cast<size_t>(sib)].keys[0];
        insert_into_parent(leaf, promote, sib);    // 叶子分裂：上提的是副本（数据仍在叶子里）
    }

    // ---------------- 内部节点分裂：中位数上提，自己不留 ----------------
    void split_internal(int node) {
        size_t mid     = nodes_[static_cast<size_t>(node)].keys.size() / 2;
        int    promote = nodes_[static_cast<size_t>(node)].keys[mid];

        vector<int> right_keys(nodes_[static_cast<size_t>(node)].keys.begin() +
                                   static_cast<ptrdiff_t>(mid + 1),
                               nodes_[static_cast<size_t>(node)].keys.end());
        vector<int> right_children(nodes_[static_cast<size_t>(node)].children.begin() +
                                       static_cast<ptrdiff_t>(mid + 1),
                                   nodes_[static_cast<size_t>(node)].children.end());
        nodes_[static_cast<size_t>(node)].keys.resize(mid);
        nodes_[static_cast<size_t>(node)].children.resize(mid + 1);

        int sib = new_node(false);
        nodes_[static_cast<size_t>(sib)].keys     = std::move(right_keys);
        nodes_[static_cast<size_t>(sib)].children = std::move(right_children);
        for (int c : nodes_[static_cast<size_t>(sib)].children)
            nodes_[static_cast<size_t>(c)].parent = sib;

        insert_into_parent(node, promote, sib);
    }

    // ---------------- 把 (mid_key, right) 塞进父节点；父节点没有就说明该长高了 ----------------
    void insert_into_parent(int left, int mid_key, int right) {
        int parent = nodes_[static_cast<size_t>(left)].parent;
        if (parent < 0) {                          // 根被分裂 -> 树长高一层
            int nr = new_node(false);
            nodes_[static_cast<size_t>(nr)].keys     = {mid_key};
            nodes_[static_cast<size_t>(nr)].children = {left, right};
            nodes_[static_cast<size_t>(left)].parent  = nr;
            nodes_[static_cast<size_t>(right)].parent = nr;
            root_   = nr;
            height_ += 1;
            return;
        }
        size_t at = static_cast<size_t>(
            upper_bound(nodes_[static_cast<size_t>(parent)].keys.begin(),
                        nodes_[static_cast<size_t>(parent)].keys.end(), mid_key) -
            nodes_[static_cast<size_t>(parent)].keys.begin());
        nodes_[static_cast<size_t>(parent)].keys.insert(
            nodes_[static_cast<size_t>(parent)].keys.begin() + static_cast<ptrdiff_t>(at), mid_key);
        nodes_[static_cast<size_t>(parent)].children.insert(
            nodes_[static_cast<size_t>(parent)].children.begin() + static_cast<ptrdiff_t>(at + 1),
            right);
        nodes_[static_cast<size_t>(right)].parent = parent;

        if (static_cast<int>(nodes_[static_cast<size_t>(parent)].keys.size()) > KEY_PER_NODE)
            split_internal(parent);
    }

    vector<BPlusNode> nodes_;
    int               root_   = -1;
    int               height_ = 0;                 // 分裂根几次 = 多长高几层
};

// ================================================================
//  方案 D：红黑树（用有序数组模拟 std::map 的下降路径）
// ================================================================
//  std::map 每个节点是独立的堆分配块，内存里散着放。
//  所以每下降一层，就是一次谁也猜不到的跳转：一次缓存行缺失；
//  数据上了磁盘，就是整整一次随机页读。这一层记 1 次 I/O。
//
static uint64_t rbtree_find(const vector<int>& arr, const vector<int>& queries,
                            uint64_t& page_io, uint64_t& found) {
    uint64_t cmp = 0;
    page_io      = 0;
    found        = 0;
    const int n  = static_cast<int>(arr.size());
    for (int q : queries) {
        int lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            ++cmp;
            ++page_io;                             // 每下降一层：节点在随机位置，换一次页
            if (arr[mid] < q)      lo = mid + 1;
            else if (arr[mid] > q) hi = mid - 1;
            else { ++found; break; }
        }
    }
    return cmp;
}

// ================================================================
//  打表
// ================================================================
struct Row {
    string   name;
    uint64_t cmp     = 0;
    uint64_t page_io = 0;
    uint64_t found   = 0;
    double   ms      = 0.0;
};

static void print_table(const vector<Row>& rows, int qn) {
    cout << "\n+------------------------------+------------+------------+-----------+------------+\n";
    cout << "| 方案                         | 比较/次查询 | 页读/次查询 | 命中总数  | 总耗时(ms) |\n";
    cout << "+------------------------------+------------+------------+-----------+------------+\n";
    for (const auto& r : rows) {
        cout << "| " << left << setw(26) << r.name << " | " << right << setw(10)
             << (r.cmp / static_cast<uint64_t>(qn)) << " | " << setw(10)
             << (r.page_io / static_cast<uint64_t>(qn)) << " | " << setw(9) << r.found << " | "
             << setw(10) << fixed << setprecision(2) << r.ms << " |\n";
    }
    cout << "+------------------------------+------------+------------+-----------+------------+\n";
}

static void banner(const string& s) {
    cout << "\n--------------------------------------------------------------\n  " << s
         << "\n--------------------------------------------------------------\n";
}

int main() {
    setup_console_utf8();

    const int N    = 1'000'000;                    // 100 万行
    const int Q    = 1000;                         // 1000 次查询取平均
    const int SEED = 20261012;

    cout << "================================================================\n"
         << "  线性扫描 / 有序数组二分 / 手写 B+ 树 / 红黑树：四笔账摆一起\n"
         << "================================================================\n";
    cout << "\n  数据规模  : " << N << " 个 int（当作一张 100 万行的表）\n"
         << "  查询次数  : " << Q << " 次（一半存在、一半不存在）\n"
         << "  B+ 树参数 : 页 " << PAGE_SIZE << " 字节，单节点最多 " << KEY_PER_NODE
         << " 个 key，阶（扇出）= " << (KEY_PER_NODE + 1) << "\n";

    // ---------------- 造数据 ----------------
    mt19937_64 rng(SEED);
    vector<int> keys(static_cast<size_t>(N));
    iota(keys.begin(), keys.end(), 0);             // 0..999999，唯一
    shuffle(keys.begin(), keys.end(), rng);        // 打乱，模拟插入顺序无序

    vector<int> sorted = keys;
    sort(sorted.begin(), sorted.end());

    vector<int> queries;
    queries.reserve(static_cast<size_t>(Q));
    uniform_int_distribution<size_t> pick(0, static_cast<size_t>(N) - 1);
    for (int i = 0; i < Q / 2; ++i) queries.push_back(sorted[pick(rng)]);
    uniform_int_distribution<int> miss(1, 1'000'000);
    for (int i = 0; i < Q / 2; ++i) queries.push_back(-miss(rng));   // 负数一定不存在

    // ---------------- 建 B+ 树 ----------------
    banner("第 1 步：往手写 B+ 树里插 100 万个 key");
    BPlusTree tree;
    auto t0 = high_resolution_clock::now();
    for (int k : keys) tree.insert(k);
    auto t1 = high_resolution_clock::now();
    double build_ms = duration<double, milli>(t1 - t0).count();

    cout << "  建树耗时  : " << fixed << setprecision(2) << build_ms << " ms\n"
         << "  节点总数  : " << tree.node_count() << "（其中叶子 " << tree.leaf_count() << " 个）\n"
         << "  树高      : " << tree.height() << " 层（根算第 1 层）\n"
         << "  内部节点平均填充率 : " << tree.internal_fill_pct() << "%\n"
         << "  叶子链表  : 长度 " << tree.leaf_chain_length() << "，全局有序校验 "
         << (tree.verify_sorted() ? "通过" : "失败！") << "\n";

    cout << "\n  手工验算树高（扇出 = 阶 = " << (KEY_PER_NODE + 1) << "，每叶最多 " << KEY_PER_NODE
         << " 个 key）：\n";
    {
        long long cap = KEY_PER_NODE;              // 1 层（根就是叶子）最多装这么多 key
        for (int h = 1; h <= 5; ++h) {
            string mark;
            if (cap >= N) mark = "   <- 满打满算 " + to_string(h) + " 层就够";
            cout << "    " << h << " 层容量 = " << cap << " 个 key" << mark << "\n";
            if (cap >= N) break;
            cap *= static_cast<long long>(KEY_PER_NODE) + 1;   // 每多一层，容量乘以扇出
        }
    }
    cout << "  实际跑出来是 " << tree.height()
         << " 层：满打满算是理论上限，分裂会把节点平均只填到六成左右，容量得打六折，\n"
         << "  于是实际层数常常比理论上限多一层。\n"
         << "  结论：千万级、亿级数据，B+ 树树高也就 4、5 层。\n";

    // ---------------- 四种方案跑起来 ----------------
    banner("第 2 步：四种查找方案，各跑 " + to_string(Q) + " 次");
    vector<Row> rows(4);

    // A 线性扫描
    {
        uint64_t found = 0;
        auto     a     = high_resolution_clock::now();
        uint64_t cmp   = linear_scan(keys, queries, found);
        auto     b     = high_resolution_clock::now();
        rows[0]        = {"A 线性扫描(无序数组)", cmp,
                          // 数据若在盘上，找一条得把整表页读一遍：4MB / 4KB = 976 页
                          static_cast<uint64_t>(Q) * (static_cast<uint64_t>(N) * sizeof(int) / PAGE_SIZE),
                          found, duration<double, milli>(b - a).count()};
    }
    // B 有序数组二分（数据常驻内存，不计 I/O）
    {
        uint64_t found = 0;
        auto     a     = high_resolution_clock::now();
        uint64_t cmp   = binary_search_count(sorted, queries, found);
        auto     b     = high_resolution_clock::now();
        rows[1]        = {"B 有序数组二分(在内存)", cmp, 0, found,
                          duration<double, milli>(b - a).count()};
    }
    // C 手写 B+ 树
    {
        uint64_t cmp = 0, io = 0, found = 0;
        auto     a   = high_resolution_clock::now();
        for (int q : queries) {
            auto st = tree.find(q);
            cmp += st.cmp;
            io += st.page_io;
            found += st.hit ? 1 : 0;
        }
        auto b = high_resolution_clock::now();
        rows[2] = {"C 手写 B+ 树", cmp, io, found, duration<double, milli>(b - a).count()};
    }
    // D 红黑树
    {
        uint64_t io = 0, found = 0;
        auto     a  = high_resolution_clock::now();
        uint64_t cmp = rbtree_find(sorted, queries, io, found);
        auto     b  = high_resolution_clock::now();
        rows[3]     = {"D 红黑树(节点散在堆上)", cmp, io, found,
                       duration<double, milli>(b - a).count()};
    }
    print_table(rows, Q);

    cout << "  注：B 那行页读 0 是它“数据全在内存”换来的，前提是内存装得下。\n"
         << "      一旦这份数据在磁盘上，B 的页读次数就是它的比较次数（20），\n"
         << "      因为二分的每一刀都落在一个新页上。数据库里这才是常态。\n";

    // ---------------- 解读 ----------------
    banner("第 3 步：这两笔账怎么读");
    cout <<
        "  【比较次数：CPU 的账】\n"
        "  A 每次要比几十万到一百万次，因为它根本不知道数据在哪。\n"
        "  B/D 都是 20 上下；C 反而略多一点 —— 页内要二分，4 层 x 7 次。\n"
        "  所以只看比较次数会得出错误结论：二分和红黑树够用了嘛，要 B+ 树干嘛？\n\n"
        "  【页读取次数：磁盘的账 —— 这才是分水岭】\n"
        "  A：整表读一遍，100 万 int = 4MB，976 页，每次查询都读一遍，自杀。\n"
        "  B：内存里 0 次；上磁盘就是 20 次随机 I/O。\n"
        "  C：等于树高，4 次。这就是 B+ 树的全部意义。\n"
        "  D：每下降一层一次不可预知的跳转，内存里是缓存行缺失，\n"
        "     上磁盘是 20 次随机 I/O，和二分一个命。\n\n"
        "  【为什么数据库选 B+ 树，四条理由，一条比一条硬】\n"
        "  1) 树矮，所以 I/O 少。B+ 树一个节点就是一页，一次 I/O 换 101 个分叉；\n"
        "     红黑树一次 I/O 只换 2 个分叉。关键量是扇出（fan-out），\n"
        "     log 的底数不一样：log2(1e6)=20，log101(1e6)约 3。\n"
        "  2) 数据全在叶子，非叶子只存路标，同样 4KB 能装的路标最多，扇出最大。\n"
        "     红黑树每个节点都得挂数据，扇出被数据尺寸拖死。\n"
        "  3) 叶子之间有链表，范围查询（BETWEEN / ORDER BY / 前缀匹配）顺着链表走，\n"
        "     随机 I/O 变顺序 I/O，顺序读带宽是随机读的几十倍。\n"
        "     红黑树做范围扫要中序递归，节点物理顺序与逻辑顺序无关，到处跳。\n"
        "  4) 工程上更合适：B+ 树节点是定长页，直接对应 Buffer Pool 的一帧，\n"
        "     可以整页预读；它只分裂不旋转，一次改动只碰两三页。\n"
        "     红黑树靠旋转保平衡，一次旋转改好几个节点指针，\n"
        "     数据库里意味着好几个页要加 latch、好几页要写 WAL，并发代价大得多。\n\n"
        "  【一句话】B+ 树不是更快的查找结构，它是“在按页读写的磁盘上”最快的查找结构。\n"
        "  离开了这个前提它没什么稀奇：纯内存场景，你该用 std::map、跳表或者 hash map。\n";

    // ---------------- 范围扫描对比 ----------------
    banner("第 4 步：范围查询 —— B+ 树真正拉开差距的地方");
    {
        const int lo = 500'000, hi = 510'000;      // 取 10 万零 1 行
        auto rs = tree.range_scan(lo, hi);
        cout << "  B+ 树范围扫 [" << lo << ", " << hi << "]：命中 " << rs.rows << " 行，页读 "
             << rs.page_io << " 次，比较 " << rs.cmp << " 次\n";

        // 没有叶子链表的结构（比如 hash 索引），只能逐个点查
        uint64_t point_cmp = 0, point_io = 0, point_found = 0;
        for (int k = lo; k <= hi; ++k) {
            vector<int> one = {k};
            uint64_t    one_found = 0;
            point_cmp += binary_search_count(sorted, one, one_found);
            point_found += one_found;
            point_io += static_cast<uint64_t>(tree.height());   // 每次点查都要从根走到底
        }
        cout << "  逐个点查同样 " << (hi - lo + 1) << " 行：页读 " << point_io << " 次，比较 "
             << point_cmp << " 次，命中 " << point_found << "\n";
        cout << "  页读取差距 = " << (point_io / max<uint64_t>(rs.page_io, 1)) << " 倍。\n"
             << "  这就是 ORDER BY id LIMIT 10000 能顺链表飞、而 hash 索引只能逐个点查的原因\n"
             << "  （hash 索引天生不支持范围，这也是 InnoDB 主键必须是 B+ 树的原因）。\n";
    }

    // ---------------- 顺序 vs 随机 ----------------
    banner("第 5 步：现场测一次“顺序访问 vs 随机跳转”，感受扇出为什么值钱");
    {
        const int M = 4'000'000;
        vector<int> buf(static_cast<size_t>(M));
        iota(buf.begin(), buf.end(), 0);
        vector<int> order(static_cast<size_t>(M));
        iota(order.begin(), order.end(), 0);
        shuffle(order.begin(), order.end(), rng);

        volatile long long sink = 0;
        auto a = high_resolution_clock::now();
        for (int i = 0; i < M; ++i) sink += buf[static_cast<size_t>(i)];   // 顺序：硬件预取最爱
        auto b = high_resolution_clock::now();
        double seq_ms = duration<double, milli>(b - a).count();

        a = high_resolution_clock::now();
        for (int i = 0; i < M; ++i) sink += buf[static_cast<size_t>(order[static_cast<size_t>(i)])];
        b = high_resolution_clock::now();
        double rnd_ms = duration<double, milli>(b - a).count();

        cout << "  顺序访问 400 万个 int : " << fixed << setprecision(2) << seq_ms << " ms\n"
             << "  随机访问 400 万个 int : " << rnd_ms << " ms（差 "
             << fixed << setprecision(1) << (rnd_ms / max(seq_ms, 0.01)) << " 倍）\n"
             << "  这就是一页里把 100 个 key 紧挨着排值钱的原因：\n"
             << "  4KB = 64 条缓存行，B+ 树在页内怎么跳都还在同一条页里，CPU 预取一路顺风；\n"
             << "  红黑树跳一次节点，就可能把刚载入的整条缓存行废掉。\n";
    }

    // ================================================================
    cout << "\n================================================================\n"
         << "  老师傅的话\n"
         << "================================================================\n"
         << "  徒弟，这份代码是本模块的核心，你别只看结论，要盯着三件事。\n\n"
         << "  1) 一定要把“比较次数”和“I/O 次数”分成两栏记。\n"
         << "     你写 C++ 养成的习惯是看大 O、看比较次数，那套在内存里对。\n"
         << "     数据一旦在磁盘上：一次比较 1ns，一次随机页读 100us，十万倍。\n"
         << "     谁能少读一页谁就赢。数据库工程师谈性能，开口是“这条查询读几页”，\n"
         << "     不说“复杂度多少”。\n\n"
         << "  2) 扇出是 B+ 树的命根子。\n"
         << "     一页 4KB 装 100 个 key，扇出 101；红黑树扇出 2。\n"
         << "     100 万行上是 3 层和 20 层的差距，10 亿行上是 4 层和 30 层的差距。\n"
         << "     所以数据库拼命想让一页塞更多 key：定长 key、非叶子只存路标、\n"
         << "     前缀压缩、页开大点 —— 全围着扇出转。\n\n"
         << "  3) 我这个 B+ 树是残缺的，故意的：没删除、没并发、没落盘。\n"
         << "     你补作业的时候就懂了：\n"
         << "     - 删除会让叶子变空，要合并，一合并就可能连锁上提，树会缩高；\n"
         << "     - 并发要 latch coupling（握手锁）：下降时锁父放子；分裂要同时锁两页，\n"
         << "       这就是为什么“分裂”在真实引擎里是个危险动作；\n"
         << "     - 落盘就是把每个 node 换成上一课的 Page，children 存页号而不是下标，\n"
         << "       上一课的 Pager 直接接上就行，你自己试试。\n\n"
         << "  【课后动手】\n"
         << "   1. 把 KEY_PER_NODE 依次改成 4、16、64、256，记下树高和每次查询的页读次数。\n"
         << "      你会看到一条明显的边际收益递减曲线 —— 这就是“页开多大”的答案：\n"
         << "      大到能摊薄 I/O，小到不超过缓存层次和内存浪费的上限。\n"
         << "   2. 给 tree.find() 装上上一课那个 Buffer Pool，访问序列设成热点集中在\n"
         << "      前 1% 的 key，看页读次数从“树高”变成什么。真实数据库里，树高只是\n"
         << "      I/O 的下界，命中率才是决定项。\n"
         << "   3. 把查询全换成不存在的 key，比较 B 和 C 的差距。想一想：\n"
         << "      “不存在”这个结论，B+ 树为什么和“存在”花一样多的页？\n"
         << "   4. 思考题：key 换成 UUID（随机分布、无局部性），B+ 树会怎样？\n"
         << "      为什么很多人劝你别拿 UUID 当主键？（提示：插入位置随机，\n"
         << "      页分裂满天飞、缓存命中率崩、顺序写退化成随机写。第 3 课 SQL 里能验证。）\n"
         << "=== 本文件结束 ===\n";
    return 0;
}
