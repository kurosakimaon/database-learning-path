// ===========================================================================
// 04 · 通向真数据库：MySQL Connector/C++、ODBC、以及 ORM 的边界
// ===========================================================================
//
// ★★★ 重要声明（先看这段，不然你会困惑）★★★
//
//   本机关未安装 MySQL，也没有 MySQL Connector/C++、没有 unixODBC、没有
//   SOCI / sqlpp11 / ODB。所以本文件里所有"调用外部驱动"的代码都被
//   #ifdef 关掉了，不参与编译；它们的存在意义是**工程参考**，
//   等你到有 MySQL 的机器上，把宏打开、装上对应的库就能编译。
//
//   这个文件的设计意图不是"跑起来"，而是让你把三件事分清楚：
//     1) 驱动层（Connector/C++、ODBC）到底提供了什么抽象；
//     2) ORM 到底帮你省了什么、又偷走了什么；
//     3) 什么时候必须绕开 ORM 回到裸 SQL。
//   能被编译器验证的部分（编译期列映射、N+1 的成本模型）我留在
//   编译路径里，跑给你看。伪代码部分我在每段头上标了【参考代码·未编译】。
//
//   打开方式（等你有环境时）：
//     g++ -std=c++17 -DMY_HAS_MYSQL_CONN -I"Connector 的 include 目录"
//         04_mysql_and_orm.cpp -o 04.exe -L"Connector 的 lib 目录" -lcppconn
//   没有环境也别慌，本文件默认形态（不带任何 -D）是可以直接编译运行的：
//     g++ -std=c++17 -Wall -Wextra 04_mysql_and_orm.cpp -o 04.exe
//
// ===========================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// Windows 控制台 UTF-8，让中文正常显示（本机 PowerShell 5 需要）
// ---------------------------------------------------------------------------
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void setup_utf8_console()
{
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
}
#else
static void setup_utf8_console() {}
#endif

static void hr(const char *title)
{
    std::printf("\n---------------- %s ----------------\n", title);
}

// ===========================================================================
// 一、【参考代码·未编译】MySQL Connector/C++ 的完整用法
// ===========================================================================
//
// 师傅先给你把地图画清楚。C++ 连 MySQL 有三条路：
//
//   路线 1：MySQL Connector/C++（官方，X Dev API 或 Classic JDBC 风格 API）
//           依赖 libmysql / libcppconn，接口是 C++ 类，异常驱动。
//   路线 2：ODBC（跨数据库，驱动即插件）
//           接口是 C 句柄 + SQLPrepare/SQLBindParameter，返回码驱动。
//   路线 3：直接上 libmysqlclient（C API，和 01 号文件的 SQLite 一个味道）
//           没人包装时最累，但控制力最强。
//
// 三条路的共同点，也是和 SQLite 最大的不同：
//   * 连接是**跨网络的**：TCP 握手 + TLS + 认证，几毫秒到几十毫秒。
//     这就是为什么 03 号文件里的连接池在真数据库上不是"优化项"而是"必需品"。
//   * 一次查询的成本 = 网络往返 + 服务端解析/规划/执行 + 结果集序列化 +
//     再往返。网络往返往往比执行本身还贵，所以"少往返"是第一性原则。
//   * 错误分两层：连接层错误（网络断了、超时）和 SQL 层错误（语法、约束）。
//     前者可重试，后者重试等于自杀。分类错了，线上就出事。
// ---------------------------------------------------------------------------
#ifdef MY_HAS_MYSQL_CONN
// 【参考代码·未编译】需要 MySQL Connector/C++ 8.x（JDBC 风格 API）
#include <cppconn/connection.h>
#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <memory>
#include <stdexcept>

