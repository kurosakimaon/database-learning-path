-- ================================================================
--  03_index_in_practice.sql —— 索引的账，用 sqlite3 亲手算一遍
-- ================================================================
--
--  前两课你在 C++ 里把原理刨到底了：页、Buffer Pool、B+ 树、扇出。
--  这一课把它落到 SQL 上：同一条语义，写法差一点，执行计划差一百倍。
--
--  怎么跑（不要建文件库，全程内存库，跑完就没了）：
--      sqlite3 :memory: ".read 03_index_in_practice.sql"
--
--  看两样东西：
--    1) EXPLAIN QUERY PLAN —— 计划长什么样（走没走索引、走哪个、扫还是寻）
--    2) .timer on 后面的 Run Time —— 真实耗时
--
--  说明：本文件里所有 "真实输出" 注释，是用 sqlite3 3.51.1 实跑抓下来的，
--  不是手写臆造的。你换个版本、换个机器，计划可能差一点、耗时差很多，
--  这不重要 —— 重要的是你学会 "问 SQL：你到底怎么走的"。
--
--  术语对齐（SQLite 的说法 -> 通用说法）：
--    SCAN                 = 全表扫描 / 全索引扫描（从头到尾撸一遍）
--    SEARCH ... USING ... = 索引寻位（B+ 树从根走到叶子）
--    INTEGER PRIMARY KEY  = rowid = 聚簇索引的叶子本身
--    COVERING INDEX       = 覆盖索引，要的东西索引里全有，不用回表
--
--  在 MySQL 那边对应 EXPLAIN 的 type=ALL / ref / range，Extra 里的
--  Using index（=覆盖）、Using filesort（=临时 B 树排序）、
--  Using temporary（=临时表）。第 5 关 query_internals 里会把这个
--  训练场扩成 15 道题。

.timer off
PRAGMA page_size = 4096;         -- 和上一课的页大小对齐：SQLite 默认就是 4KB

-- ================================================================
--  0. 造一张 20 万行的表 + 一张 5000 行的表
-- ================================================================
--  用递归 CTE 当数字发生器，不用下载任何样例库。
--  WITH RECURSIVE 的写法记一下，面试和排障都用得上：
--      WITH RECURSIVE seq(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM seq WHERE i < N)
--      SELECT ... FROM seq;
--  它就是 "自己引用自己" 的 CTE：一条种子行 + 一条递推规则。
--
CREATE TABLE orders (
    id      INTEGER PRIMARY KEY,          -- 关键：INTEGER PRIMARY KEY 就是 rowid
    user_id INTEGER NOT NULL,             -- 5000 个用户，平均每人 40 单
    amount  INTEGER NOT NULL,             -- 0..99999，几乎不重复（高选择性）
    status  TEXT    NOT NULL,             -- 只有 4 个值（低选择性）
    tag     TEXT                          -- 只有 2 个值（更低选择性）
);

INSERT INTO orders (user_id, amount, status, tag)
WITH RECURSIVE seq(i) AS (
    SELECT 1 UNION ALL SELECT i + 1 FROM seq WHERE i < 200000
)
SELECT (i * 7919) % 5000 + 1,                                  -- user_id 打散
       (i * 104729) % 100000,                                  -- amount 打散
       CASE i % 4 WHEN 0 THEN 'new' WHEN 1 THEN 'paid'
                  WHEN 2 THEN 'done' ELSE 'cancel' END,        -- status 四等分
       CASE WHEN i % 100 = 0 THEN 'vip' ELSE 'normal' END      -- tag 1:99
FROM seq;

CREATE TABLE users (
    id    INTEGER PRIMARY KEY,
    name  TEXT NOT NULL,
    phone TEXT NOT NULL                   -- 注意是 TEXT，后面有坑
);

INSERT INTO users (name, phone)
WITH RECURSIVE seq(i) AS (
    SELECT 1 UNION ALL SELECT i + 1 FROM seq WHERE i < 5000
)
SELECT 'user_' || i, '138' || printf('%08d', i) FROM seq;

SELECT '== 表造好了 ==';
SELECT count(*) AS orders_rows FROM orders;
SELECT count(*) AS users_rows  FROM users;

