/*
================================================================
  01_sqlite_c_api.cpp —— 第 4 关第一站：裸着手上 SQLite C API

  编译：  g++ -std=c++17 -O2 01_sqlite_c_api.cpp -o 01.exe -lsqlite3
  运行：  .\01.exe

  师傅的话：
    这一关咱们不聊 ORM，不聊框架，就把 SQLite 最原始的那套
    C 接口一行一行敲一遍。为什么？因为不管你以后用 MySQL
    Connector、ODBC、还是公司自研的 DAL，底下全是这几件事：

        打开连接 → 准备语句 → 绑定参数 → 逐行取数据 → 释放语句 → 关连接

    把这六步刻进肌肉记忆里，以后看任何数据库驱动的源码都不慌。

    本文件还有一个重头戏：先亲手用"字符串拼 SQL"制造一次
    SQL 注入，让你亲眼看见密码校验是怎么被绕过去的；
    然后立刻用预处理语句（bind 参数）修好它。
    这两段代码放在一起对比，比你说一百遍"要用参数化查询"都管用。
================================================================
*/

#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// 小工具：让 Windows 控制台正确显示 UTF-8 中文。
// 不搞这一步，源码里的中文在 PowerShell 里就是一堆乱码，
// 你会误以为程序跑错了。这是 Windows 上写 C++ 的日常坑。
// ---------------------------------------------------------------------------
static void setup_utf8_console()
{
#ifdef _WIN32
    SetConsoleOutputCP(65001); // 控制台输出代码页切到 UTF-8
#endif
}

// ---------------------------------------------------------------------------
// 小工具：检查 SQLite 返回码。
// C API 的规矩是"返回码 + errmsg"，不抛异常。
// 真实项目里你会把它包成异常（02 号文件就干这个），
// 但今天先手写，让你知道异常下面垫的是什么。
// ---------------------------------------------------------------------------
static void check(int rc, const char *what, sqlite3 *db)
{
    if (rc != SQLITE_OK)
    {
        // 出错了就打印错误并收工。生产代码里这里应该抛异常或者返回错误码，
        // 绝对不能"打印一下继续往下跑"——那样后面的逻辑全建立在坏状态上。
        std::printf("[致命] %s 失败，rc=%d，errmsg=%s\n", what, rc,
                    db ? sqlite3_errmsg(db) : "(无连接)");
        std::fflush(stdout);
        std::exit(1);
    }
}

static void hr(const char *title)
{
    std::printf("\n================ %s ================\n", title);
}

// ---------------------------------------------------------------------------
// 表结构：
//   users(id, username, password, role)
//   sessions(sid, username, note)     ← 专门用来演示"删表"式注入
// ---------------------------------------------------------------------------
static const char *DB_FILE = "capi_demo.db";

// 一个"登录查询"：拼字符串版本。
// 返回值：查到的行数；同时把拼出来的 SQL 打出来给你看个清楚。
static int login_by_concat(sqlite3 *db, const std::string &user, const std::string &pass)
{
    // ⚠️ 反面教材：把用户输入直接拼进 SQL。
    // 很多人想："我就拼个字符串，能出什么事？"
    // 出事就出在——用户的输入里没有"数据"和"代码"的界限。
    std::string sql = "SELECT id, username, role FROM users WHERE username='" + user +
                      "' AND password='" + pass + "';";
    std::printf("  实际发给数据库的语句：\n    %s\n", sql.c_str());

    struct Ctx
    {
        int rows = 0;
    } ctx;

    // sqlite3_exec 的回调式取结果。回调返回非 0 就提前结束。
    int rc = sqlite3_exec(
        db, sql.c_str(),
        [](void *p, int argc, char **argv, char **colv) -> int
        {
            auto *c = static_cast<Ctx *>(p);
            ++c->rows;
            std::printf("    命中一行 → ");
            for (int i = 0; i < argc; ++i)
                std::printf("%s=%s ", colv[i], argv[i] ? argv[i] : "NULL");
            std::printf("\n");
            return 0;
        },
        &ctx, nullptr);

    if (rc != SQLITE_OK)
    {
        std::printf("    查询报错：%s\n", sqlite3_errmsg(db));
        return -1;
    }
    return ctx.rows;
}

