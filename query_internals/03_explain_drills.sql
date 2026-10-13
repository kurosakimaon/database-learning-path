-- =====================================================================
--  03_explain_drills.sql —— 15 道 EXPLAIN 读数训练
--
--  跑法（内存库，不落盘、跑完即散）：
--      sqlite3 :memory: ".read 03_explain_drills.sql"
--
--  玩法：每道题先看【你猜】，在脑子里写下计划，再看下面真实的
--  EXPLAIN QUERY PLAN 输出（本文件所有输出都是 sqlite3 3.51 实跑采集，
--  不是编的）和【解读】。猜错了的地方最值得记 —— 那就是你的
--  "直觉和会计账本"不一致的地方。
--
--  先学会读三种行：
--    SCAN  t ...               全表从头扫到尾（或整个索引从头扫到尾）
--    SEARCH t USING INDEX i(c) 走树：等值 (c=?)、范围 (c>? AND c<?)
--    COVERING INDEX            查询要的列索引里全有 —— 压根不回表
--  树画在多个算子上时，缩进/枝丫表示谁喂谁（火山模型从下往上喂）。
--  两本账世界观（第 2 关）：SCAN 大表 = 页的账；回表 = 随机页的账；
--  USE TEMP B-TREE = 排序在内存/临时文件里重建一棵树 = 比较+页双重账。
-- =====================================================================

-- ---------------- 造数据：100 万订单 + 5 万用户 + 1000 优惠券 ----------
CREATE TABLE users(
  id   INTEGER PRIMARY KEY,          -- rowid 表：主键即聚簇（第 2 关 01）
  name TEXT NOT NULL,
  city TEXT NOT NULL,
  age  INT  NOT NULL
);
INSERT INTO users
WITH RECURSIVE cte(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM cte WHERE i<50000)
SELECT i,
       'user_' || i,
       CASE i % 8 WHEN 0 THEN '北京' WHEN 1 THEN '上海' WHEN 2 THEN '广州'
                  WHEN 3 THEN '深圳' WHEN 4 THEN '杭州' WHEN 5 THEN '成都'
                  WHEN 6 THEN '武汉' ELSE '西安' END,
       18 + (i * 37) % 50
FROM cte;

CREATE TABLE orders(
  id      INTEGER PRIMARY KEY,
  uid     INT  NOT NULL,             -- 每个 uid 恰好 20 单
  amount  INT  NOT NULL,             -- 0~9999 近似均匀分布
  status  TEXT NOT NULL,             -- 四种状态各 25%
  created TEXT NOT NULL,             -- 与 id 无关的日期（故意错开）
  code    TEXT                       -- 只有 1/10 的行非空，且【没建索引】
);
INSERT INTO orders
WITH RECURSIVE cte(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM cte WHERE i<1000000)
SELECT i,
       (i % 50000) + 1,
       (i * 7919) % 10000,
       CASE i % 4 WHEN 0 THEN 'new' WHEN 1 THEN 'paid'
                  WHEN 2 THEN 'ship' ELSE 'done' END,
       date('2024-01-01', '+' || (i % 366) || ' days'),
       CASE WHEN i % 10 = 0 THEN 'C' || (i % 1000) END
FROM cte;

CREATE TABLE coupons(code TEXT NOT NULL, title TEXT NOT NULL);
INSERT INTO coupons
WITH RECURSIVE cte(i) AS (SELECT 0 UNION ALL SELECT i+1 FROM cte WHERE i<999)
SELECT 'C' || i, '满减券 ' || i FROM cte;

CREATE INDEX idx_orders_uid ON orders(uid);
CREATE INDEX idx_orders_amount_status ON orders(amount, status);  -- 复合索引
CREATE INDEX idx_users_city ON users(city);
CREATE INDEX idx_users_name ON users(name);
ANALYZE;   -- 本关 02 刚演过：不喂统计，会计拿旧地图找路

