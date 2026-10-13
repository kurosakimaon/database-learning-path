// ============================================================================
//  01_data_models.cpp —— NoSQL 四大数据模型，一个文件全摸一遍
//
//  编译：g++ -std=c++17 -O2 -Wall -Wextra 01_data_models.cpp -o 01.exe
//  运行：.\01.exe
//
//  关系模型（第 1、3 关那一套）只有一种结构：二维表。够用，但不是万能：
//    · 缓存/计数/会话 —— 表结构纯属累赘，key 进 value 出就完了（KV）
//    · 商品/内容/CMS  —— 字段天生嵌套且经常变，建表 ALTER 到头秃（文档）
//    · 指标/日志/IoT  —— 十亿行只按天查某几列，全行存纯属浪费（列族）
//    · 社交/推荐/风控 —— 灵魂查询是「朋友的朋友」，JOIN 到地老天荒（图）
//
//  老师傅的话放在文件末尾，先把四个模型跑起来再说。
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// 四大流派的身份证
enum class DataModel {
    KeyValue,    // 键值    —— 代表：Redis     擅长：按 key 点查、计数器、缓存、分布式锁
    Document,    // 文档    —— 代表：MongoDB   擅长：嵌套对象整体存取、字段灵活的 CRUD
    ColumnFamily,// 列族    —— 代表：Cassandra 擅长：海量写入、按行键取若干列、时序/指标
    Graph        // 图      —— 代表：Neo4j     擅长：多跳关系遍历（朋友的朋友）、最短路径
};

static const char *modelName(DataModel m) {
    switch (m) {
    case DataModel::KeyValue:     return "键值 KV（Redis 的路子）";
    case DataModel::Document:     return "文档 Document（MongoDB 的路子）";
    case DataModel::ColumnFamily: return "列族 Column-Family（Cassandra 的路子）";
    case DataModel::Graph:        return "图 Graph（Neo4j 的路子）";
    }
    return "?";
}

static void banner(const std::string &s) {
    std::cout << "\n============================================================\n"
              << "  " << s << "\n"
              << "============================================================\n";
}

// ============================================================================
// 模型一：键值存储 —— 本质就是一张哈希表
// Redis 的 string/hash/zset 底下都是这意思：key 哈希一下直达 value，
// O(1) 点查，没有 schema、没有 JOIN、没有事务（Redis 的事务只是批量打包）。
// ============================================================================
class KeyValueStore {
    std::unordered_map<std::string, std::string> map_;
public:
    void set(const std::string &k, const std::string &v) { map_[k] = v; }
    std::optional<std::string> get(const std::string &k) const {
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }
    bool del(const std::string &k) { return map_.erase(k) > 0; }
    // Redis 的 INCR：计数这种活儿用 SQL 要 SELECT+UPDATE 两步，这里一步
    long long incr(const std::string &k) {
        long long v = 0;
        auto it = map_.find(k);
        if (it != map_.end()) v = std::stoll(it->second);
        map_[k] = std::to_string(++v);
        return v;
    }
    size_t size() const { return map_.size(); }
};

// ============================================================================
// 模型二：文档存储 —— 嵌套 map + 序列化
// MongoDB 存的是 BSON（二进制 JSON）。文档模型的第一性原理：
// 「一起读的东西存一起」（订单连明细一个文档拿走），
// 代价是跨文档的关联和事务变贵了。
// 这里用递归 map 模拟文档，序列化输出 JSON 样子货。
// ============================================================================
struct DocValue;
using Doc = std::map<std::string, DocValue>;   // 有序 map，输出稳定好读

struct DocValue {
    enum Kind { Str, Num, Sub, Arr } kind = Str;
    std::string s;
    long long n = 0;
    Doc sub;
    std::vector<DocValue> arr;

    static DocValue mkStr(std::string v) { DocValue d; d.kind = Str;  d.s = std::move(v); return d; }
    static DocValue mkNum(long long v)   { DocValue d; d.kind = Num;  d.n = v;            return d; }
    static DocValue mkSub(Doc v)         { DocValue d; d.kind = Sub;  d.sub = std::move(v); return d; }
    static DocValue mkArr(std::vector<DocValue> v) { DocValue d; d.kind = Arr; d.arr = std::move(v); return d; }
};