// 正常登录（预处理语句版本）。参数永远是参数，不会被当成 SQL 语法。
static int login_by_prepare(sqlite3 *db, const std::string &user, const std::string &pass)
{
    sqlite3_stmt *st = nullptr;
    const char *sql = "SELECT id, username, role FROM users WHERE username=? AND password=?;";
    check(sqlite3_prepare_v2(db, sql, -1, &st, nullptr), "prepare(login)", db);

    // SQLITE_TRANSIENT：让 SQLite 立刻自己拷一份字符串。
    // 这里必须用 TRANSIENT，因为 user.c_str() 是 std::string 的内部缓冲，
    // 出了作用域就没了；用 SQLITE_STATIC 等于把生命周期赌在别人身上。
    check(sqlite3_bind_text(st, 1, user.c_str(), -1, SQLITE_TRANSIENT), "bind(user)", db);
    check(sqlite3_bind_text(st, 2, pass.c_str(), -1, SQLITE_TRANSIENT), "bind(pass)", db);

    int rows = 0;
    while (true)
    {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW)
        {
            ++rows;
            std::printf("    命中一行 → id=%lld username=%s role=%s\n",
                        sqlite3_column_int64(st, 0),
                        reinterpret_cast<const char *>(sqlite3_column_text(st, 1)),
                        reinterpret_cast<const char *>(sqlite3_column_text(st, 2)));
            continue;
        }
        if (rc == SQLITE_DONE)
            break;
        check(rc, "step(login)", db);
    }
    sqlite3_finalize(st); // 一定要 finalize，否则语句和内存泄漏
    return rows;
}