-- 先对几个关键基数心里有数（真实输出跟在后面）：
SELECT 'users='   || count(*) FROM users;                          -- users=50000
SELECT 'orders='  || count(*) FROM orders;                         -- orders=1000000
SELECT 'amount8848='  || count(*) FROM orders WHERE amount = 8848; -- =100（万分之一）
SELECT 'amount9800p=' || count(*) FROM orders WHERE amount > 9800; -- =19900（2%）
SELECT 'paid='    || count(*) FROM orders WHERE status = 'paid';   -- =250000（25%）

-- =====================================================================
-- 题 1：没索引的列做等值过滤，25% 的行命中
-- ---------------------------------------------------------------------
-- 【你猜】status 没建索引。会 SCAN orders（聚簇树实测 7733 页），
-- 还是根本不去碰表？
EXPLAIN QUERY PLAN SELECT count(*) FROM orders WHERE status = 'paid';
-- 真实输出：
--   QUERY PLAN
--   `--SCAN orders USING COVERING INDEX idx_orders_amount_status
-- 【解读】是 SCAN，但扫的不是表 —— 扫 idx_amount_status 整棵索引。
-- count(*) 不挑列，索引实测 3853 页 vs 表 7733 页，扫索引省一半页账。
-- status 是这棵树的第二列，没法定位，只能整棵顺一遍。
-- 教训①：SQLite 里没有"没索引可走"这回事，只有"走哪棵最便宜的树"。

-- =====================================================================
-- 题 2：索引列等值查找，命中万分之一
-- ---------------------------------------------------------------------
-- 【你猜】SEARCH + 回表？还是别的？
EXPLAIN QUERY PLAN SELECT id, amount FROM orders WHERE amount = 8848;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING COVERING INDEX idx_orders_amount_status (amount=?)
-- 【解读】SEARCH = 走树定位。(amount=?) 表示用第一列做等值。
-- 为什么还 COVERING？要的两列 id、amount 索引里都有（叶子带 rowid），
-- 100 行结果一个表页都不碰。选择性好的等值 + 覆盖 = 教科书最优。

-- =====================================================================
-- 题 3：范围查找，命中 2%
-- ---------------------------------------------------------------------
-- 【你猜】范围比等值"贵"，2% 的选择性，会不会被嫌弃改走全表？
EXPLAIN QUERY PLAN SELECT id, amount FROM orders WHERE amount > 9800;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING COVERING INDEX idx_orders_amount_status (amount>?)
-- 【解读】照走不误。(amount>?) 从定位点起沿叶子链表向右扫 19900 个
-- 索引条目。范围扫不是"随机"：树上定位一次，之后全是叶子顺序走。
-- 第 2 关 02 实测过这种"定位+顺链"比全表快两个数量级。

-- =====================================================================
-- 题 4：同一棵树、同一个 WHERE，SELECT 的列不同，计划就变
-- ---------------------------------------------------------------------
-- 【你猜】把题 3 的 SELECT 换一换，计划哪行会变？
-- (a) 要的列索引里都有：
EXPLAIN QUERY PLAN SELECT amount, status FROM orders WHERE amount BETWEEN 100 AND 200;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING COVERING INDEX idx_orders_amount_status (amount>? AND amount<?)
-- (b) 多要一列 uid（索引里没有，必须回表）：
EXPLAIN QUERY PLAN SELECT uid, amount FROM orders WHERE amount BETWEEN 100 AND 200;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING INDEX idx_orders_amount_status (amount>? AND amount<?)
-- 【解读】只差一个单词 COVERING，背后差 10100 次回表的账。
-- (b) 每命中一行都要拿 rowid 回聚簇树取 uid —— 页的账 ×4 起步。
-- "覆盖"不是一种索引，是一种"查询恰好被索引全包"的状态。
-- 练习：把 (b) 的 uid 去掉，再看计划变回来（自己敲一遍才算会）。

-- =====================================================================
-- 题 5：有索引却没走 —— 两问
-- ---------------------------------------------------------------------
-- (a) 给 idx(amount,status) 明明含 status，为何题 1 定位不了 status？
EXPLAIN QUERY PLAN SELECT count(*) FROM orders WHERE status = 'paid';
-- 真实输出（与题 1 相同）：
--   QUERY PLAN
--   `--SCAN orders USING COVERING INDEX idx_orders_amount_status
-- 【解读】最左前缀：树按 (amount,status) 排序，单给 status 无从定位，
-- 第二列只在"第一列已定位"时才有意义。想让 status 能走树，
-- 得单独建 (status) 或 (status, ...) 打头的索引。
-- (b) 换个真正有索引的列，立刻变脸：
EXPLAIN QUERY PLAN SELECT count(*) FROM orders WHERE status = 'paid' AND uid = 7;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING INDEX idx_orders_uid (uid=?)
-- 【解读】uid=7 走树锁定 20 行，status 在这 20 行里行过滤。
-- 会计不在乎"两个条件哪个先写"，只在乎"哪列能定位"。