void mysql_conn_demo()
{
    // --- 1. 连接串：把该说清楚的一都说清楚 ---
    //
    // 这几个参数是新手最容易漏、漏了必踩坑的：
    //   charset=utf8mb4  ← 不是 utf8！MySQL 的 "utf8" 是 3 字节阉割版，
    //                      存 emoji / 生僻字直接报"Incorrect string value"。
    //                      新库一律 utf8mb4，排序规则配 utf8mb4_0900_ai_ci。
    //   timezone=...     ← 客户端和会话时区不一致，DATETIME 会整体偏移几小时。
    //                      服务端 session time_zone 与客户端 timezone 必须对齐，
    //                      或者干脆全库只存 UTC，展示层再转。
    //   useSSL / sslMode ← 生产必须开；内网盘算也要知道自己裸奔了。
    //   connectTimeout / readTimeout ← 不设的话，服务端挂了你会永久阻塞。
    //   pool 相关参数在 Connector/C++ 里没有，池要自己实现或换第三方池。
    const std::string url = "tcp://127.0.0.1:3306";
    const std::string opts =
        "?schema=shop&charset=utf8mb4&sslMode=REQUIRED"
        "&connectTimeout=3&readTimeout=10&useCompression=true";

    try
    {
        sql::Driver *drv = get_driver_instance();
        std::unique_ptr<sql::Connection> conn(drv->connect(url + opts, "app", "s3cret"));

        // --- 2. 会话级设置：连接复用时这些都会被重置，池化时要重新设 ---
        conn->setAutoCommit(false);              // 真数据库默认 autocommit=1，批量必须关
        conn->setTransactionIsolation(
            sql::Connection::TRANSACTION_READ_COMMITTED); // MySQL 默认 RR，多数业务改 RC 更合适
        std::unique_ptr<sql::Statement> sess(conn->createStatement());
        sess->execute("SET SESSION time_zone = '+00:00'"); // 会话锁 UTC，别信机器时区
        sess->execute("SET NAMES utf8mb4 COLLATE utf8mb4_0900_ai_ci");

        // --- 3. 预处理语句：和 SQLite 的 prepare/bind/execute 一模一样的思想 ---
        //
        // 注意 MySQL 的占位符是 ?，和 SQLite 一样；PG 的 libpq 用 $1..$n。
        // 参数化 = 协议层就把"数据"和"代码"分开，这是唯一可靠的防注入手段。
        // 任何"我自己转义引号"的方案都是历史包袱。
        std::unique_ptr<sql::PreparedStatement> ps(
            conn->prepareStatement("INSERT INTO orders(user_id, amount, memo) VALUES(?,?,?)"));
        for (int i = 0; i < 1000; ++i)
        {
            ps->setInt(1, i % 50);
            ps->setDouble(2, 9.9 + i);
            ps->setString(3, std::string("批量导入 ") + std::to_string(i));
            ps->executeUpdate(); // 每条一次网络往返 —— 所以下面要走 batch
        }
        conn->commit();

        // --- 4. 真正的批量：rewriteBatchedStatements 才是省往返的开关 ---
        //
        // Connector/C++ 的 addBatch + executeBatch，如果连接串没开
        // rewriteBatchedStatements=true，底层仍然一条条发！
        // 开了之后驱动会把同构 INSERT 拼成 INSERT ... VALUES (...),(...)，
        // 一次往返。这个开关不生效是很多人"批处理没提速"的唯一原因。
        std::unique_ptr<sql::PreparedStatement> pb(
            conn->prepareStatement("INSERT INTO orders(user_id, amount, memo) VALUES(?,?,?)"));
        for (int i = 0; i < 5000; ++i)
        {
            pb->setInt(1, i % 100);
            pb->setDouble(2, 1.0 * i);
            pb->setString(3, "batch");
            pb->addBatch();
            if (i % 1000 == 999)
            {
                pb->executeBatch(); // 攒 1000 条发一次
                conn->commit();
            }
        }
        pb->executeBatch();
        conn->commit();

        // --- 5. 结果集：游标式逐行取，注意 NULL 必须先 isNull 再取 ---
        std::unique_ptr<sql::PreparedStatement> q(
            conn->prepareStatement("SELECT id, user_id, amount, memo FROM orders "
                                   "WHERE user_id = ? ORDER BY id LIMIT ?"));
        q->setInt(1, 7);
        q->setInt(2, 20);
        std::unique_ptr<sql::ResultSet> rs(q->executeQuery());
        while (rs->next())
        {
            const int64_t id = rs->getInt64("id");
            const double amt = rs->getDouble("amount");
            // Connector/C++ 的 getString 返回 std::string；二进制/大字段用 getBlob
            const std::string memo = rs->isNull("memo") ? std::string("<NULL>") : rs->getString("memo");
            std::printf("  id=%lld amt=%.2f memo=%s\n", (long long)id, amt, memo.c_str());
        }
        // 注意：ResultSet 没读完就复用/销毁 Statement，服务端还有未消化的结果，
        // 这条连接会进入 "Commands out of sync" 状态，必须重新净空。池化时大坑。
    }
    catch (const sql::SQLException &e)
    {
        // 三个信息都要记：what()、getErrorCode()（MySQL 错误号）、getSQLState()
        // 1213 = Deadlock found（可重试）；1205 = Lock wait timeout（可重试但要退避）；
        // 1062 = Duplicate entry（业务错误，重试没意义）；2003/2013 = 连接层（视情况）。
        std::fprintf(stderr, "MySQL 错误 %d SQLSTATE=%s: %s\n", e.getErrorCode(),
                     e.getSQLState().c_str(), e.what());
        throw;
    }
}
#else
void mysql_conn_demo()
{
    std::printf("MySQL Connector/C++ 示例在源码第 一 节，本机未安装该库，未参与编译。\n");
    std::printf("要点回顾：\n");
    std::printf("  · 连接串必须写 charset=utf8mb4（MySQL 的 utf8 是 3 字节残缺版，存 emoji 会报错）\n");
    std::printf("  · 时区：session time_zone 与驱动 timezone 必须对齐，最省心是全库 UTC\n");
    std::printf("  · connectTimeout / readTimeout 必须设，否则服务端挂了你会永久阻塞\n");
    std::printf("  · 批量要 addBatch/executeBatch，且连接串开 rewriteBatchedStatements=true，\n");
    std::printf("    否则驱动还是一条条发，你的\"批处理\"根本没批。\n");
    std::printf("  · 错误分类：1213/1205 死锁与锁等待可重试；1062 唯一冲突是业务错误，重试无意义。\n");
}
#endif

