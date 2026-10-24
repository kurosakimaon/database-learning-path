// ============================================================================
//  第 5 关 · query_internals · 01_join_algorithms.cpp
//  四种 JOIN 算法同台竞技：嵌套循环 / 索引连接 / 排序归并 / 哈希连接
//
//  编译：g++ -std=c++17 -O2 01_join_algorithms.cpp -o 01_join.exe
//  运行：.\01_join.exe
//
//  场景（跟 03_explain_drills.sql 里那套表对得上）：
//      users  : 5 000 行   {id, name}          —— 小表（构建侧）
//      orders : 50 000 行  {id, uid, amount}   —— 大表（探测侧）
//      订单里掺约 2% 悬空 uid（指向不存在的用户），练一练 INNER JOIN 的语义。
//  查询：SELECT ... FROM orders o JOIN users u ON o.uid = u.id
//
//  老师傅先给你把世界观摆好：
//  JOIN 的本质就是两层 for 循环加一个 if（第 1 关说过的）。
//  四种算法没有发明新数学，它们只是在三样东西之间做取舍：
//      比较次数（CPU 的账）、内存（临时空间）、要不要预先有序/有索引。
//  优化器选哪种，就看这三样各自的价签。
// ============================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;

// ----------------------------------------------------------------------------
// 表结构与造数
// ----------------------------------------------------------------------------
struct User {
    int id;
    string name;
};
struct Order {
    int id;
    int uid;      // 指向 users.id，故意掺 2% 指向不存在的用户（悬空外键）
    int amount;
};

// 可复现的伪随机（LCG），保证每次跑出来的数字一样，方便你对照注释
struct Lcg {
    uint64_t s = 20240514;
    int next(int mod) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<int>((s >> 33) % static_cast<uint64_t>(mod));
    }
};

const int N_USERS  = 5000;    // users 行数
const int N_ORDERS = 50000;   // orders 行数
const int PAGE     = 4096;    // 页大小，算"页读次数"用

struct JoinResult {
    long long matched = 0;    // 拼出来的结果行数
    long long cmp     = 0;    // join key 比较次数（CPU 的账）
    long long io      = 0;    // 估算页读取次数（磁盘的账）
    long long memKB   = 0;    // 算法自己占的临时内存（KB，估算）
    double    ms      = 0;    // 真实耗时
};

static void banner(const char* t) {
    printf("\n============================================================\n");
    printf("  %s\n", t);
    printf("============================================================\n");
}

// ============================================================================
// 算法一：Block Nested Loop Join（块嵌套循环，没有索引没有有序时的兜底）
// ============================================================================
// 朴素 NLJ 是"外层每行扫一遍内层"：50 000 × 5 000 = 2.5 亿次比较，
// 内层还要被重读 50 000 遍。真实引擎至少做"块"优化：
// 外层的若干行（凑一页）一次装进内存，内层每扫一遍伺候一整块。
// 比较次数 = (外层块数 × 内层行数)，页读 = 外层 1 遍 + 内层"每块 1 遍"。
static JoinResult joinBNL(const vector<Order>& orders, const vector<User>& users) {
    JoinResult st;
    const int BLOCK = 100;                       // 一块装 100 个 order ≈ 一页
    const int usersPages =
        static_cast<int>((users.size() * sizeof(User) + PAGE - 1) / PAGE);

    auto t0 = Clock::now();
    size_t blocks = 0;
    for (size_t b = 0; b < orders.size(); b += BLOCK) {
        size_t e = min(orders.size(), b + BLOCK);
        blocks++;
        st.io += 1;                              // 读外层这一块 = 1 页
        for (const User& u : users) {            // 内层整表扫一遍
            for (size_t i = b; i < e; ++i) {     // 块内每一行都跟它比
                st.cmp++;
                if (orders[i].uid == u.id) st.matched++;
            }
        }
        st.io += usersPages;                     // 这一块的代价：内层整表 1 遍
    }
    st.ms = chrono::duration<double, milli>(Clock::now() - t0).count();
    st.memKB = BLOCK * sizeof(Order) / 1024 + 1; // 只驻留一块
    printf("  外层块数 = %zu，每块都要把 users 整表扫一遍\n", blocks);
    printf("  比较次数没省：还是 M×N = %lld 次（块内照样逐行比）。\n", st.cmp);
    printf("  分块省的是磁盘的账：朴素 NLJ 内层要被重扫 %d 遍（外层每行一遍），\n", N_ORDERS);
    printf("  分块后只扫 %zu 遍 —— 页读从 %lld 降到 %lld。CPU 没省，I/O 才是它的主战场。\n",
           blocks, (long long)N_ORDERS * usersPages, st.io);
    return st;
}