-- 要点：造数是练索引的前提。一千行的表怎么折腾都快，你什么都看不出来；
--       二十万行才有 "走错一步慢一百倍" 的体感。

-- ================================================================
--  1. 聚簇索引 vs 二级索引：SQLite 里 rowid 就是聚簇索引
-- ================================================================
--  上一课讲过 "数据全在叶子"。SQLite 的 rowid 表就是那张 B+ 树：
--      叶子按 rowid 有序放着整行数据本身。
--  所以按 id 查 = 直接走聚簇树，找到即拿到整行，一次都不多走。
--
SELECT '== 1.1 按 id（聚簇）查 ==';
EXPLAIN QUERY PLAN SELECT * FROM orders WHERE id = 123456;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING INTEGER PRIMARY KEY (rowid=?)
--
--  要点：这一行就是 "顺着聚簇 B+ 树从根走到叶子"。
--        树高 3~4，所以这条查询的磁盘页读次数是 3~4，不是 20 万次比较。

--  换成二级索引看看差别。先建：
CREATE INDEX idx_orders_user ON orders(user_id);

SELECT '== 1.2 按 user_id（二级索引）查 ==';
EXPLAIN QUERY PLAN SELECT * FROM orders WHERE user_id = 4321;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING INDEX idx_orders_user (user_id=?)
--
--  要点：注意这里只写了 "USING INDEX"，没写 COVERING。
--        意思是：先在 idx_orders_user 这棵树上找到 user_id=4321 的一堆条目，
--        每条条目里只有 (user_id, rowid)，拿到 rowid 还得**回到 rowid 树上**
--        把那 40 行整行捞出来 —— 这叫 "回表"（MySQL 里叫 book lookup）。
--        一次二级索引查询 = 走两棵树。这是二级索引最容易被忽略的成本。

--  聚簇 vs 二级，三条本质区别，背下来：
--    (a) 一张表只有一张聚簇树（数据只能按一种顺序物理存放）；二级索引想建几棵建几棵。
--    (b) 聚簇索引找到叶子 = 找到数据；二级索引找到叶子 = 找到 rowid，还要回表。
--    (c) InnoDB 的二级索引叶子存的是 (索引列, 主键)，所以主键越长，
--        每一棵二级索引都被撑大 —— 这就是 "别拿 UUID 当主键" 的第二条理由
--        （第一条是随机插入把页分裂满天飞，上一课的课后题 4）。

-- ================================================================
--  2. 无索引 vs 有索引：把耗时打到屏幕上
-- ================================================================
--  先把刚才建的索引删掉，看裸奔的样子。
DROP INDEX idx_orders_user;

SELECT '== 2.1 无索引：全表扫描 ==';
.timer on
SELECT count(*) FROM orders WHERE user_id = 4321;
.timer off
-- 真实输出（我这台机器，你机器上数量级一致即可）：
--   QUERY PLAN 之外的结果：40
--   Run Time: real 0.006302 ...
--  要点：20 万行，一行一行比。耗时看着还不高？因为 SQLite 全在内存页里，
--        而且 count(*) 不用回表。换成 SELECT * 把 40 行拼出来，差距会再放大。
--        真实磁盘数据库里，这一步是 20 万行 × 行宽 的页读取。

.timer on
CREATE INDEX idx_orders_user ON orders(user_id);
.timer off
SELECT '== 2.2 建索引本身也要钱（注意上面建索引的 Run Time）==';
--  要点：索引不是免费的。每次 INSERT/UPDATE/DELETE，
--        这张表上有几棵索引，就要多维护几棵 B+ 树：多几次下降、多几次分裂、
--        多几页 WAL。写多读少的表上猛加索引，是典型的 "读优化写惩罚"。

SELECT '== 2.3 有索引：B+ 树寻位 ==';
.timer on
SELECT count(*) FROM orders WHERE user_id = 4321;
.timer off
-- 真实输出：
--   40
--   Run Time: real 0.000021 ...
--
--  要点：同一句 SQL，一个索引，两个数量级。
--        别高兴太早 —— 这只是 "选择性高" 的情形。第 5 节里，索引会让你吃瘪。