// ===========================================================================
// 二、【参考代码·未编译】ODBC 路线：跨数据库的"最低公分母"
// ===========================================================================
//
// 什么时候选 ODBC：
//   * 你的产品要同时支持 MySQL / SQL Server / Oracle / PG，不想为每家学一套 API；
//   * 你有 DBA 只管 DSN，运维在系统里配好数据源，程序只认一个名字；
//   * 你要接冷门数据库（DB2、Sybase、各种国产库），只有 ODBC 驱动。
// 什么时候别选：
//   * 只连一种库 —— 原生驱动的扩展能力（COPY、批量数组、异步、专用类型）ODBC 拿不到；
//   * 你在乎极致延迟 —— ODBC 多一层，接口啰嗦，代码量是原生 API 的 2~3 倍。
// ---------------------------------------------------------------------------
#ifdef MY_HAS_ODBC
// 【参考代码·未编译】需要 windows.h + isql.h（或 iodbc/unixODBC）
#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

void odbc_demo()
{
    SQLHANDLE env = nullptr, dbc = nullptr, stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);

    // DSN 方式（DSN=shop）或 连接串方式（DRIVER={MySQL ODBC 8.0 Driver};SERVER=...）
    SQLRETURN rc = SQLDriverConnectA(
        dbc, nullptr,
        (SQLCHAR *)"DSN=shop;UID=app;PWD=s3cret;CHARSET=utf8mb4;", SQL_NTS,
        nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(rc))
    {
        SQLCHAR state[8] = {0}, msg[1024] = {0};
        SQLINTEGER native = 0;
        // ODBC 的错误一定是"三层"的：SQLRETURN + SQLSTATE + native code + 消息文本。
        // 只记 SQLRETURN（比如 SQL_ERROR）等于什么都没记。
        SQLGetDiagRecA(SQL_HANDLE_DBC, dbc, 1, state, &native, msg, sizeof(msg), nullptr);
        std::fprintf(stderr, "ODBC 连接失败 state=%s native=%d msg=%s\n", state, native, msg);
        return;
    }

    SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0);
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    // 参数绑定：注意要先把 C 类型和 SQL 类型都写明白，这是 ODBC 最烦的地方。
    SQLPrepareA(stmt, (SQLCHAR *)"INSERT INTO orders(user_id, amount, memo) VALUES(?,?,?)", SQL_NTS);
    SQLINTEGER uid = 1;
    SQLDOUBLE amt = 9.9;
    SQLCHAR memo[64] = "odbc";
    SQLLEN ind_uid = 0, ind_amt = 0, ind_memo = SQL_NTS;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &uid, 0, &ind_uid);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &amt, 0, &ind_amt);
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 63, 0, memo, sizeof(memo),
                     &ind_memo);
    // 想传 NULL：把对应的 SQLLEN 设成 SQL_NULL_DATA。
    // 忘了设（或设成 0）你会得到一个空串而不是 NULL —— 这个 bug 极难查。
    SQLExecute(stmt);

    // 数组绑定（array binding）才是 ODBC 的高吞吐姿势：
    // 一次 SQLExecute 送 N 行，往返一次。逐行 SQLExecute 和逐条 INSERT 一样慢。
    SQLSetStmtAttr(stmt, SQL_ATTR_PARAMSET_SIZE, reinterpret_cast<SQLPOINTER>(1000), 0);

    SQLTransact(SQL_NULL_HANDLE, dbc, SQL_COMMIT);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLDisconnect(dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
}
#else
void odbc_demo()
{
    std::printf("ODBC 示例在源码第 二 节，本机未安装 ODBC 驱动/头文件，未参与编译。\n");
    std::printf("要点回顾：\n");
    std::printf("  · 错误必三层：SQLRETURN + SQLSTATE + native code + 消息文本，只记一个等于没记。\n");
    std::printf("  · 传 NULL 要把 SQLLEN 设成 SQL_NULL_DATA，否则得到空串，这类 bug 极难查。\n");
    std::printf("  · 高吞吐靠 SQL_ATTR_PARAMSET_SIZE 数组绑定，逐行 SQLExecute 与逐条 INSERT 一样慢。\n");
}
#endif