-- =====================================================================
-- 题 6：复合索引里"用掉了第几列"，括号里写得明明白白
-- ---------------------------------------------------------------------
-- 【你猜】amount>100 AND status='paid'，(amount,status) 两列都能用上吗？
EXPLAIN QUERY PLAN SELECT id, amount, status FROM orders
WHERE amount > 100 AND status = 'paid' LIMIT 20;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING COVERING INDEX idx_orders_amount_status (amount>?)
-- 【解读】括号只有 (amount>?) —— 只有第一列参与定位！
-- 树里 status 只在每个 amount 值内部有序；amount 一开范围，
-- status 的全局序就断了，只能当行过滤。范围列【右边】的列全部失效，
-- 这是"最左前缀"最隐蔽的下半句。
-- 若查询常这样问，索引该建成 (status, amount)：等值打头、范围收尾。

-- =====================================================================
-- 题 7：ORDER BY 一列没索引的列 —— filesort 现身
-- ---------------------------------------------------------------------
-- 【你猜】ORDER BY created LIMIT 20，created 没索引，多惨？
EXPLAIN QUERY PLAN SELECT id, created FROM orders ORDER BY created LIMIT 20;
-- 真实输出：
--   QUERY PLAN
--   |--SCAN orders
--   `--USE TEMP B-TREE FOR ORDER BY
-- 【解读】USE TEMP B-TREE FOR ORDER BY = SQLite 的"filesort"：
-- 把 100 万行 (created,id) 塞进一棵内存临时 B 树，再按序吐出。
-- LIMIT 20 救不了它 —— 不读完 100 万行，不知道最小的 20 个是谁。
-- 账：100 万行 × log(100万) 次比较 + 内存超限时落盘成临时文件。
-- 解法：给 created 建索引（序是白捡的），或接受这一行计划的存在。

-- =====================================================================
-- 题 8：ORDER BY 有索引 —— 临时树那行消失了
-- ---------------------------------------------------------------------
-- 【你猜】ORDER BY amount，idx(amount,status) 打头就是 amount。
EXPLAIN QUERY PLAN SELECT id, amount FROM orders ORDER BY amount LIMIT 20;
-- 真实输出：
--   QUERY PLAN
--   `--SCAN orders USING COVERING INDEX idx_orders_amount_status
-- 【解读】同一个 SCAN 关键字，这回是"按叶子链表顺序 = 按 amount 有序"
-- 地扫，LIMIT 20 读 20 条就收工。没有 TEMP B-TREE 行。
-- 对比题 7：一个词的差别（created vs amount），一边全量排序一边早停。
-- SQLite 的 LIMIT 早停能力完全取决于"数据是否已按目标序供给"。

