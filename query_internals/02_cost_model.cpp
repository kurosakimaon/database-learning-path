// ============================================================================
//  第 5 关 · query_internals · 02_cost_model.cpp
//  亲手实现一个迷你优化器的"会计部分"：代价 = 页读×IO_COST + 比较×CPU_COST
//
//  编译：g++ -std=c++17 -O2 02_cost_model.cpp -o 02_cost.exe
//  运行：.\02_cost.exe
//
//  本文件回答三个问题：
//    1. 优化器嘴里的"代价"到底是笔什么账？（就是上面那个公式）
//    2. 为什么它会选错计划？—— 公式没错，喂给公式的"统计"是旧的
//    3. ANALYZE 在补什么？MySQL 放着索引不走近全表，账是怎么算的？
//
//  C++ 类比：优化器 = 拿静态 profile 的编译器 + 一本会过期的缓存统计。
//  它从不真的执行 SQL，全靠估——估的是"页数"和"行数"，不是时间。
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace std;

// ----------------------------------------------------------------------------
// 代价常数（各家引擎都有这么一对"汇率"，靠标定测出来）
// SQLite 源码里一页 IO 记 1、一次 CPU 操作记 0.005 上下；
// MySQL 的 handler::read_time 也大致是这个量级关系。
// 注意一个通病：这里随机 IO 和顺序 IO 同价 —— 这是很多"选错计划"的
// 第二根源（第一根源是本文件主角：统计过期）。
// ----------------------------------------------------------------------------
const double IO_COST  = 1.0;    // 读一页
const double CPU_COST = 0.005;  // 一次比较

// ----------------------------------------------------------------------------
// 表与索引的物理信息（这部分一般不过期：page count 现查就行）
// ----------------------------------------------------------------------------
struct TableInfo {
    const char* name;
    int rows;         // 行数（会过期的统计！）
    int pages;        // 表页数
    int treeH;        // 主键 B+ 树树高
    int rowsPerPage;
};
TableInfo T_ORDERS = {"orders", 50000, 500, 3, 100};   // 4096 页装 100 行
TableInfo T_USERS  = {"users",  5000,  50,  2, 100};

// orders.amount 上的二级索引 idx_amount
const int IDX_PAGES_PER_ROW = 200;   // 索引页更窄，一页 200 个 key
const int IDX_TREE_H = 2;

// 一次主键点查在页内要做几次比较（树高×每层二分）
int probeCmp(const TableInfo& t) { return t.treeH * 7; }

// ----------------------------------------------------------------------------
// 统计信息：本关只需要一条 —— amount > 100 的选择率（过滤后剩多少行）
// 真实引擎里这是直方图/采样估计出来的，而且会【过期】：
// 你 bulk update 了 5 万行，它可能还记着上个月的分布。
// ----------------------------------------------------------------------------
struct Stats { double selAmountGt100; };

Stats STALE = {0.001};  // ANALYZE 之前的旧统计：以为 amount>100 只剩 0.1%
Stats FRESH = {0.99};   // ANALYZE 重新采样：其实现在剩 99%
                        // 背景：上个月跑过一次 UPDATE orders
                        //         SET amount = amount * 100;
                        // 数据变了，旧统计没跟上 —— 事故就这么埋下的。

// ----------------------------------------------------------------------------
// 候选计划。每个计划 = join 顺序 × join 算法 × 访问路径 的一种组合。
// estimate() 只吃 Stats，不碰真实数据 —— 优化器的视角就到这儿为止。
// realSel 传真实选择率时，得到的是"这条计划真跑起来"的账（上帝视角，
// 只为让你看见估错的多离谱，程序里才这么对比，引擎自己看不见）。
// ----------------------------------------------------------------------------
struct PlanCost {
    string name;
    long long pages = 0;
    long long cmp = 0;
    double cost = 0;
    string formula;   // 账目明细，打印出来给你看它怎么按的算盘
};

double price(long long pages, long long cmp) {
    return pages * IO_COST + cmp * CPU_COST;
}

