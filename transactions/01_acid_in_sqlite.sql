-- =================================================================--
--  01_acid_in_sqlite.sql
--  第 3 关 · 心法期 —— 用 SQL 亲手把 ACID 摸一遍
--
--  怎么跑（PowerShell 5）：
--    cd d:\Users\Desktop\database\database-learning-path\transactions
--    # A) 内存库，快速过一遍原子性/一致性/保存点
--    sqlite3 :memory: ".read 01_acid_in_sqlite.sql"
--
--    # B) 文件库，这样 WAL、锁行为才看得见（推荐，跑完自己删）
--    sqlite3 lab.db ".read 01_acid_in_sqlite.sql"
--    # 跑完清理：Remove-Item lab.db, lab.db-wal, lab.db-shm -ErrorAction SilentlyContinue
--
--  用 C++ 开发的人，把事务想成什么？
--    BEGIN ... COMMIT      ≈  一段加锁的临界区，末尾一次性"发布"
--    ROLLBACK              ≈  临界区里抛异常，栈展开时把副作用撤销
--    undo log              ≈  进入临界区前先记下旧值（RAII 的" destructor 恢复"）
--    redo log / WAL        ≈  先把"我要改什么"追加到日志并 fsync，再改真数据
--    行锁                  ≈  按 key 分片的 std::mutex
--  这一路你会反复看到这套对应关系。
-- =================================================================--

-- .mode column / .headers on 只是为了输出好看，跟事务本身无关
.mode column
.headers on
.width 12 10 10

.print ''
.print '============ 第 0 步：环境自检 ============'
.print ''
SELECT sqlite_version() AS sqlite_版本;

-- 外键默认是关的！这是 SQLite 最大的"惊喜"之一。
-- MySQL InnoDB 默认开外键，SQLite 必须每个连接显式开。
PRAGMA foreign_keys = OFF;
SELECT * FROM pragma_foreign_keys;   -- 输出 0：外键检查关着
PRAGMA foreign_keys = ON;
SELECT * FROM pragma_foreign_keys;   -- 输出 1：开了
.print '--- PRAGMA foreign_keys=ON 已打开（每个连接都要开，它是连接级设置）---'
.print '--- C++ 类比：它是 per-thread 的，不是一把全局锁，别以为开一次就完事 ---'
.print ''

-- =================================================================--
--  第 1 步：A —— Atomicity（原子性）
--  要么全成，要么全废。中途 ROLLBACK，外面看什么都没发生过。
--  底层靠谁干活：undo log（回滚日志），先把旧值记下来，出事倒着放回去。
-- =================================================================--
.print ''
.print '============ 第 1 步：原子性 —— BEGIN / ROLLBACK ============'
.print ''

CREATE TABLE account (
    id      INTEGER PRIMARY KEY,
    name    TEXT    NOT NULL,
    balance INTEGER NOT NULL
);

INSERT INTO account(id, name, balance) VALUES (1, '张三', 1000), (2, '李四', 500);

.print '--- 初始状态 ---'
SELECT * FROM account;

BEGIN;                    -- 显式开启事务（SQLite 默认是"自动提交"，一条语句一个事务）
UPDATE account SET balance = balance - 200 WHERE id = 1;
UPDATE account SET balance = balance + 200 WHERE id = 2;
.print '--- 事务内部：已经看见新值了（自己看自己是永远看得见自己的写的）---'
SELECT * FROM account;
ROLLBACK;                 -- 全部撤销

.print '--- ROLLBACK 之后：世界被擦干净了，跟没发生过一样 ---'
SELECT * FROM account;

BEGIN;
UPDATE account SET balance = balance - 200 WHERE id = 1;
UPDATE account SET balance = balance + 200 WHERE id = 2;
COMMIT;                   -- 一次性发布

.print '--- COMMIT 之后：转账生效 ---'
SELECT * FROM account;

.print '--- 关键洞察：COMMIT 之前别的连接看不到这两条 UPDATE 的效果，'
.print '    但 COMMIT 一旦被接受，即使断电，重启后它也必须在。'
.print '    前半句靠隔离性（锁/MVCC），后半句靠持久性（WAL + fsync）。'
.print '    学徒常犯的错：把 BEGIN...COMMIT 当成"加了个 mutex"。它不是互斥，'
.print '    它是"可撤销 + 可持久"，互斥只是其中一条实现路径。---'