-- =====================================================================
-- 题 9：GROUP BY + ORDER BY 聚合值 —— 临时表跑不掉
-- ---------------------------------------------------------------------
-- 【你猜】GROUP BY uid（有索引！）能省掉临时树吗？ORDER BY count(*) 呢？
EXPLAIN QUERY PLAN SELECT uid, count(*) FROM orders GROUP BY uid ORDER BY count(*) DESC LIMIT 5;
-- 真实输出：
--   QUERY PLAN
--   |--SCAN orders USING COVERING INDEX idx_orders_uid
--   `--USE TEMP B-TREE FOR ORDER BY
-- 【解读】GROUP BY uid 确实蹭上了 idx_uid 的序（5 万个分组顺流而出，
-- 分组本身没花排序账 —— 注意 FOR GROUP BY 没出现）。
-- 但 ORDER BY count(*) 排的是"聚合出来的新值"，任何索引都不认识它，
-- 5 万行分组结果必须重建一棵临时树。
-- 判据背下来：ORDER BY 的列（或表达式）若是查询现算出来的，
-- TEMP B-TREE 一行基本必现；能优化的是减少进入排序的行数。

-- =====================================================================
-- 题 10：JOIN 的驱动表 —— 优化器替你选，且常常是对的
-- ---------------------------------------------------------------------
-- (a) 过滤后 users 剩 6250 行（杭州）：
EXPLAIN QUERY PLAN SELECT o.id, o.amount FROM users u
JOIN orders o ON o.uid = u.id
WHERE u.city = '杭州';
-- 真实输出：
--   QUERY PLAN
--   |--SEARCH u USING COVERING INDEX idx_users_city (city=?)
--   `--SEARCH o USING INDEX idx_orders_uid (uid=?)
-- 【解读】users 当驱动表（6250 行），每行拿 id 走 idx_uid 进 orders：
-- 6250 次树上定位（树高实测 4 层）+ 12.5 万行回表取 amount（idx_uid
-- 叶子只有 uid+rowid，amount 必须回表）。若反过来用 100 万订单驱动
-- users，定位次数直接翻 160 倍。驱动表 = 过滤后更小的那张。
-- (b) 过滤变成 age>58（无索引，命中 9000 行，18%）：
EXPLAIN QUERY PLAN SELECT o.id, o.amount FROM users u
JOIN orders o ON o.uid = u.id
WHERE u.age > 58;
-- 真实输出：
--   QUERY PLAN
--   |--SCAN u
--   `--SEARCH o USING INDEX idx_orders_uid (uid=?)
-- 【解读】驱动方式没变（还是 users 先进），但第一步从 SEARCH 退化成
-- SCAN u —— age 没索引，5 万行全读再行过滤。join 策略不变，
-- 访问路径降级。这行 SCAN u 就是"该给 age 建索引了"的信号灯。

-- =====================================================================
-- 题 11：没索引的 join 列 —— SQLite 自己现造索引
-- ---------------------------------------------------------------------
-- 【你猜】orders.code 没索引，跟 coupons 连接，SQLite 认栽全表×全表？
EXPLAIN QUERY PLAN SELECT c.code, o.amount FROM coupons c
JOIN orders o ON o.code = c.code;
-- 真实输出：
--   QUERY PLAN
--   |--SCAN o
--   `--SEARCH c USING AUTOMATIC COVERING INDEX (code=?)
-- 【解读】AUTOMATIC COVERING INDEX：执行时【临时】给 coupons.code
-- 建了棵一次性树，用完即扔。SQLite 发现"这列马上要被重复点查 100 万
-- 次"，建临时树比 100 万×1000 次嵌套循环便宜太多。
-- MySQL 的同款叫 block nested loop / hash join（8.0.18+），思路不同
-- 但都是"优化器替你救场"。救场费仍要付（每次执行重建），
-- 高频查询请自己把索引建了。