// 计划 A：orders 全表扫（顺带过滤）→ HASH JOIN（users 建表）
// 这是 01 里 hash join 的代价公式：页 = 两边各扫一遍；比较 = M+N。
PlanCost planHashJoin(const Stats& s) {
    PlanCost p; p.name = "A 全表扫 orders + Hash Join(users 建表)";
    long long est = (long long)(T_ORDERS.rows * s.selAmountGt100); // 过滤后行数
    p.pages = T_ORDERS.pages + T_USERS.pages;
    p.cmp   = T_ORDERS.rows      // 全表逐行过滤 amount>100
            + T_USERS.rows       // BUILD：users 进哈希表
            + est;               // PROBE：过滤后的 orders 逐行探测
    p.cost = price(p.pages, p.cmp);
    p.formula = "页 = 500(orders) + 50(users) = 550；"
                "比较 = 50000(过滤) + 5000(建表) + est(探测)";
    return p;
}

// 计划 B：orders 全表扫（顺带过滤）→ INDEX NL（每行拿 uid 点查 users 主键）
PlanCost planFullNL(const Stats& s) {
    PlanCost p; p.name = "B 全表扫 orders + Index NL(点查 users 主键)";
    long long est = (long long)(T_ORDERS.rows * s.selAmountGt100);
    p.pages = T_ORDERS.pages + est * T_USERS.treeH;   // 驱动表 1 遍 + 每行走一趟树
    p.cmp   = T_ORDERS.rows + est * probeCmp(T_USERS);
    p.cost = price(p.pages, p.cmp);
    p.formula = "页 = 500 + est×2(users 树高)；比较 = 50000 + est×14";
    return p;
}

// 计划 C：走 idx_amount 范围扫 → 回表 → INDEX NL 点查 users
// 关键：est 行 × 随机回表 + est × users 树高，全是按【估计行数】算的。
// 统计一过期，est 就是无根之木。
PlanCost planIndexNL(const Stats& s) {
    PlanCost p; p.name = "C idx_amount 范围扫 + 回表 + Index NL";
    long long est = (long long)(T_ORDERS.rows * s.selAmountGt100);
    long long idxPages  = IDX_TREE_H + (est + IDX_PAGES_PER_ROW - 1) / IDX_PAGES_PER_ROW;
    long long heapPages = est;                  // 每行一次随机回表（非覆盖索引）
    long long joinPages = est * T_USERS.treeH;  // 每行再走一趟 users 主键树
    p.pages = idxPages + heapPages + joinPages;
    p.cmp   = est           // 索引页内顺序扫 key
            + est           // 回表后复查 WHERE（引擎习惯再验一遍）
            + est * probeCmp(T_USERS);
    p.cost = price(p.pages, p.cmp);
    p.formula = "页 = 索引(2+est/200) + 回表(est×1 随机) + join(est×2)；"
                "比较 = est×(1+1+14)";
    return p;
}

// 计划 D：orders 过滤后按 uid 排序 → 与（天然按 id 有序的）users 归并
// 排序费在这里注定白交：users 的序是主键白捡的，orders 的序却得现排。
// 若 orders 上恰好有 (uid) 索引，序就是免费的，D 立刻翻身 ——
// 这就是"索引帮 join 提速"的又一种姿势（省排序，不只省查找）。
PlanCost planSortMerge(const Stats& s) {
    PlanCost p; p.name = "D 过滤 + 按 uid 排序 + Sort-Merge(users 已有序)";
    long long est = (long long)(T_ORDERS.rows * s.selAmountGt100);
    long long sortCmp = (long long)(est * (log2(max(2.0, (double)est))));
    long long sortPages = (est / T_ORDERS.rowsPerPage) * 2; // 写 run + 归并读回
    p.pages = T_ORDERS.pages + sortPages + T_USERS.pages;
    p.cmp   = T_ORDERS.rows + sortCmp + est + T_USERS.rows; // 过滤+排序+归并
    p.cost = price(p.pages, p.cmp);
    p.formula = "页 = 500 + 排序落盘(est/100×2) + 50；比较 = 50000 + est×log2(est) + est + 5000";
    return p;
}

// ----------------------------------------------------------------------------
vector<PlanCost> enumerate(const Stats& s) {
    vector<PlanCost> v;
    v.push_back(planHashJoin(s));
    v.push_back(planFullNL(s));
    v.push_back(planIndexNL(s));
    v.push_back(planSortMerge(s));
    return v;
}