// ============================================================================
// 算法二：Index Join（嵌套循环 + 内层索引 —— NLJ 的"穿了鞋"版本）
// ============================================================================
// 外层照旧逐行流过去，但内层不再整表扫：拿 o.uid 去 users 的主键上
// 走一棵 B+ 树。每次探测 = 下降 树高 层：页读 = 树高 页（没被 Buffer Pool 接住时），
// 比较 = 每层一次页内二分（约 log₂扇出 次），不是"树高次"那么便宜。
// 这就是第 1 关 EXPLAIN 里 "SEARCH u USING INTEGER PRIMARY KEY" 的运行时真相。
static JoinResult joinIndexNL(const vector<Order>& orders, const vector<User>& users) {
    JoinResult st;

    // 预先把内层按 id 排好并建"树"（模拟主键索引；5000 行的树高 = 2 层）
    vector<int> order2id(users.size());
    for (size_t i = 0; i < users.size(); ++i) order2id[i] = users[i].id;
    sort(order2id.begin(), order2id.end());
    const int treeH = 2;                          // 5000 行、扇出 101：1 层叶 + 1 层根

    auto t0 = Clock::now();
    st.io += (N_ORDERS * (int)sizeof(Order) + PAGE - 1) / PAGE; // 外层整表 1 遍
    for (const Order& o : orders) {
        // 手写二分，比较次数一是一二是二地记账
        int lo = 0, hi = (int)order2id.size() - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            st.cmp++;
            if (order2id[mid] == o.uid) { st.matched++; break; }
            if (order2id[mid] < o.uid) lo = mid + 1; else hi = mid - 1;
        }
        st.io += treeH;                           // 每探测一次 = 走一趟树
    }
    st.ms = chrono::duration<double, milli>(Clock::now() - t0).count();
    st.memKB = order2id.size() * sizeof(int) / 1024 + 1;
    printf("  外层 %d 行 × 树高 %d = 页读 %lld。数字吓人？\n", N_ORDERS, treeH, st.io);
    // 索引总页数 = 叶子页 + 根页，别拿树高冒充页数（5000 行 / 100 行每页 = 50 叶 + 1 根）
    const int idxPages = (N_USERS + 99) / 100 + 1;
    printf("  但这棵索引满打满算就 %d 页，早被 Buffer Pool 焊死在内存里，\n", idxPages);
    printf("  真实 I/O 发生在冷缓存 / 大内表场景。小内表 + 索引 = 优化器的心头好。\n");
    return st;
}

// ============================================================================
// 算法三：Sort-Merge Join（排序归并 —— 两边先排好，然后像拉链一样拉过去）
// ============================================================================
// 归并阶段的美妙之处：两边各自单调，比较次数 ≈ 外层 + 内层 + 匹配输出，
// 仅仅 O(M+N)，是四种算法里归并阶段最省的。
// 代价全在前置的"排好"上。真实引擎里若 join 列上正好有索引，
// 序是白捡的（第 2 关说的叶子链表），排序费直接免——所以优化器
// 特别爱在有索引的列上用 merge。等值以外的条件（BETWEEN、<=）
// 也能用 merge，这是它独一份的本事。
static JoinResult joinSortMerge(const vector<Order>& orders, const vector<User>& users) {
    JoinResult st;

    auto t0 = Clock::now();
    // --- 排序阶段（模拟 sort 缓冲区：只拷 join key，等价于引擎里的"瘦身行"）---
    vector<int> lkey(orders.size());
    for (size_t i = 0; i < orders.size(); ++i) lkey[i] = orders[i].uid;
    vector<int> rkey(users.size());
    for (size_t i = 0; i < users.size(); ++i) rkey[i] = users[i].id;

    sort(lkey.begin(), lkey.end(), [&](int a, int b) { st.cmp++; return a < b; });
    sort(rkey.begin(), rkey.end(), [&](int a, int b) { st.cmp++; return a < b; });
    long long sortCmp = st.cmp;

    // --- 归并阶段 ---
    size_t i = 0, j = 0;
    while (i < lkey.size() && j < rkey.size()) {
        st.cmp++;
        if (lkey[i] == rkey[j]) {
            // 等值可能一对多：数两边各自的并列段，做小笛卡尔
            size_t i2 = i, j2 = j;
            while (i2 < lkey.size() && lkey[i2] == lkey[i]) i2++;
            while (j2 < rkey.size() && rkey[j2] == rkey[j]) j2++;
            st.matched += (long long)(i2 - i) * (j2 - j);
            i = i2; j = j2;
        } else if (lkey[i] < rkey[j]) {
            i++;
        } else {
            j++;
        }
    }
    st.io += (N_ORDERS * 4 + PAGE - 1) / PAGE * 2;  // 左半：写盘 1 遍 + 读回 1 遍
    st.io += (N_USERS * 4 + PAGE - 1) / PAGE * 2;   // 右半同理
    st.ms = chrono::duration<double, milli>(Clock::now() - t0).count();
    st.memKB = (lkey.size() + rkey.size()) * sizeof(int) / 1024 + 1;
    printf("  其中排序费 %lld 次比较（M log M + N log N），归并只用 %lld 次。\n",
           sortCmp, st.cmp - sortCmp);
    printf("  排序装不下的话要外部归并：分 run 落盘再两两合并，页读翻倍起跳——\n");
    printf("  这就是 EXPLAIN 里 \"USE TEMP B-TREE\" / MySQL 里 filesort 的账单。\n");
    return st;
}