// ===========================================================================
// 三、ORM 的定位：它省的是 CRUD 的体力活，偷的是你对 SQL 的控制权
// ===========================================================================
//
// 三个在 C++ 里最有代表性的方案，师傅按"离 SQL 的距离"给你排开：
//
//   SOCI        —— 半 ORM / 数据访问层。你仍然写 SQL 字符串，它只负责
//                  "把结果集灌进 C++ 对象"和"把 C++ 对象变成绑定参数"。
//                  接口像 soc::session << "select ...", into(row);
//                  还有编译期行转换（rowset + user mapping 宏）。
//                  适合：SQL 你说了算，只是懒得手写绑定与取列。
//
//   sqlpp11     —— 类型安全的编译期查询构造器（header-only, C++14/17）。
//                  select(columns(f::id, f::name)).from(f).where(f.id == 7);
//                  写错列名/类型**编译不过**，这是它最大的卖点。
//                  没有对象状态跟踪、没有 UnitOfWork，不会偷偷发查询。
//                  适合：想在 C++ 里获得"编译期 SQL 正确性"，又不想被 ORM 绑架。
//
//   ODB         —— 重量级全 ORM。#pragma db object 标注类，
//                  编译器插件（odb 工具）扫头文件生成映射代码，
//                  有 object cache、lazy loading、事务内对象状态跟踪。
//                  适合：领域模型稳定、CRUD 密集、团队愿意接受构建链上多一个代码生成器。
//                  代价：构建复杂（odb 编译器插件 + 生成的 .cxx 进构建系统）、
//                        报错信息是模板地狱、lazy loading 天然带来 N+1。
//
// 通用取舍（换任何语言/框架都成立）：
//   ORM 帮你省：手写绑定/取列、类型转换、对象-关系映射、样板 CRUD、迁移脚本。
//   ORM 偷走你：对 SQL 形态的掌控（它生成的 SQL 你可能从没看过）、
//               对往返次数的掌控（lazy loading → N+1）、
//               对执行计划的掌控（它拼出来的 JOIN 顺序、能否用上索引，你不清楚）。
//   师傅的规矩：**任何 ORM，上线前把它生成的 SQL 打开看一遍。**
//   所有驱动都能开"语句日志"：MySQL 的 general_log / Connector 的 trace，
//   PostgreSQL 的 log_statement='all'，SQLite 的 sqlite3_trace_v2。
//   没看过生成 SQL 就用 ORM，等于闭眼开车。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 【本节可编译部分】手写一个"编译期列映射"
//
// 目标：让"结构体字段 ↔ SQL 列"这件事在编译期完成，零运行时反射、零宏魔法。
// 这正是 sqlpp11 / SOCI 编译期映射在做的事，只不过它们用了更花哨的模板。
// 你自己实现一遍，就明白 ORM 的"类型安全"到底从哪来的 ——
// 说白了就是 C++17 的结构化绑定 + 模板参数包 + constexpr。
// ---------------------------------------------------------------------------