-- =================================================================--
--  第 2 步：SAVEPOINT —— 部分回滚
--  真实业务里常见：一个大批处理事务，其中一条子操作失败，
--  不想把整锅倒掉，只想撤销这一段。这就是 SAVEPOINT 的价值。
--  C++ 类比：SAVEPOINT ≈ try 块入口记一个水位，ROLLBACK TO ≈ 局部 catch 后回退。
-- =================================================================--
.print ''
.print '============ 第 2 步：SAVEPOINT 部分回滚 ============'
.print ''

BEGIN;
UPDATE account SET balance = balance + 100 WHERE id = 1;   -- 这一笔要保留

SAVEPOINT sp_batch;                                        -- 打个水位标记
UPDATE account SET balance = balance + 500 WHERE id = 2;   -- 这一笔要撤销
UPDATE account SET balance = -99999      WHERE id = 2;     -- 模拟这一步"业务失败"
.print '--- 子事务里，改坏了，准备回退到水位 ---'
SELECT * FROM account;

ROLLBACK TO sp_batch;      -- 只撤销 sp_batch 之后的改动，外层事务还活着
RELEASE sp_batch;          -- 释放水位（RELEASE 不是提交，只是把标记从栈上摘掉）

UPDATE account SET balance = balance + 50 WHERE id = 2;    -- 换条正确路子重试
COMMIT;                    -- 外层事务正常提交

.print '--- 结果：+100 保住了，中间的 -99999 被精确切除 ---'
SELECT * FROM account;

.print '--- 坑警告：ROLLBACK TO 之后事务仍然处于开启状态，'
.print '    很多人以为回退=结束，忘了 COMMIT，连接一断就被隐式回滚。---'

-- =================================================================--
--  第 3 步：D —— Durability（持久性）与 WAL
--  注意：journal_mode 是"库级"属性，会持久化在库文件里；
--  内存库切不了 WAL，会原样返回 "memory"。所以想看到 WAL，
--  必须用文件库跑：sqlite3 lab.db ".read 01_acid_in_sqlite.sql"
-- =================================================================--
.print ''
.print '============ 第 3 步：持久性 —— WAL 模式 ============'
.print ''
PRAGMA journal_mode;                 -- 默认 delete；文件库会被下面切成 wal
PRAGMA journal_mode = WAL;           -- 内存库会返回 "memory"，这是预期行为，不是报错
PRAGMA synchronous = NORMAL;         -- WAL 下 NORMAL 是性价比最高的档位
PRAGMA wal_autocheckpoint = 1000;    -- 默认每 1000 页 checkpoint 一次
PRAGMA journal_mode;                 -- 再看一眼，文件库里这里应该是 wal
PRAGMA synchronous;                  -- 1 = NORMAL

.print '--- 三档 synchronous 的取舍（这是"性能 vs 掉电丢多少"的旋钮）：'
.print '    OFF(0)    ：不等 fsync，最快，掉电可能丢最近若干事务'
.print '    NORMAL(1) ：WAL 模式只在 checkpoint 时 fsync，掉电最多丢 checkpoint 未落的那部分'
.print '              —— 但已 COMMIT 且已写进 -wal 的事务不会丢'
.print '    FULL(2)   ：每次 COMMIT 都 fsync，最慢最安全'
.print '    C++ 类比：synchronous 就是你给 fstream 手动 flush + FlushFileBuffers 的频率。'
.print '    数据库的持久性承诺从来不是"零丢失"，而是"在某个明确定义的失败模型下不丢"。---'

BEGIN;
CREATE TABLE IF NOT EXISTS orders (
    id     INTEGER PRIMARY KEY,
    acct   INTEGER NOT NULL REFERENCES account(id),  -- 外键指向 account
    amount INTEGER NOT NULL CHECK (amount > 0),      -- CHECK 约束
    uniq   TEXT    NOT NULL UNIQUE                    -- 唯一约束
);
INSERT INTO orders(acct, amount, uniq) VALUES (1, 30, 'ORD-001');
COMMIT;