// ============================================================================
// 算法四：Hash Join（哈希连接 —— 等值连接的大杀器）
// ============================================================================
// 两阶段：BUILD 拿小表建哈希表；PROBE 大表流过去逐行探测。
// 总比较 ≈ M + N（哈希直指目标，不用再扫），内存 = 小表的哈希表。
// PostgreSQL 多少年的默认等值连接算法；MySQL 到 8.0.18 才补上，
// 一补上就把大量 NL join 拍在沙滩上。
// 小表装不进内存怎么办？Grace Hash：两边按同一个 hash 分成 k 份落盘，
// 再逐对子文件做小哈希——分而治之，跟外部排序一个思路。
static JoinResult joinHash(const vector<Order>& orders, const vector<User>& users) {
    JoinResult st;
    unordered_map<int, int> table;                 // uid -> 用户数（本场景恒 1）
    table.reserve(users.size() * 2);

    auto t0 = Clock::now();
    // BUILD：只建小表。真实引擎还会顺手把投影后的"瘦身列"塞进去
    for (const User& u : users) {
        st.cmp++;                                  // 插桶时的 key 校验（防重/防碰撞）
        table[u.id]++;
    }
    // PROBE：大表流式探测，一行不留，不需要缓存外层
    for (const Order& o : orders) {
        auto it = table.find(o.uid);               // 哈希定位 O(1)
        st.cmp++;                                  // 找到桶后的 key 相等确认
        if (it != table.end()) st.matched += it->second;
    }
    st.io += (N_ORDERS * (int)sizeof(Order) + PAGE - 1) / PAGE; // 外层 1 遍
    st.io += (N_USERS * (int)sizeof(User) + PAGE - 1) / PAGE;   // 小表 1 遍
    st.ms = chrono::duration<double, milli>(Clock::now() - t0).count();
    st.memKB = (table.bucket_count() * sizeof(void*) +
                users.size() * (sizeof(int) * 2 + 16)) / 1024 + 1;
    printf("  BUILD 只建了 users（%d 行），orders 全程流过去一行不留。\n", N_USERS);
    printf("  比较次数 = 建表 %d + 探测 %d ≈ M+N，这就是 %lld 这个数的来历。\n",
           N_USERS, N_ORDERS, st.cmp);
    return st;
}