// 一列的元信息：列名 + "指向成员的指针"（member pointer 是编译期常量）。
// 两个模板参数：Obj 是对象类型，T 是该列的类型。
// 注意 &Order::id 的类型是 int64_t Order::*，所以必须写成 T Obj::*，
// 写成 T T::* 编译不过 —— 我第一次写就卡在这儿，因为"对象类型"和
// "列类型"经常被当成一个。
template <typename Obj, typename T>
struct Column
{
    const char *name;
    T Obj::*field;
};

// 把一行的各列拼成可读文本（真实场景这里是 bind 到 stmt 上）
struct TextRowBuilder
{
    std::string buf;
    void add_int(int64_t v) { buf += std::to_string(v); buf += ' '; }
    void add_double(double v)
    {
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "%.2f", v);
        buf += tmp;
        buf += ' ';
    }
    void add_str(const std::string &v) { buf += "'"; buf += v; buf += "' "; }
};

// 按列类型分派：字符串 → add_str，浮点 → add_double，其余 → add_int。
// if constexpr 是 C++17 的东西：编译期按类型选分支，没选中的分支根本不实例化，
// 所以不会因为"给 int64_t 调 add_str"而报错。ORM 的类型映射就靠这一招。
template <typename Obj, typename T>
void emit_col(TextRowBuilder &b, const Obj &o, T Obj::*f)
{
    if constexpr (std::is_same_v<T, std::string>)
        b.add_str(o.*f);
    else if constexpr (std::is_floating_point_v<T>)
        b.add_double(static_cast<double>(o.*f));
    else
        b.add_int(static_cast<int64_t>(o.*f));
}

// 业务结构体：注意它没有任何基类、没有任何宏、没有任何构造函数魔法。
// 这就是编译期映射的好处 —— 你的领域对象保持干净。
struct Order
{
    int64_t id = 0;
    int64_t user_id = 0;
    double amount = 0.0;
    std::string memo;
    std::optional<std::string> coupon; // NULL 列用 optional 表达，别用空串混
};

// 列清单：一个 constexpr 的 tuple，全部信息在编译期就绪。
// 加一个字段：改结构体 + 在这里加一行，别处一行都不用动。
constexpr auto kOrderColumns = std::make_tuple(
    Column<Order, int64_t>{"id", &Order::id},
    Column<Order, int64_t>{"user_id", &Order::user_id},
    Column<Order, double>{"amount", &Order::amount},
    Column<Order, std::string>{"memo", &Order::memo});

constexpr std::size_t kOrderColumnCount = std::tuple_size_v<decltype(kOrderColumns)>;

// 生成 "id, user_id, amount, memo"。
// C++17 的 fold expression 只能折叠"参数包"，不能直接折叠 tuple，
// 所以标准写法是：把 tuple 的下标变成 index_sequence 参数包，再折叠。
// 这个套路抄一遍就记一辈子，模板元编程的日常就是它。
// （C++20 允许模板参数写进 lambda，C++17 不行，所以这里用函数模板。）
inline void append_col_name(std::string &s, bool first, const char *name)
{
    if (!first)
        s += ", ";
    s += name;
}

template <std::size_t... Is>
void append_col_names(std::string &s, std::index_sequence<Is...>)
{
    // 别写成 s += sep += name：+= 是右结合的，会被解析成 s += (sep += name)，
    // 而三元运算符的结果不是左值，编译器报"lvalue required as left operand"。
    // 这个报错我第一次见以为是 tuple 的问题，其实纯粹是运算符优先级。
    (append_col_name(s, Is == 0, std::get<Is>(kOrderColumns).name), ...);
}