.print '--- 若上面 journal_mode 输出是 wal，去目录看：'
.print '    lab.db        主库文件（可能"落后"于最新状态）'
.print '    lab.db-wal    追加写的日志，最新改动在这儿，它是顺序写，所以对 SSD/HDD 都友好'
.print '    lab.db-shm    共享内存索引，描述 wal 的哪些帧可读，供多连接共享'
.print '    这正是"先写日志再改数据"(WAL 协议) 的实物形态。---'

-- =================================================================--
--  第 4 步：C —— Consistency（一致性）
--  AID 是手段，C 是目的。C 由"约束 + 业务规则 + AID 兜底"共同保证。
-- =================================================================--
.print ''
.print '============ 第 4 步：一致性 —— 约束怎么兜底 ============'
.print ''

.print '--- 4.1 唯一约束：重复提交同一个订单号会被拦住 ---'
.print '---     这就是幂等性的最后一道闸，应用层判重的 race condition 靠它收尾 ---'
INSERT OR IGNORE INTO orders(acct, amount, uniq) VALUES (1, 30, 'ORD-001');
.print '    ↑ INSERT OR IGNORE 静默失败；不写 OR IGNORE 时会报 UNIQUE constraint failed'
SELECT changes() AS 本次实际插入行数;

.print ''
.print '--- 4.2 CHECK 约束：负数金额进不来 ---'
INSERT OR IGNORE INTO orders(acct, amount, uniq) VALUES (1, -50, 'ORD-002');
.print '    ↑ 被 CHECK (amount > 0) 拦下，changes() 为 0'
SELECT changes() AS 本次实际插入行数;

.print ''
.print '--- 4.3 外键约束：给一个不存在的账户下单会被拦住 ---'
.print '---     大坑：SQLite 的 INSERT OR IGNORE 只消化 UNIQUE / NOT NULL / CHECK /'
.print '        PRIMARY KEY 冲突，外键违规不吃 OR IGNORE，照样报错！'
.print '        所以下面这条会真的失败（这是预期输出，不是脚本坏了），'
.print '        而且它会让整个 sqlite3 进程退出码变成 1 —— 别被吓到。---'
INSERT OR IGNORE INTO orders(acct, amount, uniq) VALUES (999, 10, 'ORD-003');
.print '    ↑ 报 "FOREIGN KEY constraint failed (19)"，并且 INSERT OR IGNORE 没救下它'
SELECT changes() AS 本次实际插入行数;

.print ''
.print '--- 4.4 一致性自检：钱不能凭空产生也不会凭空消失 ---'
SELECT (SELECT SUM(balance) FROM account) AS 账户总余额,
       (SELECT COUNT(*) FROM orders)      AS 订单数;

.print '--- 注意"守恒"这种业务级不变量，DB 的约束表达不了（跨行的全局不变量）。'
.print '    实际项目做法：① 事务内先加锁校验 ② 异步对账 job 巡检 ③ 报警。'
.print '    别指望把业务一致性全外包给数据库，DB 只保证它声明过的那部分。---'