void printTable(const vector<PlanCost>& v, const char* title, int chosen) {
    printf("\n%s\n", title);
    printf("  %-44s %8s %10s %10s\n", "候选计划", "页读", "比较", "代价");
    for (size_t i = 0; i < v.size(); ++i) {
        printf("  %s %-42s %8lld %10lld %10.1f\n",
               ((int)i == chosen) ? "★" : " ", v[i].name.c_str(),
               v[i].pages, v[i].cmp, v[i].cost);
    }
    printf("  （账目明细）\n");
    for (auto& x : v) printf("    %s\n", x.formula.c_str());
}

static void banner(const char* t) {
    printf("\n============================================================\n");
    printf("  %s\n", t);
    printf("============================================================\n");
}

int main() {
    banner("第 5 关：优化器是个会计，不是预言家");
    printf(R"(
查询（跟 explain_pipeline.html 里演的是同一条）：

  SELECT u.name
  FROM orders o JOIN users u ON o.uid = u.id
  WHERE o.amount > 100;

优化器手里没有水晶球，只有一本账 + 一份统计。公式朴素得令人感动：

  代价 = 预计页读次数 × IO_COST(1.0) + 预计比较次数 × CPU_COST(0.005)

四个候选计划（join 顺序 × 算法 × 访问路径 的组合）：
  A 全表扫 orders + Hash Join          —— 两边各扫一遍，哈希 O(M+N)
  B 全表扫 orders + Index NL           —— 过滤后逐行点查 users 主键
  C idx_amount 范围扫 + 回表 + Index NL —— 看起来最"聪明"：用上索引了
  D 过滤 + 排序 + Sort-Merge           —— users 有序是白捡，orders 得现排

现在把统计喂进去。注意：优化器【看不见真实数据】，只能用统计估行数。)");

    // ------------------------------------------------------------------
    // 第一轮：优化器眼前只有旧统计（上个月 bulk update 之后没 ANALYZE）
    // ------------------------------------------------------------------
    banner("第一轮：优化器手里是【过期统计】（以为 amount>100 只剩 0.1%）");
    printf("  背景：上个月跑过 UPDATE orders SET amount = amount * 100;\n");
    printf("  真实选择率现在是 99%%，但统计还停在 0.1%% —— 它不知道。\n");

    auto stalePlans = enumerate(STALE);
    int pick = 0;
    for (int i = 1; i < (int)stalePlans.size(); ++i)
        if (stalePlans[i].cost < stalePlans[pick].cost) pick = i;
    printTable(stalePlans, "各候选计划的估计代价：", pick);
    printf("\n  会计拍板：选 %s\n", stalePlans[pick].name.c_str());
    printf("  理由冠冕堂皇：范围扫只取 0.1%% 的行（50 行），回表才 50 次，\n");
    printf("  比全表扫 500 页「便宜」。—— 算盘没错，料给错了。\n");

    // ------------------------------------------------------------------
    // 第二轮：上帝视角。用真实选择率 99% 重算这些计划"真跑起来"的账。
    // ------------------------------------------------------------------
    banner("第二轮：真跑一遍（真实选择率 99%）—— 看它错得多离谱");
    auto realPlans = enumerate(FRESH);
    printTable(realPlans, "同一批计划，按真实选择率重算的【实际】代价：", -1);
    printf("\n  它选了 %s：估计 %.1f，实际跑出来 %.1f —— 差 %.0f 倍。\n",
           stalePlans[pick].name.c_str(), stalePlans[pick].cost, realPlans[pick].cost,
           realPlans[pick].cost / max(1e-9, stalePlans[pick].cost));
    // 按真实代价找全场最低，跟它选的对比
    int bestReal = 0;
    for (int i = 1; i < (int)realPlans.size(); ++i)
        if (realPlans[i].cost < realPlans[bestReal].cost) bestReal = i;
    printf("  真正最便宜的其实是 %s（%.1f），只有它所选计划的 1/%.0f。\n",
           realPlans[bestReal].name.c_str(), realPlans[bestReal].cost,
           realPlans[pick].cost / max(1e-9, realPlans[bestReal].cost));
    printf("  病灶复盘：计划 C 的页读 = 索引页 + 回表 + join 探测，三项全乘 est。\n");
    printf("  est 低估 %d 倍（0.1%% vs 99%%），代价就崩到天上。优化器不傻，是账本过期：\n",
           (int)(FRESH.selAmountGt100 / STALE.selAmountGt100));
    printf("      公式没错 × 统计过期 = 灾难。\n");

    // ------------------------------------------------------------------
    // 第三轮：ANALYZE —— 重新采样统计，再让同一个会计算一遍
    // ------------------------------------------------------------------
    banner("第三轮：执行 ANALYZE 之后（统计更新到 99%），同一个会计重新打分");
    auto freshPlans = enumerate(FRESH);
    int pick2 = 0;
    for (int i = 1; i < (int)freshPlans.size(); ++i)
        if (freshPlans[i].cost < freshPlans[pick2].cost) pick2 = i;
    printTable(freshPlans, "ANALYZE 后的估计代价：", pick2);
    printf("\n  现在它选了 %s —— 和上帝视角的正确答案一致。\n", freshPlans[pick2].name.c_str());
    printf("  估计 %0.1f ≈ 实际 %0.1f。同一个公式，换了本新鲜的账，立刻判若两人。\n",
           freshPlans[pick2].cost, realPlans[pick2].cost);

    // ------------------------------------------------------------------
    // 老师傅总结
    // ------------------------------------------------------------------
    banner("老师傅总结：ANALYZE 到底补了什么 & MySQL 不走索引的三笔账");
    printf(R"(
  ◆ ANALYZE 补的是"行数估计"这一块拼图
      SQLite：ANALYZE 把每张表/每个索引的 row 数和每个前缀的 distinct
              数写进 sqlite_stat1 表；后续查询按它估行数。
      MySQL ：ANALYZE TABLE 采样 20 个页（innodb_stats_persistent_sample_pages）
              刷新统计；8.0 还能 BUILD STATISTICS 建列直方图。
      本文件只演了一个统计量（选择率），真实引擎是一整本：
      行数、每列 distinct 数、直方图、相关性 —— 每一页都可能过期。
      常见事故现场：bulk load / 大批量 UPDATE 之后没 ANALYZE，
      或自动采样还没跑，优化器拿着上个月的地图找今天的路。

  ◆ "MySQL 放着索引不走"的三笔账（全是同一个会计算出来的）
      1) 选择性账：过滤条件命中全表 20%% 以上（经验阈值，跟页
         填充率有关），"回表 est 次随机 I/O" 比 "顺序扫全表" 还贵。
         优化器算过账后【主动】放弃索引 —— 这是对的，不是 bug。
         本文件计划 C 在新统计下的下场就是它。
      2) 估算账：MySQL 的行数估计靠采样几个索引页外推，遇倾斜
         数据（某个值占 90%%）能错一个数量级，于是该走索引的
         也没走。解法：ANALYZE TABLE、直方图、或 FORCE INDEX 兜底。
      3) 资格账：WHERE UPPER(name)=... / phone+0=13800... 这种
         写法把索引直接踢出候选（不是贵，是没资格 —— 树按原值
         排序，按变换后的值走不了树）。第 2 关 03 的 5.x 节演的
         就是这一类。改写法（sargable）比调参数有用。

  ◆ 顺带一提的两个"会计盲区"
      - 本模型随机 I/O 与顺序 I/O 同价。InnoDB/MySQL 会给范围扫
        打折（multi-block read 便宜），PostgreSQL 用
        random_page_cost=4 显式区分。所以同一个查询两边可能选不同计划。
      - 代价模型不知道你的 Buffer Pool 里正躺着哪些页（缓存是
        实例状态，不是统计）。热缓存时全表扫其实更便宜 ——
        这也是 PG 的 pg_prewarm / MySQL 的"预热"存在的意义。

  下一文件 04_wal_and_crash.cpp 换一半矛盾：内存断电即失。
  写账本（WAL）怎么就保住了数据？崩溃注入三次，恢复跑三遍。)");
    return 0;
}