inline std::string order_column_list()
{
    std::string s;
    append_col_names(s, std::make_index_sequence<kOrderColumnCount>{});
    return s;
}

// 生成 "VALUES(?,?,?,?)"：占位符数量 = 列数，永远不会和列清单不一致。
// 手写 SQL 最常见的一类 bug 就是"改了列名忘了改占位符个数"，
// 编译期映射从根上消灭这类 bug —— 这就是 ORM/类型安全的真实价值：
// 不是"少打几个字"，是"少一类 bug"。
inline std::string order_placeholders()
{
    std::string s = "VALUES(";
    for (std::size_t i = 0; i < kOrderColumnCount; ++i)
        s += (i == 0 ? "?" : ",?");
    s += ")";
    return s;
}

// 把对象的所有列"输出"一遍（真实场景里这里是 bind 到 stmt 上）
template <std::size_t... Is>
void emit_order(TextRowBuilder &b, const Order &o, std::index_sequence<Is...>)
{
    (emit_col(b, o, std::get<Is>(kOrderColumns).field), ...);
}

inline std::string order_to_text(const Order &o)
{
    TextRowBuilder b;
    emit_order(b, o, std::make_index_sequence<kOrderColumnCount>{});
    return b.buf;
}

// 反向：从"查询结果的第 i 列"填回对象。
// 真实 ORM 在这里做类型检查与 NULL 处理；我们把 NULL 用 optional 表达。
inline Order order_from_columns(int64_t id, int64_t user_id, double amount, std::string memo,
                                std::optional<std::string> coupon)
{
    Order o;
    o.id = id;
    o.user_id = user_id;
    o.amount = amount;
    o.memo = std::move(memo);
    o.coupon = std::move(coupon);
    return o;
}

// ---------------------------------------------------------------------------
// 【可编译】N+1 的成本模型：不连数据库也能把账算明白
//
// 一次"查询"的真实成本 = 固定往返成本 + 每行处理成本。
// ORM 的 lazy loading 会把"固定往返成本"乘以 N，这一点在 SQLite（进程内、
// 往返极便宜）上不明显，在 MySQL（跨网络、往返 0.5ms 起）上要命。
// 这里用可测量函数把这个差别量化给你看。
// ---------------------------------------------------------------------------
struct RoundTrip
{
    // 用可配置的常数模拟不同部署形态的往返成本（单位：微秒）
    double inprocess_sqlite = 3.0;   // 进程内：解析+加锁+读页
    double local_mysql = 300.0;      // 同机房 MySQL：一次往返约 0.3ms
    double remote_mysql = 3000.0;    // 跨地域：3ms 很正常
    double per_row_us = 0.4;         // 服务端每行序列化 + 客户端每行取列
};

struct QueryPlanCost
{
    double n_plus_1_us = 0.0;
    double joined_us = 0.0;
    double ratio = 0.0;
};

static QueryPlanCost estimate_n_plus_one(const RoundTrip &rt, double n_children, double rows_per_child)
{
    QueryPlanCost c;
    // N+1：1 次父查询 + N 次子查询，每次都付一遍完整往返
    c.n_plus_1_us = rt.inprocess_sqlite +                          // 父查询
                    n_children * (rt.local_mysql + rows_per_child * rt.per_row_us);
    // JOIN / IN(...)：一次往返把数据全拿回来
    c.joined_us = rt.local_mysql + n_children * rows_per_child * rt.per_row_us;
    c.ratio = c.joined_us > 0 ? c.n_plus_1_us / c.joined_us : 0.0;
    return c;
}