static std::string dump(const DocValue &v);   // 前向声明（递归）

static std::string dump(const Doc &d) {
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (const auto &kv : d) {
        if (!first) os << ", ";
        first = false;
        os << "\"" << kv.first << "\": " << dump(kv.second);
    }
    os << "}";
    return os.str();
}
static std::string dump(const DocValue &v) {
    switch (v.kind) {
    case DocValue::Str: return "\"" + v.s + "\"";
    case DocValue::Num: return std::to_string(v.n);
    case DocValue::Sub: return dump(v.sub);
    case DocValue::Arr: {
        std::ostringstream os;
        os << "[";
        for (size_t i = 0; i < v.arr.size(); ++i) {
            if (i) os << ", ";
            os << dump(v.arr[i]);
        }
        os << "]";
        return os.str();
    }
    }
    return "?";
}

class DocumentStore {
    // MongoDB 的集合（collection）≈ 不需要建表、行与行结构可以不同的表
    std::map<std::string, Doc> coll_;
public:
    void insert(const std::string &id, Doc d) { coll_[id] = std::move(d); }
    const Doc *find(const std::string &id) const {
        auto it = coll_.find(id);
        return it == coll_.end() ? nullptr : &it->second;
    }
    // 「更新嵌套字段」：MongoDB 里是 $set {"items.0.qty": 5}。
    // 路径法：address.city = "苏州"。SQL 要 JSON_SET，Mongo 原生就是点路径。
    bool setPath(const std::string &id, const std::string &path, DocValue v) {
        auto it = coll_.find(id);
        if (it == coll_.end()) return false;
        Doc *cur = &it->second;
        size_t pos = 0;
        while (true) {
            size_t dot = path.find('.', pos);
            std::string key = path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
            if (dot == std::string::npos) { (*cur)[key] = std::move(v); return true; }
            auto &child = (*cur)[key];
            if (child.kind != DocValue::Sub) child = DocValue::mkSub({});
            cur = &child.sub;
            pos = dot + 1;
        }
    }
    size_t size() const { return coll_.size(); }
};

// ============================================================================
// 模型三：列族 —— 行键 + 「列的地图」，稀疏存储
// Cassandra/HBase 的路子。一行不是一个定长结构，而是
//   (rowKey, columnName) -> (value, timestamp)
// 的映射。两个杀手锏：
//   1. 稀疏：没写过的列不占空间（SQL 的 NULL 也要占位/位图）
//   2. 按列族分别落盘：只读 price 列时，绝不碰 description 列的磁盘页
// 「列族」= 一捆经常一起读的列打包存放，所以叫列【族】。
// ============================================================================
class ColumnFamilyStore {
    struct Cell { std::string value; long long ts; };
    // rowKey -> (列名 -> 最新 cell)。真系统里同一列存多版本（按 ts 排序的宽行），
    // 这里保留最新值 + 版本计数，足够看清结构。
    std::map<std::string, std::map<std::string, Cell>> rows_;
    long long versions_ = 0;
public:
    void put(const std::string &row, const std::string &col, const std::string &v) {
        rows_[row][col] = Cell{v, ++versions_};
    }
    std::optional<std::string> get(const std::string &row, const std::string &col) const {
        auto r = rows_.find(row);
        if (r == rows_.end()) return std::nullopt;
        auto c = r->second.find(col);
        if (c == r->second.end()) return std::nullopt;   // 稀疏：没写过就是不存在
        return c->second.value;
    }
    // 列裁剪：只取指定列（真系统按列族分别读盘，这里数一下"碰了几列"做对比）
    std::map<std::string, std::string> slice(const std::string &row,
                                             const std::vector<std::string> &cols,
                                             size_t *touched) const {
        std::map<std::string, std::string> out;
        if (touched) *touched = 0;
        auto r = rows_.find(row);
        if (r == rows_.end()) return out;
        for (const auto &c : cols) {
            auto it = r->second.find(c);
            if (it != r->second.end()) {
                out[c] = it->second.value;
                if (touched) ++*touched;
            }
        }
        return out;
    }
    // 宽行全貌（打印稀疏结构用）
    std::vector<std::string> columnsOf(const std::string &row) const {
        std::vector<std::string> out;
        auto r = rows_.find(row);
        if (r == rows_.end()) return out;
        for (const auto &kv : r->second) out.push_back(kv.first);
        return out;
    }
};