// ============================================================================
// main
// ============================================================================
int main() {
    banner("第 5 关开场白：JOIN 在生产端是怎么跑的");

    printf(R"(
第 1 关说过：JOIN 的本质是两层 for + 一个 if。
这一关要看的是——引擎拿到 o.uid = u.id 这个条件后，
到底用哪台"机器"去执行。候选就四台：

  1. Block Nested Loop —— 啥都没有时的苦力：分块，内层反复整表扫
  2. Index Join        —— 内层有索引：外层逐行，内层走 B+ 树点查
  3. Sort-Merge        —— 两边先排序（或索引白捡序），归并拉链
  4. Hash Join         —— 小表建哈希表，大表流式探测

数据：orders %d 行 JOIN users %d 行，ON o.uid = u.id。
掺了约 2%% 悬空 uid（指向不存在的用户），INNER JOIN 要把它们扔掉，
四种算法的 matched 必须一致——先对结果，再比速度，别拿错题比快慢。
)", N_ORDERS, N_USERS);

    // ---------------- 造数 ----------------
    Lcg rng;
    vector<User> users(N_USERS);
    for (int i = 0; i < N_USERS; ++i) {
        users[i].id = i + 1;
        users[i].name = "user_" + to_string(i + 1);
    }
    vector<Order> orders(N_ORDERS);
    long long dangling = 0;
    for (int i = 0; i < N_ORDERS; ++i) {
        orders[i].id = i + 1;
        // 前 5000 个 uid 留作悬空靶子：命中它们就拼不上
        int u = rng.next(100);
        orders[i].uid = (u < 2) ? (N_USERS + 1 + rng.next(500)) : (1 + rng.next(N_USERS));
        orders[i].amount = 100 + rng.next(9900);
        if (orders[i].uid > N_USERS) dangling++;
    }
    printf("\n造数完成：%d 订单、%d 用户，其中悬空 uid %lld 行（%.1f%%）。\n",
           N_ORDERS, N_USERS, dangling, 100.0 * dangling / N_ORDERS);

    // ---------------- 四台机器挨个跑 ----------------
    struct Row { const char* name; JoinResult r; };
    vector<Row> rows;

    banner("算法一：Block Nested Loop（分块嵌套循环）");
    rows.push_back({"BNL 分块嵌套", joinBNL(orders, users)});

    banner("算法二：Index Join（NL + 内层主键树）");
    rows.push_back({"Index Join", joinIndexNL(orders, users)});

    banner("算法三：Sort-Merge（排序 + 归并）");
    rows.push_back({"Sort-Merge", joinSortMerge(orders, users)});

    banner("算法四：Hash Join（哈希连接）");
    rows.push_back({"Hash Join", joinHash(orders, users)});

    // ---------------- 汇总 ----------------
    banner("四算法总账（matched 必须一致，先对结果再比快慢）");
    printf("  %-14s %12s %12s %12s %10s %10s\n",
           "算法", "比较次数", "页读(估)", "内存KB", "耗时ms", "matched");
    for (auto& x : rows)
        printf("  %-14s %12lld %12lld %12lld %10.2f %10lld\n",
               x.name, x.r.cmp, x.r.io, x.r.memKB, x.r.ms, x.r.matched);

    long long m0 = rows[0].r.matched;
    bool agree = true;
    for (auto& x : rows) if (x.r.matched != m0) agree = false;
    printf("\n  四种算法结果%s：%lld 行（= %d - 悬空 %lld）。\n",
           agree ? "一致 ✓" : "竟然不一致 ✗（有 bug！）", m0, N_ORDERS, dangling);

    // ---------------- 老师傅总结：优化器什么时候选哪种 ----------------
    banner("优化器怎么选：看三张价签");
    printf(R"(
  选谁不看心情，看三张价签：外层多大、内层有没有索引、两边排没排序。

  ▸ Index Join —— 外层小（或被 LIMIT 截了小）、内层 join 列有索引。
      成本 ≈ 外层行数 × 树高。第 1 关"驱动表"说的就是它：
      谁当外层谁被逐行伺候，所以驱动表要选小的。
      外层大了就翻车：50 000 次点查听着爽，内表千万行、树高 4、
      缓存还接不住时，就是 20 万次随机 I/O —— 直接劝退。

  ▸ Hash Join —— 等值连接、无索引可用、小边装得进内存。
      比较次数 O(M+N)，把 BNL 的 M×N 打成渣。
      PostgreSQL 的默认主力；MySQL 8.0.18+ 顶替了大量场景的 NLJ。
      装不进去就 Grace Hash（按 hash 分区落盘再逐对做）；
      左大右更大时还会 BKA（batch key access）攒一批 key 一起去探测。
      局限：只吃等值。o.uid = u.id 行，o.uid BETWEEN u.lo AND u.hi 不行。

  ▸ Sort-Merge —— 两边已有序（或索引白捡序）、或要等值之外的比较、
      或结果还要保持 join key 有序喂给下游 ORDER BY / merge 聚合。
      归并阶段 O(M+N) 漂亮，但"先排好"三个字可能是天价（外部排序），
      所以优化器爱把它用在"序是现成的"场合 —— join 列上有索引时最风光。

  ▸ BNL —— 兜底。没索引、没内存建哈希、又不等值（如 BETWEEN join），
      才轮到它。MySQL 5.x 时代被骂惨的就是"动不动掉进 BNL"。

  一句话记忆：
      有索引走 Index Join；等值没索引走 Hash Join；
      有序（或要序）走 Sort-Merge；BETWEEN 大表走 Sort-Merge；
      实在没辙才 BNL。下一文件 02_cost_model.cpp 就把"怎么选"
      写成真正的算术：代价 = 页读 × IO_COST + 比较 × CPU_COST。
)");
    return 0;
}