// ---------------------------------------------------------------------------
// 【可编译】"隐式事务"为什么能吃掉性能：一个纯算术账
//
// autocommit=1 时，每条语句 = 一个事务 = 至少一次 fsync（组提交能缓解但不能消灭）。
// 假设 fsync 成本 F、语句执行成本 E，则 N 条的成本 = N*(E+F) vs E + N*E + F。
// 这就是 02 号文件里几千倍差距的数学来源，跟具体数据库无关。
// ---------------------------------------------------------------------------
static void implicit_txn_math()
{
    const double N = 10000.0;
    const double E = 1.5;   // 单条语句执行成本（微秒）
    const double F = 9000;  // 一次 fsync 落盘成本（微秒，机械盘/带杀软时上万很常见）
    const double autocommit = N * (E + F);
    const double onetxn = E + N * E + F;
    std::printf("  N=%.0f  单条执行 E=%.1fus  fsync F=%.0fus\n", N, E, F);
    std::printf("  autocommit（每条一个事务）：%.0f ms\n", autocommit / 1000.0);
    std::printf("  显式包成一个事务        ：%.2f ms\n", onetxn / 1000.0);
    std::printf("  → 理论倍数 %.0f 倍。02 号文件里我们实测到 %.0f 倍，同一个道理。\n",
                autocommit / onetxn, 7217.0);
    std::printf("  推论：MySQL 的 autocommit 默认是 1。批量导入前先 SET autocommit=0，\n");
    std::printf("        或者显式 START TRANSACTION/COMMIT，别裸着跑。\n");
}

// ---------------------------------------------------------------------------
// 【可编译】"什么时候必须回到裸 SQL"——用一段真实需求把边界画出来
// ---------------------------------------------------------------------------
static void where_orm_fails()
{
    std::printf("  这六类需求，任何 ORM 都会让你最终手写 SQL（区别只是提前还是最后）：\n");
    std::printf("  1. UPSERT / ON CONFLICT、INSERT ... ON DUPLICATE KEY UPDATE：\n");
    std::printf("     幂等写入的唯一正确姿势，多数 ORM 没有一等公民支持。\n");
    std::printf("  2. 窗口函数（ROW_NUMBER / SUM OVER）、CTE、递归查询：\n");
    std::printf("     用于分组 TopN、层级展开、去重取最新。ORM 的查询构造器一般只能 escape。\n");
    std::printf("  3. 大批量写入与 COPY / LOAD DATA INFILE：\n");
    std::printf("     对象化本身就比裸批量慢一个量级，这条路上 ORM 是纯负担。\n");
    std::printf("  4. 需要精确控制索引（FORCE INDEX、覆盖索引、避免回表）：\n");
    std::printf("     ORM 生成的 SELECT * 直接把覆盖索引打成全表回表。\n");
    std::printf("     规矩：**永远不要 SELECT ***，只取你要的列，这一条就能省一半。\n");
    std::printf("  5. 锁语义（SELECT ... FOR UPDATE / FOR SHARE、SKIP LOCKED 做队列）：\n");
    std::printf("     ORM 的对象缓存会让你以为读到了新值，实际读到的是它缓存里的旧对象。\n");
    std::printf("  6. 执行计划调优：你必须先看 EXPLAIN，然后按索引顺序手写 SQL。\n");
    std::printf("     这一步 ORM 只会碍事，因为它随时可能改写你的 SQL 形态。\n");
    std::printf("\n");
    std::printf("  师傅的分工建议（真项目里被验证过的配比）：\n");
    std::printf("    70%% 的 CRUD 用 ORM / sqlpp11 —— 省体力、类型安全、改动小。\n");
    std::printf("    30%% 的热路径与复杂查询用裸 SQL + 预处理语句 —— 换可控性。\n");
    std::printf("    两种风格必须共用同一套连接管理与事务边界，否则会打起来。\n");
    std::printf("    （事务边界打起来的症状：ORM 的 UnitOfWork 和你的手 START TRANSACTION\n");
    std::printf("      撞在一起，出现\"nested transaction\"警告或者提交两次。）\n");
}