// ============================================================================
// 模型四：图 —— 邻接表 + BFS 最短路径
// Neo4j 的路子。图库的第一性原理：关系是【一等公民】，
// 存的时候就存成指针/边，查询顺着边跳（index-free adjacency），
// 多跳遍历的成本只和「摸到的边数」有关，和全库规模无关。
// SQL 里同一条「朋友的朋友」：JOIN 自己两次，十亿用户时想象一下。
// ============================================================================
class GraphStore {
    std::map<std::string, std::set<std::string>> adj_;   // 无向图的邻接表
public:
    void addNode(const std::string &n) { adj_[n]; }
    void addEdge(const std::string &a, const std::string &b) {
        adj_[a].insert(b);
        adj_[b].insert(a);
    }
    size_t nodes() const { return adj_.size(); }
    size_t edges() const {
        size_t e = 0;
        for (const auto &kv : adj_) e += kv.second.size();
        return e / 2;   // 无向边记了两遍
    }
    // BFS 最短路径：图库的看家本领「跳数」。返回路径；不可达返回空。
    std::vector<std::string> shortest(const std::string &a, const std::string &b) const {
        if (a == b) return {a};
        std::map<std::string, std::string> prev;                 // 前驱表
        std::queue<std::string> q;
        std::set<std::string> seen{a};
        q.push(a);
        while (!q.empty()) {
            std::string u = q.front(); q.pop();
            auto it = adj_.find(u);
            if (it == adj_.end()) continue;
            for (const auto &v : it->second) {
                if (seen.count(v)) continue;
                seen.insert(v);
                prev[v] = u;
                if (v == b) {                                    // 回溯路径
                    std::vector<std::string> path{b};
                    for (std::string cur = b; cur != a; cur = prev[cur]) path.push_back(prev[cur]);
                    std::reverse(path.begin(), path.end());
                    return path;
                }
                q.push(v);
            }
        }
        return {};
    }
    // 「朋友的朋友」推荐：二跳邻居里去掉直接朋友和自己，按共同好友数排序
    std::vector<std::pair<int, std::string>> friendOfFriend(const std::string &who) const {
        std::map<std::string, int> cnt;
        auto oneHop = adj_.count(who) ? adj_.at(who) : std::set<std::string>{};
        for (const auto &f : oneHop) {
            auto it = adj_.find(f);
            if (it == adj_.end()) continue;
            for (const auto &ff : it->second)
                if (ff != who && !oneHop.count(ff)) ++cnt[ff];   // 二跳、非直接朋友
        }
        std::vector<std::pair<int, std::string>> out;
        for (const auto &kv : cnt) out.emplace_back(kv.second, kv.first);
        std::sort(out.rbegin(), out.rend());                     // 共同好友多的排前面
        return out;
    }
};

// ============================================================================
//  演示
// ============================================================================
static void demoKV() {
    banner("模型一：键值 KV —— Redis 擅长什么，一眼看穿");
    std::cout << "  " << modelName(DataModel::KeyValue) << "\n\n";
    KeyValueStore redis;
    redis.set("session:a1b2c3", "user=42|exp=3600");
    redis.set("rank:hot", "post_77");
    std::cout << "  SET session:a1b2c3 → GET 回来: " << *redis.get("session:a1b2c3") << "\n";
    std::cout << "  GET 不存在的 key  → " << (redis.get("session:xxx") ? "有值" : "(nil)，这就是点查的全部世界\n");
    for (int i = 0; i < 3; ++i) redis.incr("count:page_home");
    std::cout << "  INCR count:page_home ×3 → " << *redis.get("count:page_home")
              << "   << 计数器一条命令搞定，SQL 得两行加事务\n";
    std::cout << "  擅长：点查、计数、缓存、排行榜、分布式锁。不擅长：查询「value 里 name 叫张三的所有 key」——只能 SCAN 全表。\n";
}