int main()
{
    setup_utf8_console();

    hr("01 · SQLite 裸 C API 走一遍");
    std::printf("SQLite 版本：%s\n", sqlite3_libversion());

    // 每次跑都从干净文件开始，免得上一轮的残留影响这一轮的判断。
    std::remove(DB_FILE);
    std::remove("capi_demo.db-journal");
    std::remove("capi_demo.db-wal");
    std::remove("capi_demo.db-shm");

    // =========================================================================
    // 1. 打开连接
    // =========================================================================
    sqlite3 *db = nullptr;
    int rc = sqlite3_open(DB_FILE, &db);
    if (rc != SQLITE_OK)
    {
        // 注意：open 失败时 db 也可能是非空的，errmsg 还是得从它身上拿，
        // 然后再 close。这是 C API 的"好习惯"，别漏。
        std::printf("打开失败：%s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }
    std::printf("已打开数据库文件：%s\n", DB_FILE);

    // =========================================================================
    // 2. sqlite3_exec：建表 + 插几条种子数据
    //    exec 适合"不需要参数、不需要结果集"的语句（DDL、批量 DML）。
    // =========================================================================
    hr("sqlite3_exec：建表与种子数据");
    const char *setup =
        "CREATE TABLE users("
        "  id       INTEGER PRIMARY KEY,"  // INTEGER PRIMARY KEY 就是 rowid 别名
        "  username TEXT NOT NULL,"
        "  password TEXT NOT NULL,"
        "  role     TEXT NOT NULL"
        ");"
        "INSERT INTO users(username,password,role) VALUES('alice','a1b2c3','user');"
        "INSERT INTO users(username,password,role) VALUES('bob','hunter2','user');"
        "INSERT INTO users(username,password,role) VALUES('admin','S3cret!Admin','admin');"
        "CREATE TABLE sessions(sid INTEGER PRIMARY KEY, username TEXT, note TEXT);"
        "INSERT INTO sessions(username,note) VALUES('alice','早上登录');"
        "INSERT INTO sessions(username,note) VALUES('bob','加班');"
        "INSERT INTO sessions(username,note) VALUES('admin','半夜巡检');";

    char *errmsg = nullptr;
    rc = sqlite3_exec(db, setup, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK)
    {
        std::printf("建表失败：%s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return 1;
    }
    std::printf("建表 + 种子数据完成（users 3 行、sessions 3 行）\n");

    // =========================================================================
    // 3. 【反面教材】字符串拼接 → SQL 注入
    // =========================================================================
    hr("反面教材：拼字符串造成的 SQL 注入");

    std::printf("\n[场景 A] 正常用户登录\n");
    std::printf("  用户输入：username=alice  password=a1b2c3\n");
    int hit = login_by_concat(db, "alice", "a1b2c3");
    std::printf("  → 命中 %d 行（正常，登录成功）\n", hit);

    std::printf("\n[场景 B] 攻击者输入：username = admin' OR '1'='1\n");
    std::printf("  （密码随便填，比如 'x'）\n");
    hit = login_by_concat(db, "admin' OR '1'='1", "x");
    std::printf("  → 命中 %d 行。", hit);
    if (hit > 0)
        std::printf("看到了吗？密码根本没校验，直接以 admin 身份登进来了！\n");
    else
        std::printf("这次没命中（说明环境有变化，请检查 SQL 拼装）\n");

    std::printf("\n[场景 C] 更狠的：用 ; 串一条 DROP TABLE 进来\n");
    std::printf("  username = admin'; DROP TABLE sessions; --\n");
    {
        // exec 会执行分号隔开的多条语句，这就是"多语句执行"的危险面。
        // 有些驱动默认禁止多语句（比如 MySQL 的 CLIENT_MULTI_STATEMENTS 不开），
        // 但 SQLite 的 exec 就是照单全收。
        std::string evil = "SELECT sid, note FROM sessions WHERE username='admin'; DROP TABLE sessions; --' AND password='x';";
        std::printf("  实际发出去的语句：\n    %s\n", evil.c_str());
        rc = sqlite3_exec(
            db, evil.c_str(),
            [](void *, int argc, char **argv, char **) -> int
            {
                std::printf("    回显一行：");
                for (int i = 0; i < argc; ++i)
                    std::printf("%s ", argv[i] ? argv[i] : "NULL");
                std::printf("\n");
                return 0;
            },
            nullptr, &errmsg);
        if (rc != SQLITE_OK)
        {
            std::printf("    执行报错：%s\n", errmsg ? errmsg : "?");
            sqlite3_free(errmsg);
        }

        // 现在再看 sessions 还在不在。
        rc = sqlite3_exec(db, "SELECT COUNT(*) FROM sessions;", nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK)
        {
            std::printf("  → 表已经没了，查询报错：\"%s\"\n", errmsg ? errmsg : "?");
            std::printf("  → 这就是不参数化的代价：一条注入语句能把表干掉。\n");
            sqlite3_free(errmsg);
        }
        else
        {
            std::printf("  → sessions 还在（本次未触发删表）\n");
        }
    }

    // =========================================================================
    // 4. 【正确姿势】预处理语句 + 参数绑定
    // =========================================================================
    hr("正确姿势：sqlite3_prepare_v2 + bind + step + finalize");

    std::printf("\n[对照 A] 同样的攻击输入，交给参数化查询\n");
    std::printf("  用户输入：username = admin' OR '1'='1  password = x\n");
    hit = login_by_prepare(db, "admin' OR '1'='1", "x");
    std::printf("  → 命中 %d 行。", hit);
    std::printf(hit == 0 ? "参数被当成\"一个普通字符串\"去比对，注入失效。\n"
                         : "居然命中了？说明代码有问题，检查！\n");

    std::printf("\n[对照 B] 正确用户名密码，参数化查询照常工作\n");
    hit = login_by_prepare(db, "admin", "S3cret!Admin");
    std::printf("  → 命中 %d 行\n", hit);

    // =========================================================================
    // 5. 完整读数据：bind_* 全家桶 + column_* 全家桶
    // =========================================================================
    hr("预处理语句完整示范：带参数的范围查询");

    {
        // 用一张临时表演示各种类型的绑定与读取
        check(sqlite3_exec(db,
                           "CREATE TABLE metrics(id INTEGER PRIMARY KEY,"
                           " name TEXT, score REAL, cnt INTEGER, tag TEXT);",
                           nullptr, nullptr, &errmsg),
              "create metrics", db);
        sqlite3_free(errmsg);

        sqlite3_stmt *st = nullptr;
        check(sqlite3_prepare_v2(db,
                                 "INSERT INTO metrics(name,score,cnt,tag) VALUES(?,?,?,?);",
                                 -1, &st, nullptr),
              "prepare(insert)", db);

        struct Sample
        {
            std::string name;
            double score;
            long long cnt;
            const char *tag; // nullptr 表示 NULL
        };
        const std::vector<Sample> samples = {
            {"qps", 98.5, 1200, "perf"},
            {"latency_ms", 3.25, 87, "perf"},
            {"cache_hit", 0.912, 40210, "cache"},
            {"null_demo", 0.0, 0, nullptr},
        };

        // 这里故意不用事务（一条条自动提交），02 号文件会给你看事务带来的
        // 数量级差距。数据量小所以现在无所谓。
        for (const auto &s : samples)
        {
            sqlite3_reset(st);          // 复位，准备复用同一条语句
            sqlite3_clear_bindings(st); // 清掉上一次的绑定
            check(sqlite3_bind_text(st, 1, s.name.c_str(), -1, SQLITE_TRANSIENT), "bind name", db);
            check(sqlite3_bind_double(st, 2, s.score), "bind score", db);
            check(sqlite3_bind_int64(st, 3, s.cnt), "bind cnt", db);
            if (s.tag)
                check(sqlite3_bind_text(st, 4, s.tag, -1, SQLITE_TRANSIENT), "bind tag", db);
            else
                check(sqlite3_bind_null(st, 4), "bind null", db); // NULL 要显式绑

            int r = sqlite3_step(st);
            if (r != SQLITE_DONE)
                check(r, "step(insert)", db);
        }
        sqlite3_finalize(st);
        std::printf("插入 metrics %zu 行\n", samples.size());

        // 读回来。注意取文本要用 sqlite3_column_text 返回的 const unsigned char*。
        sqlite3_stmt *rd = nullptr;
        check(sqlite3_prepare_v2(db,
                                 "SELECT id, name, score, cnt, tag FROM metrics "
                                 "WHERE score >= ? ORDER BY id;",
                                 -1, &rd, nullptr),
              "prepare(select)", db);
        check(sqlite3_bind_double(rd, 1, 0.5), "bind threshold", db);

        std::printf("查询 score >= 0.5 的结果（共 %d 列）：\n", sqlite3_column_count(rd));
        std::printf("  %-4s %-12s %-10s %-8s %-8s\n", "id", "name", "score", "cnt", "tag");
        int rows = 0;
        while (true)
        {
            int r = sqlite3_step(rd);
            if (r == SQLITE_DONE)
                break;
            if (r != SQLITE_ROW)
                check(r, "step(select)", db);

            // sqlite3_column_text 返回 NULL 表示该列是 SQL NULL。
            // 直接 reinterpret_cast 然后 printf，就是对 NULL 解引用 → 崩。
            const unsigned char *name = sqlite3_column_text(rd, 1);
            const unsigned char *tag = sqlite3_column_text(rd, 4);
            std::printf("  %-4d %-12s %-10.3f %-8lld %-8s\n",
                        sqlite3_column_int(rd, 0),
                        name ? reinterpret_cast<const char *>(name) : "(NULL)",
                        sqlite3_column_double(rd, 2),
                        sqlite3_column_int64(rd, 3),
                        tag ? reinterpret_cast<const char *>(tag) : "NULL");
            ++rows;
        }
        sqlite3_finalize(rd);
        std::printf("共取回 %d 行\n", rows);
    }

    // =========================================================================
    // 6. 错误处理：errmsg 什么时候有内容
    // =========================================================================
    hr("错误处理：prepare 失败与 errmsg");
    {
        sqlite3_stmt *bad = nullptr;
        rc = sqlite3_prepare_v2(db, "SELEKT * FROM users", -1, &bad, nullptr);
        std::printf("故意写错的 SQL → rc=%d，errmsg=\"%s\"\n", rc, sqlite3_errmsg(db));
        // 记住：errmsg 指向的字符串下一次调用同一连接的 API 时就可能失效，
        // 要留就立刻拷走（std::string），别存 const char*。
        if (bad)
            sqlite3_finalize(bad);
    }

    // =========================================================================
    // 7. 收尾
    // =========================================================================
    hr("收尾");
    sqlite3_close(db); // 所有 stmt 都 finalize 之后再 close
    std::printf("连接已关闭。清理临时文件：");
    std::printf("capi_demo.db -> %d\n", std::remove(DB_FILE));

    hr("本文件要点回顾");
    std::printf("1. open/exec/prepare/bind/step/column/finalize/close 这条流水线要背下来。\n");
    std::printf("2. sqlite3_finalize 不是可选项：不 finalize 就泄漏，close 还会返回 SQLITE_BUSY。\n");
    std::printf("3. 用户输入永远走 bind，永远不要拼字符串。这是铁律，没有例外。\n");
    std::printf("4. bind_text 拿不准就用 SQLITE_TRANSIENT；用 SQLITE_STATIC 必须保证指针活到 step 结束。\n");
    std::printf("5. column_text 可能返回 NULL；column_count/column_type 是防御性编程的好帮手。\n");
    return 0;
}