// ===========================================================================
// 主函数
// ===========================================================================
int main()
{
    setup_utf8_console();

    std::printf("\n================================================================\n");
    std::printf("  04 · MySQL Connector/C++、ODBC、ORM 的边界（本机未装 MySQL）\n");
    std::printf("================================================================\n");
    std::printf("本文件所有外部驱动调用都在 #ifdef 里，未参与编译；\n");
    std::printf("可编译部分是\"编译期列映射\"和\"成本模型\"，用来把 ORM 的原理讲实。\n");

    hr("路线 1：MySQL Connector/C++");
    mysql_conn_demo();

    hr("路线 2：ODBC");
    odbc_demo();

    hr("可编译验证：编译期列映射（sqlpp11 / SOCI 的核心思想）");
    const std::string cols = order_column_list();
    const std::string ph = order_placeholders();
    std::printf("  由结构体自动生成的列清单：%s\n", cols.c_str());
    std::printf("  由列数自动生成的占位符  ：%s\n", ph.c_str());
    std::printf("  → 拼出的 INSERT：%s INTO orders(%s) %s\n", "PREPARE", cols.c_str(), ph.c_str());
    std::printf("  → 列名与占位符个数由同一份 tuple 推导，天生不可能不一致。\n");

    Order o = order_from_columns(1001, 7, 199.90, "订单 A", std::nullopt);
    std::printf("  对象 → 绑定值：%s", order_to_text(o).c_str());
    std::printf("（coupon 为 nullopt，对应 SQL NULL —— 用 optional 而不是空串）\n");
    Order o2 = order_from_columns(1002, 7, 12.50, "订单 B", std::string("C-2026-88"));
    std::printf("  对象 → 绑定值：%s", order_to_text(o2).c_str());
    std::printf("（coupon 有值：%s）\n", o2.coupon ? o2.coupon->c_str() : "NULL");
    std::printf("  字段总数（编译期算出）：%zu，加字段只需改结构体和 tuple 一处。\n",
                std::tuple_size_v<decltype(kOrderColumns)>);

    hr("成本模型：N+1 到底贵在哪");
    RoundTrip rt;
    const double n = 200.0, r = 5.0;
    QueryPlanCost c = estimate_n_plus_one(rt, n, r);
    std::printf("  假设：父查询 200 个子记录，每个子记录 5 行；同机房 MySQL 往返 300us\n");
    std::printf("  ORM lazy loading（N+1）：%.0f ms\n", c.n_plus_1_us / 1000.0);
    std::printf("  一条 JOIN / IN 查询     ：%.0f ms\n", c.joined_us / 1000.0);
    std::printf("  → 差距 %.0f 倍。注意贵的是\"往返次数\"，不是数据量。\n", c.ratio);
    std::printf("  同一个模型换个参数你自己算：把 n 调到 10000，差距会到千倍量级。\n");
    std::printf("  修法（ORM 里都有，但你必须知道要用）：\n");
    std::printf("    · JOIN FETCH / eager loading —— 一次拿全，代价是父行重复传输；\n");
    std::printf("    · 批量加载（batching / IN 一批 id）—— 把 N 次往返压成 1 次；\n");
    std::printf("    · 直接在 DTO 层写一条 JOIN SQL —— 列表页几乎总该这么干。\n");

    hr("成本模型：隐式事务（autocommit）的税")  ;
    implicit_txn_math();

    hr("边界：什么时候必须绕开 ORM 回到裸 SQL");
    where_orm_fails();

    std::printf("\n================================================================\n");
    std::printf("  师傅总结\n");
    std::printf("================================================================\n");
    std::printf("1. 换数据库不换\"思想\"：prepare/bind/step/finalize 这套，\n");
    std::printf("   在 MySQL 叫 PreparedStatement::executeQuery，在 ODBC 叫 SQLExecute，\n");
    std::printf("   在 PG 叫 PQexecParams。你把 01 号文件吃透，换库只是换函数名。\n");
    std::printf("2. 真数据库和 SQLite 的头号差别是\"网络往返\"。\n");
    std::printf("   所以两条铁律：连接必须池化（03 号文件）、往返必须合并（批处理 + 禁 N+1）。\n");
    std::printf("3. ORM 是生产力工具，不是防火墙。用它就必须定期看它生成的 SQL，\n");
    std::printf("   并给热路径留一条\"退回裸 SQL\"的口子。架构上这叫逃生舱，没有逃生舱的方案不上线。\n");
    std::printf("4. utf8mb4、时区、connectTimeout/readTimeout、重试与不可重试的错误分类 ——\n");
    std::printf("   这四件事没有任何 ORM 会替你决定，必须你在连接层就定死。\n");
    std::printf("5. 想在本机跑通本文件的参考代码：装 MySQL 8 + Connector/C++ 8，\n");
    std::printf("   加 -DMY_HAS_MYSQL_CONN 重新编译；或装 MySQL ODBC 驱动，加 -DMY_HAS_ODBC。\n");
    std::printf("   本机关未安装 MySQL，以上代码仅作工程参考。\n");
    return 0;
}