static void demoDocument() {
    banner("模型二：文档 —— 一次查询抱走整个订单");
    std::cout << "  " << modelName(DataModel::Document) << "\n\n";
    DocumentStore orders;
    Doc order;
    order["_id"]    = DocValue::mkStr("order_1001");
    order["user"]   = DocValue::mkStr("alice");
    order["total"]  = DocValue::mkNum(25900);                    // 分单位存钱，老规矩
    Doc addr;
    addr["city"] = DocValue::mkStr("杭州");
    addr["zip"]  = DocValue::mkStr("310000");
    order["address"] = DocValue::mkSub(std::move(addr));
    std::vector<DocValue> items;
    {
        Doc i1; i1["sku"] = DocValue::mkStr("kb-01"); i1["qty"] = DocValue::mkNum(2);
        Doc i2; i2["sku"] = DocValue::mkStr("ms-07"); i2["qty"] = DocValue::mkNum(1);
        items.push_back(DocValue::mkSub(std::move(i1)));
        items.push_back(DocValue::mkSub(std::move(i2)));
    }
    order["items"] = DocValue::mkArr(std::move(items));
    orders.insert("order_1001", std::move(order));

    std::cout << "  插入订单文档，一次 find 拿到完整结构（SQL 要 orders JOIN order_items）：\n    "
              << dump(*orders.find("order_1001")) << "\n\n";
    orders.setPath("order_1001", "address.city", DocValue::mkStr("苏州"));
    std::cout << "  $set address.city = 苏州 后：\n    " << dump(*orders.find("order_1001")) << "\n";
    std::cout << "  擅长：字段灵活、嵌套整体读写的内容/商品/配置类业务。\n"
              << "  代价：跨文档「JOIN」变贵，多文档事务直到 Mongo 4.0 才有且很贵。\n";
}

static void demoColumnFamily() {
    banner("模型三：列族 —— 稀疏宽行 + 列裁剪");
    std::cout << "  " << modelName(DataModel::ColumnFamily) << "\n\n";
    ColumnFamilyStore cf;
    // 两张订单：一张有物流列，一张没有 —— 稀疏存储，没写的列不存在而非 NULL
    cf.put("order/1001", "status", "PAID");
    cf.put("order/1001", "amount", "25900");
    cf.put("order/1001", "carrier", "SF");        // 只有 1001 发货了
    cf.put("order/1002", "status", "CREATED");
    cf.put("order/1002", "amount", "9900");

    std::cout << "  order/1001 实际存在的列: ";
    for (auto &c : cf.columnsOf("order/1001")) std::cout << c << " ";
    std::cout << "\n  order/1002 实际存在的列: ";
    for (auto &c : cf.columnsOf("order/1002")) std::cout << c << " ";
    std::cout << "   << 没有 carrier，不是 NULL，是压根不存在\n";

    size_t touched = 0;
    auto part = cf.slice("order/1001", {"status", "amount"}, &touched);
    std::cout << "  只读 status+amount 两列 → 拿到 " << part.size()
              << " 列，磁盘上只碰了 " << touched
              << " 列的数据 << 列裁剪：description 那种大列根本不落眼\n";
    std::cout << "  擅长：海量写入 + 已知 rowKey 取指定列（监控指标、IoT、消息流水）。\n"
              << "  代价：schema 围绕查询设计（一行一种查询），ad-hoc 即席查询基本没门。\n";
}