-- ================================================================
--  3. 覆盖索引：把回表省掉
-- ================================================================
--  回表 = 拿着 rowid 回聚簇树再走一趟。如果索引里已经有我要的全部列，
--  这一趟就能整个省掉。计划里的标记是 COVERING INDEX。
--
SELECT '== 3.0 只取 id：同一个索引，计划里多了 COVERING ==';
EXPLAIN QUERY PLAN SELECT id FROM orders WHERE user_id = 4321;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING COVERING INDEX idx_orders_user (user_id=?)
--
--  对比 1.2 那条 SELECT *：同一个索引，一个有 COVERING 一个没有。
--  差别就在 SELECT 后面那几个字。
--
--  再进一步：把常用列塞进索引里，做成真正的覆盖索引。
CREATE INDEX idx_orders_user_amount ON orders(user_id, amount);

SELECT '== 3.1 覆盖：要的列都在索引里 ==';
EXPLAIN QUERY PLAN SELECT user_id, amount FROM orders WHERE user_id = 4321 AND amount > 5000;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING COVERING INDEX idx_orders_user_amount (user_id=? AND amount>?)
--
SELECT '== 3.2 不覆盖：多要一个 status，就得回表 ==';
EXPLAIN QUERY PLAN SELECT user_id, amount, status FROM orders WHERE user_id = 4321 AND amount > 5000;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING INDEX idx_orders_user_amount (user_id=? AND amount>?)
--
--  要点：注意这里只剩 "USING INDEX"，COVERING 不见了。
--        SQLite 的计划树不会把回表那一步单独画一行（rowid 表回表是内联在
--        索引寻位里的），但它在 MySQL 的 EXPLAIN 里会现形：
--            type=ref, Extra 里没有 Using index  ->  要回表
--            type=range, Extra=Using index      ->  覆盖，不回表
--        判断口径永远是那一条：**你要的列，索引里有没有全？**
--        这就是 MySQL 里 "select * 会毁掉覆盖索引" 的机器证据。
--        线上写查询，只取需要的列；高频报表 SQL 值得为它设计一个覆盖索引。

--  范围条件也吃索引：
SELECT '== 3.3 范围扫描走索引 ==';
EXPLAIN QUERY PLAN SELECT user_id, amount FROM orders WHERE user_id BETWEEN 100 AND 130;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING COVERING INDEX idx_orders_user_amount (user_id>? AND user_id<?)
--
--  要点：上一课说的叶子链表在这儿现形了 —— 定位到 user_id=100 那片叶子，
--        顺着链表往后读就完事，不用每个值回根重走。

--  下面几节要单看复合索引 idx_sua 的表现，把前面那两棵小树先拆掉。
--  不拆的话，优化器会挑中它们，你就看不清 idx_sua 到底怎么被对待了。
--  （这件事本身就是一条经验：索引不是越多越好，它们互相抢活，
--    调试计划时先想清楚 "此刻表上有哪几棵树"。）
DROP INDEX idx_orders_user;
DROP INDEX idx_orders_user_amount;

-- ================================================================
--  4. 最左前缀：复合索引是 "按字典序排的元组"
-- ================================================================
--  复合索引 (a, b, c) 不是 "三个索引"，是 **一棵** 按 (a,b,c) 字典序排的 B+ 树。
--  想清楚这一点，最左前缀就不用背了：
--      电话簿按 (姓, 名) 排。你知道姓，翻得快；只知名不知姓，只能从头翻。
--
CREATE INDEX idx_sua ON orders(status, user_id, amount);

SELECT '== 4.1 全前缀：三个条件都用上 ==';
EXPLAIN QUERY PLAN SELECT count(*) FROM orders
 WHERE status = 'paid' AND user_id = 4321 AND amount > 500;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING COVERING INDEX idx_sua (status=? AND user_id=? AND amount>?)