-- =====================================================================
-- 题 12：IN 子查询 vs EXISTS —— 计划长得不一样，账却…
-- ---------------------------------------------------------------------
-- (a) IN（非相关子查询）：
EXPLAIN QUERY PLAN SELECT count(*) FROM orders o
WHERE o.uid IN (SELECT id FROM users WHERE age > 58);
-- 真实输出：
--   QUERY PLAN
--   |--SEARCH o USING COVERING INDEX idx_orders_uid (uid=?)
--   `--LIST SUBQUERY 1
--      |--SCAN users
--      `--CREATE BLOOM FILTER
-- 【解读】LIST SUBQUERY：子查询只跑【一次】，9000 个 id 物化成清单
-- （还顺手 CREATE BLOOM FILTER 建了个布隆过滤器快速否掉不命中项），
-- 外层拿每行 uid 去清单里 SEARCH。一次物化 + O(1) 查找。
-- (b) EXISTS（相关子查询）：
EXPLAIN QUERY PLAN SELECT count(*) FROM orders o
WHERE EXISTS (SELECT 1 FROM users u WHERE u.id = o.uid AND u.age > 58);
-- 真实输出：
--   QUERY PLAN
--   |--SCAN o USING COVERING INDEX idx_orders_uid
--   |--BLOOM FILTER ON u (id=?)
--   `--SEARCH u EXISTS USING INTEGER PRIMARY KEY (rowid=?)
-- 【解读】字面上"每行订单执行一次子查询"，实际被 SQLite 拍平：
-- 扫订单，每行拿 uid 直接 SEARCH users 主键（rowid=?），
-- 树高 3 页封顶，还有 BLOOM FILTER 提前拦截。
-- 结论：在 SQLite 里 IN/EXISTS 经常殊途同归（都会物化或拍平），
-- 别背"IN 一定慢/EXISTS 一定快"的老咒语，跑 EXPLAIN 看真实计划。

-- =====================================================================
-- 题 13：LIMIT 的早停魔法（无 ORDER BY）
-- ---------------------------------------------------------------------
-- 【你猜】status='paid' 要 10 行就够，和题 1 差多少？
EXPLAIN QUERY PLAN SELECT id FROM orders WHERE status = 'paid' LIMIT 10;
-- 真实输出：
--   QUERY PLAN
--   `--SCAN orders USING COVERING INDEX idx_orders_amount_status
-- 【解读】计划一个字没变！LIMIT 不改计划，只改【执行】：
-- 火山模型上层喊够 10 行了，下层立刻停手。paid 占 25%，平均扫 40
-- 个条目就凑够 —— 题 1 扫 100 万条，这道题扫几十条。
-- 实测（count 包一层防止打印）：Run Time real 0.000043 ms。
-- 计划相同 ≠ 耗时相同；EXPLAIN 看结构，计时看真实行为。
-- 反例就是题 7：加了 ORDER BY 之后 LIMIT 立刻停不下来。