static void demoGraph() {
    banner("模型四：图 —— 「朋友的朋友」是亲儿子查询");
    std::cout << "  " << modelName(DataModel::Graph) << "\n\n";
    GraphStore g;
    //       alice ─ bob ─ carol ─ david
    //         │               │
    //        erin          frank ─ grace
    g.addEdge("alice", "bob");   g.addEdge("bob", "carol");
    g.addEdge("carol", "david"); g.addEdge("alice", "erin");
    g.addEdge("carol", "frank"); g.addEdge("frank", "grace");
    std::cout << "  七个节点 " << g.nodes() << "、边 " << g.edges() << " 条\n";

    auto p = g.shortest("alice", "grace");
    std::cout << "  最短路径 alice→grace（" << p.size() - 1 << " 跳）: ";
    for (size_t i = 0; i < p.size(); ++i) std::cout << (i ? " → " : "") << p[i];
    std::cout << "\n";
    auto none = g.shortest("david", "erin");
    auto p2 = g.shortest("erin", "david");
    std::cout << "  erin→david（" << p2.size() - 1 << " 跳）: ";
    for (size_t i = 0; i < p2.size(); ++i) std::cout << (i ? " → " : "") << p2[i];
    std::cout << "   << 顺边跳，和图库里有多少亿用户无关\n";
    (void)none;

    auto ff = g.friendOfFriend("alice");
    std::cout << "  给 alice 做好友推荐（二跳、按共同好友数）: ";
    for (auto &kv : ff) std::cout << kv.second << "(" << kv.first << "个共同) ";
    std::cout << "\n  擅长：社交、推荐、风控（团伙挖掘 = 找稠密子图）、知识图谱。\n"
              << "  代价：聚合类分析（全表 GROUP BY）不行，那是列式 OLAP 的活。\n";
}

int main() {
    std::cout << "================================================================\n"
              << "  NoSQL 四大数据模型速览（每个模型 = 一种「查询形状」的取舍）\n"
              << "================================================================\n";
    demoKV();
    demoDocument();
    demoColumnFamily();
    demoGraph();

    banner("老师傅的话");
    std::cout <<
        " 1. 四大模型没有王者，只有「你的查询长什么样」：\n"
        "      点查/计数 → KV；嵌套对象整取整存 → 文档；\n"
        "      写海量 + 按行键取几列 → 列族；多跳关系 → 图。\n"
        "    选型第一问永远是 top 查询，不是「哪个火」。\n\n"
        " 2. 注意它们的共同点：全都放弃了关系模型的两样东西——\n"
        "      ① 即席查询能力（WHERE 随意组合、JOIN 任意表）；\n"
        "      ② 昂贵的强一致事务（多数只保证单行/单文档原子性）。\n"
        "    用「放弃通用性」换「特定查询的极致」，这就是 NoSQL 的魂。\n\n"
        " 3. 你熟悉的 C++ 词汇完全够用：\n"
        "      KV = unordered_map；文档 = 嵌套 map + 序列化；\n"
        "      列族 = map<string, map<string, Cell>> 的稀疏结构；\n"
        "      图 = 邻接表 + BFS —— 数据结构课本原题。\n"
        "    NoSQL 不是新语言，是把课本里「按查询选结构」的老话认真执行了。\n\n"
        " 4. 真上生产再记三条坑：\n"
        "      Redis 是内存货，重启靠 RDB/AOF，别当唯一存储；\n"
        "      MongoDB 文档有 16MB 上限，「一本书嵌一万章节」会炸；\n"
        "      Cassandra 的写入快是因为它把「查询」推迟到了「读取」，\n"
        "      读放大、tombstone 这些账都记在下一个文件（LSM）里。\n\n"
        " 练习：\n"
        "  1. 给 KeyValueStore 加 expire（value 里带过期时间戳，get 时惰性删除），\n"
        "     就是 Redis 的 key 过期两大策略之一。\n"
        "  2. 给 DocumentStore 写一个「按 total 字段 > 20000 过滤所有文档」，\n"
        "     体会一下没有索引时 MongoDB 的 COLLSCAN 是什么感觉。\n"
        "  3. 把 GraphStore 换成有向图（follow 关系），friendOfFriend\n"
        "     怎么改？「互关」判定写一写。\n";
    return 0;
}