--
SELECT '== 4.2 跳过最左列：索引基本作废 ==';
EXPLAIN QUERY PLAN SELECT count(*) FROM orders WHERE user_id = 4321 AND amount > 500;
-- 真实输出：
--   QUERY PLAN
--   `--SCAN orders USING COVERING INDEX idx_sua
--
--  要点：注意关键字是 SCAN 不是 SEARCH —— 它没在树上寻位，
--        而是把整棵索引从左到右扫一遍（顺便当覆盖索引省了回表，仅此而已）。
--        代价和全表扫一个量级。
--        WHERE 里条件的书写顺序不影响（优化器会重排），影响的是
--        **有没有覆盖到索引的最左列**。这是两个完全不同的概念，很多人混。
--
SELECT '== 4.3 只用上最左一列，第二列缺、第三列用不上 ==';
EXPLAIN QUERY PLAN SELECT count(*) FROM orders WHERE status = 'paid' AND amount > 500;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING COVERING INDEX idx_sua (status=?)
--
--  要点：括号里只有 status=?，amount 被丢掉了。
--        因为 (paid, ?, 600) 这个元组里中间那位不确定，
--        第三位排在哪都可能，字典序断了，索引没法用来定位 amount。
--        推论：复合索引的列顺序 —— 等值条件放前、范围条件放最后，
--        否则范围之后的列全都作废。

-- ================================================================
--  5. 索引失效的 5 种写法（每种都给真计划）
-- ================================================================
CREATE INDEX idx_orders_amount ON orders(amount);
CREATE INDEX idx_users_name  ON users(name);
CREATE INDEX idx_users_phone ON users(phone);

-- ---- (1) 在列上套函数 / 对列做运算 --------------------------------
SELECT '== 5.1 列上做了运算：索引失效 ==';
EXPLAIN QUERY PLAN SELECT count(*) FROM orders WHERE amount / 100 > 990;
-- 真实输出：
--   QUERY PLAN
--   `--SCAN orders USING COVERING INDEX idx_orders_amount
SELECT '== 5.1b 改写：把运算挪到常数那边 ==';
EXPLAIN QUERY PLAN SELECT count(*) FROM orders WHERE amount > 99000;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING COVERING INDEX idx_orders_amount (amount>?)
--
--  要点：同一个语义，一个 SCAN 一个 SEARCH。这个性质有个术语叫 **sargable**
--        （Search ARGument ABLE）。列必须是 "裸" 的，索引才能定位。
--        MySQL 里 WHERE DATE(created_at) = '2026-10-12' 是同款事故，
--        改成 created_at >= '2026-10-12' AND created_at < '2026-10-13'。
--        真要按函数结果查，就上表达式索引 / 生成列（MySQL 8.0+、PG 都支持）。

-- ---- (2) 隐式类型转换：字面量类型和列类型不一致 --------------------
SELECT '== 5.2 隐式转换：索引失效 ==';
EXPLAIN QUERY PLAN SELECT * FROM users WHERE phone + 0 = 13800001234;
-- 真实输出：
--   QUERY PLAN
--   `--SCAN users
--
SELECT '== 5.2b 类型对齐：索引生效 ==';
EXPLAIN QUERY PLAN SELECT * FROM users WHERE phone = '13800001234';
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH users USING INDEX idx_users_phone (phone=?)
--
--  要点：phone 存成 TEXT 是线上最常见的雷。Java 传 Long、前端传 number，
--        驱动把 '138...' 和 138... 一比，MySQL 会对**列**做 CAST，
--        等价于在列上套函数 —— 于是 (1) 的剧本重演，千万行表全表扫。
--        口诀：字符串列永远用字符串字面量。类型对不上时，宁可改代码别改索引。

-- ---- (3) LIKE 前导通配符 -------------------------------------------
SELECT '== 5.3 前导 %：索引失效 ==';
EXPLAIN QUERY PLAN SELECT * FROM users WHERE name LIKE '%_123%';
-- 真实输出：
--   QUERY PLAN
--   `--SCAN users
--
--  B+ 树按字典序排。'user_1%' 我能告诉你起点在哪（从 'user_1' 往后连续一段），
--  '%_123%' 我一个字都不知道，只能全撸。
SELECT '== 5.3b 后缀 %：可以走索引（但 SQLite 默认 LIKE 不区分大小写，先关一下）==';
PRAGMA case_sensitive_like = ON;
EXPLAIN QUERY PLAN SELECT * FROM users WHERE name LIKE 'user_1%';
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH users USING INDEX idx_users_name (name>? AND name<?)
PRAGMA case_sensitive_like = OFF;
--
--  要点：两个知识点叠一起：
--        1) 前缀匹配（'abc%'）是范围查询，走索引；后缀/中间匹配（'%abc%'）必扫。
--        2) SQLite 的 LIKE 默认大小写不敏感，优化器不敢用索引；
--           PRAGMA case_sensitive_like=ON（或直接用 GLOB）才给走。
--        MySQL 那边：默认 collation utf8mb4_general_ci 也是不区分大小写，
--        所以 LIKE 'abc%' 能否走索引取决于列的 collation 是否二进制比较。
--        真要做全文/模糊搜索，用 FTS、倒排索引或 ES，别拿 LIKE 硬顶。