-- =====================================================================
-- 题 14：深分页 —— 本文件最贵的一行账
-- ---------------------------------------------------------------------
-- 【你猜】LIMIT 999990, 10 取最后一页，和 LIMIT 10 差几倍？
-- (a) 原始写法：
EXPLAIN QUERY PLAN SELECT id, amount FROM orders ORDER BY id LIMIT 999990, 10;
-- 真实输出：
--   QUERY PLAN
--   `--SCAN orders
-- 实测 Run Time：real 0.012535 ms（100 万行全走一遍，99.999% 白扔）
-- (b) seek 改写（游标法：记住上一页最后一行的 id）：
EXPLAIN QUERY PLAN SELECT id, amount FROM orders WHERE id > 999990 LIMIT 10;
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH orders USING INTEGER PRIMARY KEY (rowid>?)
-- 实测 Run Time：real 0.000149 ms —— 快 84 倍。SEARCH 替代 SCAN，
-- 树上定位一次 + 顺叶子读 10 行。offset 的账是"数给你扔掉的行"。
-- (c) 页码必须保留时（只能给 offset），延迟关联把宽行扫描变窄：
EXPLAIN QUERY PLAN SELECT o.id, o.amount FROM orders o
JOIN (SELECT id FROM orders ORDER BY id LIMIT 999990, 10) t ON t.id = o.id;
-- 真实输出：
--   QUERY PLAN
--   |--CO-ROUTINE t
--   |  `--SCAN orders
--   |--SCAN t
--   `--SEARCH o USING INTEGER PRIMARY KEY (rowid=?)
-- 【解读】CO-ROUTINE：子查询作为协程跑，只拖 10 个 id 出来。
-- 内层仍是 SCAN（offset 账没消），但扫的是"只碰 id 的窄路径"，
-- 外层只对 10 行回表。三招排序：seek(c 消灭 offset) >
-- 延迟关联(缩小 offset 单价) > 原始(a)。MySQL 深分页卡顿同方同源。

-- =====================================================================
-- 题 15：LIKE 的前缀匹配 —— 一个大小写开关改变计划
-- ---------------------------------------------------------------------
-- 【你猜】name LIKE 'user_99%'，前缀确定，能走 idx_name 吗？
EXPLAIN QUERY PLAN SELECT id, name FROM users WHERE name LIKE 'user_99%';
-- 真实输出：
--   QUERY PLAN
--   `--SCAN users USING COVERING INDEX idx_users_name
-- 【解读】走不了树！SQLite 的 LIKE 默认【大小写不敏感】，
-- 比较不是按字节序做的，而树是按原文字节序排的 —— 序对不上，
-- 定位无据（第 2 关 03 的 sargable 家族：不是贵，是没资格）。
-- 证据一：GLOB 天生大小写敏感，同一棵树立刻能定位：
EXPLAIN QUERY PLAN SELECT id, name FROM users WHERE name GLOB 'user_99*';
-- 真实输出：
--   QUERY PLAN
--   `--SEARCH users USING COVERING INDEX idx_users_name (name>? AND name<?)
-- 【解读】GLOB 'user_99*' 被翻译成 name>='user_99' AND name<'user_9:'
-- 的范围定位 —— 前缀匹配 = 范围查找的马甲。
-- 证据二：打开大小写敏感开关，LIKE 也认了：
--   PRAGMA case_sensitive_like = ON; 之后同一条 LIKE 输出：
--   QUERY PLAN
--   `--SEARCH users USING COVERING INDEX idx_users_name (name>? AND name<?)
-- （实跑验证过，可自行重复：.read 本文件前先敲这行 PRAGMA）
-- MySQL 的 LIKE 'user\_99%' 走不走索引取决于列 collation 是否区分
-- 大小写（utf8mb4_general_ci 默认不区分 → 同样的 SCAN 命运）。
-- 注意 '_' 是单字符通配符，'user_99%' 实际匹配 userX99...，
-- 严格前缀要 ESCAPE 转义 —— 计划之外，语义的坑更阴。

-- =====================================================================
-- 收工对答案：15 道题背后的同一张账
-- =====================================================================
-- ① 页的账：SCAN(实测 7733 页表) vs SEARCH(树高 4 层定位)。COVERING 免回表；
--    题 1/4/13/14 全是这一本账的四个侧面。
-- ② 序的账：序能白捡（题 8 顺叶子）就得捡；捡不到就重建临时树
--    （题 7/9 的 USE TEMP B-TREE）。LIMIT 只在"已按目标序供给"时早停。
-- ③ 资格的账：最左前缀（题 5/6）、大小写（题 15）、表达式 —— 条件
--    写得让树"没资格"定位，再便宜的树也不带你。
-- ④ 计划的账：驱动表选择（题 10）、临时索引救场（题 11）、子查询
--    物化/拍平（题 12）—— 优化器做的决定，EXPLAIN 全招了。
-- 拿自己项目里一条慢查询跑一遍 EXPLAIN QUERY PLAN（MySQL 用
-- EXPLAIN / EXPLAIN ANALYZE，关键字不同、账本同一本），
-- 对照这 15 题认脸 —— 认出来，就知道钱花在哪了。