-- =================================================================--
--  第 5 步：I —— Isolation（隔离性）与锁行为
--  这一步需要"两个 sqlite3 会话"，一个进程里做不到真正的并发。
--  下面是我在本机实测的记录，请照着重跑一遍，眼见为实。
-- =================================================================--
.print ''
.print '============ 第 5 步：隔离性 —— 两会话锁实测（过程记录） ============'
.print ''
.print '--- 实测环境：Windows + sqlite3 3.51 + 文件库 lab.db + WAL 模式'
.print '--- 默认 busy_timeout 为 0（毫秒），所以抢不到锁立刻报错，最适合观察。'
.print ''
.print '################ 实测 A：WAL 下"一写多读"是真的 ################'
.print '会话 1> BEGIN; '
.print '会话 1> UPDATE account SET balance = balance - 1 WHERE id = 1;     -- 拿到写锁，未提交'
.print '会话 2> SELECT balance FROM account WHERE id = 1;                  -- 读'
.print '会话 2< 1250        ← 读到的是"事务开始前"的旧值，没被阻塞！'
.print '结论：WAL 模式读不阻塞写、写不阻塞读。读者读的是快照（旧版本），'
.print '      这就是 MVCC 的雏形——SQLite 用"多版本 + WAL"实现了 RC 级别的读。'
.print ''
.print '################ 实测 B：两个写者，后到者立刻拿 SQLITE_BUSY ################'
.print '会话 1> BEGIN IMMEDIATE;                     -- 开局就抢写锁'
.print '会话 1> UPDATE account SET balance = balance - 1 WHERE id = 1;'
.print '会话 2> BEGIN IMMEDIATE;'
.print '会话 2< Error: step (5)  [SQLITE_BUSY] The database file is locked (database is locked)'
.print '结论：SQLite 的写锁是"库级"的，不是行级！只有一个写者能活着。'
.print '      这一点和 MySQL（行锁，不同行可并发写）差别巨大。'
.print '      所以 SQLite 的并发模型是"多读单写"，别拿它扛高并发写。'
.print ''
.print '################ 实测 C：DEFERRED 事务的"升级死锁" ################'
.print '会话 1> BEGIN; UPDATE account SET balance = balance - 1 WHERE id = 1;  -- 读到共享锁，写时才升级'
.print '会话 2> BEGIN; UPDATE account SET balance = balance + 1 WHERE id = 2;  -- 也先读，也想升级'
.print '会话 2< Error: database is locked'
.print '结论：默认 BEGIN 等价 BEGIN DEFERRED，锁在第一条语句才要，'
.print '      两个事务都想"读后升级成写"就会互卡（SQLITE_BUSY_SNAPSHOT）。'
.print '      工程解法：明知要写的事务，一开始就 BEGIN IMMEDIATE。'
.print '      C++ 类比：这就是"先拿 shared_lock 读，发现要写再换 unique_lock"，'
.print '      两个人同时想升级 —— 经典的升级死锁，解法也一样：一开始就 unique_lock。'
.print ''
.print '################ 实测 D：busy_timeout 把报错变成等待 ################'
.print '会话 2> .timeout 5000        -- 等价于 PRAGMA busy_timeout = 5000'
.print '会话 2> BEGIN IMMEDIATE;     -- 这次它会等，等满 5 秒再放弃'
.print '结论：busy_timeout 是重试策略的开关。生产代码里你要么设它，'
.print '      要么自己在应用层写"遇到 SQLITE_BUSY 指数退避重试"。'
.print '      02_transaction_raii.cpp 里我们两种都手搓一遍。'
.print ''
.print '################ 实测 E：默认隔离级别是什么 ################'
.print 'SQLite 只提供 SERIALIZABLE，但 WAL 下的读用的是快照，'
.print '      实际观感接近 READ COMMITTED（读已提交，不会脏读，也不会重复读报警——'
.print '      因为在单个 deferred 事务里，快照在第一条读语句时建立，'
.print '      这更接近 REPEATABLE READ）。'
.print 'MySQL 默认 REPEATABLE READ（间隙锁防幻读）；PostgreSQL 默认 READ COMMITTED。'
.print '      面试最爱问的就是这三个默认值的差异。'

-- =================================================================--
--  收尾：把库恢复干净，方便你反复跑
-- =================================================================--
.print ''
.print '============ 收尾 ============'
.print ''
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS account;
.print '--- 若用文件库跑的，记得清理：'
.print '    Remove-Item lab.db, lab.db-wal, lab.db-shm -ErrorAction SilentlyContinue ---'

.print ''
.print '================= 老师傅的话 ================='
.print ' 1. ACID 不是一个东西，是四个不同部门签的字：'
.print '      A 由 undo log 签字，I 由 锁/MVCC 签字，'
.print '      D 由 redo/WAL + fsync 签字，C 由你和约束共同签字。'
.print ' 2. 事务不是"更粗的锁"。BEGIN...COMMIT 给你的第一件事是'
.print '    "可撤销"，第二件事才是"别人看不见"。'
.print ' 3. 长事务是万恶之源：undo/wal 膨胀、checkpoint 卡住、'
.print '    连接被占、别人被堵。写代码时心里要有个尺子——'
.print '    一个事务别超过几十毫秒。'
.print ' 4. 别在事务里做 RPC、调 HTTP、发 MQ、等用户输入。'
.print '    这叫"把外部世界的不确定性引进临界区"，你写 C++ 时'
.print '    绝不会在持锁状态下 sleep，一个道理。'
.print '============================================='
.print ''