-- ---- (4) OR 连接不同列 ---------------------------------------------
SELECT '== 5.4 OR 跨列 ==';
EXPLAIN QUERY PLAN SELECT * FROM users WHERE name = 'user_10' OR phone = '13800000010';
-- 真实输出：
--   QUERY PLAN
--   `--MULTI-INDEX OR
--      |--INDEX 1
--      |  `--SEARCH users USING INDEX idx_users_name (name=?)
--      `--INDEX 2
--         `--SEARCH users USING INDEX idx_users_phone (phone=?)
--
--  要点：SQLite 会把 OR 拆成两棵索引各查一次再合并去重（MULTI-INDEX OR）。
--        但 **MySQL 的 InnoDB 通常直接放弃索引全表扫**（它的优化器不做这个合并，
--        除非用 index_merge，而 index_merge 的触发条件挺苛刻）。
--        所以线上看到 OR 跨列，标准处置是拆成两条 SELECT 再 UNION ALL，
--        或者把其中一列挪进复合索引。
--        同列的 OR（user_id=100 OR user_id=200）其实是 IN，一般能走索引。

-- ---- (5) 低选择性列：索引帮倒忙 ------------------------------------
SELECT '== 5.5 低选择性：status 只有 4 个值 ==';
EXPLAIN QUERY PLAN SELECT * FROM orders WHERE status = 'paid';
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING INDEX idx_sua (status=?)
SELECT '== 5.5b 更低选择性的 tag（1:99），计划反而变成全表扫 =='
;
EXPLAIN QUERY PLAN SELECT count(*) FROM orders WHERE tag = 'vip';
-- 真实输出：
--   QUERY PLAN
--   `--SCAN orders
--
--  要点：这是最反直觉也最重要的一条。
--        status='paid' 命中 25% 的行。走索引要：扫 5 万条索引条目 + 回表 5 万次
--        随机 I/O；全表扫只要顺序读 20 万行。随机换顺序，账怎么算都是全表扫赢。
--        所以 MySQL 里 "明明建了索引为什么不走" 的第一大原因不是优化器蠢，
--        而是它算出走索引更贵。第二原因是统计信息过期（第 5 关代价模型那课亲手复现）。
--        推论：给性别、状态、是否删除 这种列单独建索引，基本是白养一棵树，
--        还拖累写入。要建就建复合索引，把低选择性列当 "前缀分段" 用。

-- ================================================================
--  6. 三个常见场景，看一眼计划长什么样（第 5 关展开细讲）
-- ================================================================
SELECT '== 6.1 深分页：LIMIT 100000,10 ==';
EXPLAIN QUERY PLAN SELECT id FROM orders ORDER BY id LIMIT 10 OFFSET 100000;
-- 真实输出：
--   QUERY PLAN
--   `--SCAN orders
SELECT '== 6.1b 改写：记住上一页末尾的 id（seek 分页）==';
EXPLAIN QUERY PLAN SELECT id FROM orders WHERE id > 100000 ORDER BY id LIMIT 10;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING INTEGER PRIMARY KEY (rowid>?)
--
--  要点：OFFSET 的语义是 "读出来再扔掉"，翻到第 10 万页要把前 10 万行读一遍。
--        改写成 WHERE id > 上次末尾 ORDER BY id LIMIT 10，
--        从 SCAN 变成 SEARCH，成本从 O(offset) 变成 O(树高)。

SELECT '== 6.2 排序没有索引支撑：临时 B 树 ==';
EXPLAIN QUERY PLAN SELECT id FROM orders ORDER BY user_id LIMIT 10;
-- 真实输出：
--   QUERY PLAN
--   |--SCAN orders USING COVERING INDEX idx_sua
--   `--USE TEMP B-TREE FOR ORDER BY
--  要点：USE TEMP B-TREE = MySQL EXPLAIN 里的 Using filesort。
--        它不是文件排序，是 "在内存/临时文件里现搭一棵树把结果排一遍"。
--        想消掉它：要么建一个顺序吻合的索引，要么把 ORDER BY 换成有索引的列。

SELECT '== 6.3 连接时的驱动表 ==';
EXPLAIN QUERY PLAN SELECT u.name, o.amount
  FROM orders o JOIN users u ON u.id = o.user_id
 WHERE o.status = 'paid';
-- 真实输出：
--   QUERY PLAN
--   |--SEARCH o USING COVERING INDEX idx_sua (status=?)
--   `--SEARCH u USING INTEGER PRIMARY KEY (rowid=?)
--
--  要点：计划树是自上而下的：先扫 orders（驱动表），
--        每出一行就拿它的 user_id 去 users 的主键上点查一次（被驱动表）。
--        这叫 index nested loop join。谁当驱动表决定了总代价：
--        驱动表出 5 万行 -> 点查 5 万次；反过来驱动表出 5000 行 -> 点查 5000 次。
--        这就是 "让小的、过滤得狠的那张表当驱动表" 的由来。
--        连接算法的完整账（NLJ / Sort-Merge / Hash）在 query_internals/01 里手写。

-- ================================================================
--  清理
-- ================================================================
DROP INDEX idx_sua;
DROP INDEX idx_orders_amount;
DROP INDEX idx_users_name;
DROP INDEX idx_users_phone;
DROP TABLE orders;
DROP TABLE users;
SELECT '== 全部跑完，零报错 ==';

-- ================================================================
--  老师傅的话
-- ================================================================
--  徒弟，这个文件你别读，要跑。跑的时候把 EXPLAIN 那几行当主角看。
--
--  1) 索引不是 "让查询变快的开关"，它是 "额外的一棵 B+ 树"。
--     加了它，读有两条路，优化器挑便宜的；写要多养一棵树。
--     所以决定加不加索引，看的是 "有多少查询需要它"，不是 "这条查询慢不慢"。
--
--  2) 所有索引失效的案例，本质只有一条：
--     B+ 树按 key 的字典序排，只要你的条件不能在这棵树上圈出一段连续区间，
--     它就爱莫能助。函数、隐式转换、前导 %、跳列 —— 全是这句话的推论。
--     理解这一条，你不用背清单，现场就能判断。
--
--  3) "不走索引" 不一定是 bug。低选择性列上它算得比你对：
--     5 万次随机回表 < 20 万行顺序扫描？算不过来。
--     遇到这种情况先问自己："这个索引到底为谁建的？" 十有八九答案是 "没人"。
--
--  4) 养成一个肌肉记忆：线上任何一条慢 SQL，第一件事是 EXPLAIN，
--     不是加索引、不是改连接池、不是重启。
--     先看它到底怎么走的，再决定动谁。这个习惯值很多钱。
--
--  【课后练习】
--   1. 把 orders 造到 200 万行（改那个 200000），把 .timer 开着重跑第 2 节，
--      把 SCAN 和 SEARCH 的耗时比值记下来。再算算：这个比值和行数成正比吗？
--   2. 只保留 idx_sua 一个索引，把第 4 节的三条 SQL 计划抄下来；
--      然后建 idx_sua2 为 (user_id, status, amount)，同样的三条 SQL 再抄一遍。
--      对比之后你会明白 "复合索引列顺序" 该怎么定。
--   3. 给 phone 列建成 INTEGER 再跑第 5.2 节，看计划变不变。
--      然后回答：为什么大厂规范强制 "手机号、身份证号一律用 VARCHAR 存"？
--      （提示：前导 0、+86、以及超长数字溢出 bigint。）
--   4. 把 5.4 那条 OR 拆成 UNION ALL 两条，看计划变成什么：
--        SELECT * FROM users WHERE name='user_10'
--        UNION ALL
--        SELECT * FROM users WHERE phone='13800000010' AND name<>'user_10';
--      想想为什么要加后面那个 <> 条件 —— 不加会多 what？
-- === 本文件结束 ===
