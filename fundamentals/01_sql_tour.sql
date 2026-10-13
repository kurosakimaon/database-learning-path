-- =====================================================================
--  01_sql_tour.sql —— 图书借阅系统里的 SQL 漫游
--  老师傅带徒弟系列 · 第 1 关 · 第 1 课
-- =====================================================================
--
--  这是一个"小而完整"的业务场景：图书馆。
--  三张表：readers（读者）、books（图书）、loans（借阅记录）。
--  整个文件可以从头到尾一口气跑完，每条 SQL 都会真实执行。
--
--  怎么用（PowerShell，先 cd 到本文件所在目录）：
--    1) 跑着玩，不落盘：
--         sqlite3 -bail :memory: ".read 01_sql_tour.sql"
--    2) 建一个持久练习库，跑完还能进去接着折腾：
--         sqlite3 practice.db
--         sqlite> .read 01_sql_tour.sql
--         sqlite> .tables
--
--  每个小节的结构都是：先说"问题"（业务上要查什么），
--  再给 SQL，最后写"要点"。先自己想十秒，再看答案。
-- =====================================================================


-- ================================================================
-- 第 1 节：建库建表——先有schema，后有数据
-- ================================================================
-- 问题：把"图书馆"抽象成三张表。
-- 用 C++ 的话说：CREATE TABLE 就是 struct 定义，
-- 一张表 ≈ std::vector<Row>，Row 就是那张表的一行。

-- 打开外键开关。SQLite 的外键检查默认是关闭的（大坑！），
-- 每个连接都要手动打开。
PRAGMA foreign_keys = ON;

-- IF EXISTS：存在才删，不存在也不报错。先删 loans，
-- 因为它引用另外两张表（删除顺序和依赖方向相反）。
DROP TABLE IF EXISTS loans;
DROP TABLE IF EXISTS books;
DROP TABLE IF EXISTS readers;

-- 读者表
CREATE TABLE readers (
    reader_id INTEGER PRIMARY KEY,          -- INTEGER PRIMARY KEY ≈ 自增主键（本质是 rowid 的别名）
    name      TEXT     NOT NULL,
    age       INTEGER  CHECK (age BETWEEN 6 AND 120),  -- CHECK：写入前断言，类似 assert
    city      TEXT     NOT NULL DEFAULT '未知',
    join_date TEXT     NOT NULL             -- 简化起见，日期用 TEXT 存 'YYYY-MM-DD'
);

-- 图书表
CREATE TABLE books (
    book_id  INTEGER PRIMARY KEY,
    title    TEXT NOT NULL UNIQUE,          -- UNIQUE：书名不允许重复（可建索引加速查重）
    category TEXT NOT NULL,
    price    REAL NOT NULL CHECK (price > 0),
    stock    INTEGER NOT NULL DEFAULT 0 CHECK (stock >= 0)
);

-- 借阅记录表（连接读者和图书的"事件表"）
CREATE TABLE loans (
    loan_id     INTEGER PRIMARY KEY,
    reader_id   INTEGER NOT NULL REFERENCES readers(reader_id),  -- 外键：读者必须真实存在
    book_id     INTEGER NOT NULL REFERENCES books(book_id),      -- 外键：图书必须真实存在
    loan_date   TEXT    NOT NULL,
    return_date TEXT,                        -- 允许 NULL：NULL = 还没还！
    CHECK (return_date IS NULL OR return_date >= loan_date)      -- 归还日期不能早于借出日期
);

-- 要点：
-- 1. INTEGER PRIMARY KEY 在 SQLite 里很特殊：它就是 rowid，插入时自动分配、天然有序。
-- 2. REFERENCES ... 是"列级外键"写法，声明"这一列的值必须在另一张表里存在"。
-- 3. CHECK 约束是数据库层的 assert，比"全靠程序自觉"可靠得多。


-- ================================================================
-- 第 2 节：插入数据——数据是练习的灵魂，别偷懒
-- ================================================================
-- 问题：灌入读者 10 人、图书 12 本、借阅记录 16 条。

INSERT INTO readers (reader_id, name, age, city, join_date) VALUES
    (1,  '张伟', 28, '北京', '2023-01-15'),
    (2,  '李娜', 34, '上海', '2023-03-02'),
    (3,  '王强', 22, '北京', '2023-05-20'),
    (4,  '刘洋', 41, '广州', '2022-11-08'),
    (5,  '陈静', 29, '上海', '2024-01-10'),
    (6,  '杨帆', 35, '深圳', '2024-02-18'),
    (7,  '赵磊', 19, '北京', '2024-06-01'),
    (8,  '孙悦', 45, '广州', '2021-09-30'),
    (9,  '周涛', 31, '成都', '2023-08-14'),
    (10, '吴敏', 26, '上海', '2024-03-25');

INSERT INTO books (book_id, title, category, price, stock) VALUES
    (1,  'C++ Primer',            '编程',   128.00, 5),
    (2,  'Effective C++',         '编程',    69.00, 3),
    (3,  '深入理解计算机系统',     '编程',   139.00, 4),
    (4,  '数据库系统概念',         '数据库', 118.00, 6),
    (5,  'SQL必知必会',           '数据库',  49.00, 8),
    (6,  '人类简史',               '历史',    68.00, 10),
    (7,  '万历十五年',             '历史',    45.00, 4),
    (8,  '三体',                   '科幻',    98.00, 12),
    (9,  '流浪地球',               '科幻',    39.00, 5),
    (10, '百年孤独',               '文学',    55.00, 7),
    (11, '活着',                   '文学',    39.00, 9),
    (12, '设计模式',               '编程',    89.00, 2);

-- 借阅记录：return_date 为 NULL 表示"借出未还"。
-- 注意 7 号读者赵磊一条记录都没有（后面 LEFT JOIN 会用到这个设定）；
-- 12 号书《设计模式》也一次都没被借出去过。
INSERT INTO loans (loan_id, reader_id, book_id, loan_date, return_date) VALUES
    (1,  1, 1,  '2024-01-05', '2024-01-20'),
    (2,  1, 4,  '2024-02-10', '2024-03-01'),
    (3,  2, 8,  '2024-02-15', '2024-03-10'),
    (4,  3, 2,  '2024-03-01', NULL),
    (5,  3, 5,  '2024-03-12', '2024-03-25'),
    (6,  4, 6,  '2024-03-20', NULL),
    (7,  2, 3,  '2024-04-02', '2024-04-28'),
    (8,  5, 10, '2024-04-10', '2024-04-22'),
    (9,  1, 8,  '2024-05-01', NULL),
    (10, 6, 9,  '2024-05-06', '2024-05-30'),
    (11, 8, 7,  '2024-05-15', NULL),
    (12, 3, 8,  '2024-06-01', '2024-06-18'),
    (13, 9, 1,  '2024-06-05', NULL),
    (14, 2, 5,  '2024-06-12', '2024-06-25'),
    (15, 10, 11,'2024-06-20', NULL),
    (16, 4, 5,  '2024-07-01', '2024-07-15');

-- 要点：
-- 1. 显式写出列名再插值，永远比 INSERT INTO t VALUES(...) 抗改版——
--    表加一列，前者不动照样跑，后者全线崩。
-- 2. 数据故意留了"钩子"：未归还的记录、没借过书的读者、没被借过的书。
--    后面每一节都会踩到其中一个。


-- ================================================================
-- 第 3 节：SELECT 基本查询——投影与取数
-- ================================================================
-- 问题：把整张读者表捞出来看看。
SELECT * FROM readers;

-- 问题：只关心姓名和城市两列（像从 struct 里只挑两个成员打印）。
SELECT name, city FROM readers;

-- 要点：
-- 1. SELECT 的正式流程是 FROM → WHERE → 投影（SELECT 列），
--    虽然书写时 SELECT 写在最前面。记住这个顺序，后面理解
--    "为什么 WHERE 里不能用 SELECT 起的别名"就不费劲了。
-- 2. SELECT * 适合探索，正式代码里请写明确列名：表结构一变，
--    依赖列序的程序就全乱套。


-- ================================================================
-- 第 4 节：WHERE 过滤——条件表达式
-- ================================================================
-- 问题：找出所有北京的读者。（预期 3 行）
SELECT reader_id, name, age, city
FROM readers
WHERE city = '北京';

-- 问题：找"年龄小于 30 并且住在上海"的读者。（预期 2 行：陈静、吴敏）
SELECT name, age, city
FROM readers
WHERE age < 30 AND city = '上海';

-- 问题：找价格不在 40~70 元区间之外的反面——在区间内的书。（预期 5 行）
SELECT title, price
FROM books
WHERE price BETWEEN 40 AND 70;

-- 问题：科幻和文学类的书。（IN ≈ 一串 OR 的简写）（预期 4 行）
SELECT title, category, price
FROM books
WHERE category IN ('科幻', '文学');

-- 问题：书名里带"年"字的书。LIKE 的 % 是任意长度通配。（预期 2 行：万历十五年、百年孤独）
SELECT title
FROM books
WHERE title LIKE '%年%';

-- 问题：书名以"C++"结尾的书（后缀匹配）。（预期 1 行：Effective C++）
-- 顺带记两条：LIKE 里 % = 任意长度，_ = 恰好一个字符；
-- LIKE 对 ASCII 字母大小写不敏感（'c++' 也能匹配），对中文按字面比。
SELECT title
FROM books
WHERE title LIKE '%C++';

-- 问题：所有"借出但还没还"的记录。注意：判空必须用 IS NULL，
-- 不能用 = NULL——NULL 的意思是"未知"，任何与它的比较都是 unknown。
-- （预期 6 行）
SELECT loan_id, reader_id, book_id, loan_date
FROM loans
WHERE return_date IS NULL;

-- 要点：
-- 1. = 只给值用；和 NULL 打交道只有 IS NULL / IS NOT NULL。
--    把 = NULL 写进代码，条件永远不成立，且不报错——最阴的坑。
-- 2. 字符串用单引号。双引号在 SQLite 里是标识符（列名/表名），
--    混用会出怪事。
-- 3. != 和 <> 等价，SQL 标准写法是 <>。


-- ================================================================
-- 第 5 节：ORDER BY 与 LIMIT——排序与截取
-- ================================================================
-- 问题：按年龄从大到小列读者，只看前 3 名。（孙悦45、刘洋41、杨帆35）
SELECT name, age
FROM readers
ORDER BY age DESC
LIMIT 3;

-- 问题：最贵的 5 本书；价格相同时按书名排（第二排序键保证结果稳定）。
SELECT title, price
FROM books
ORDER BY price DESC, title
LIMIT 5;

-- 问题：按编号排第 6~8 名读者（分页：跳过 5 条取 3 条）。
SELECT reader_id, name
FROM readers
ORDER BY reader_id
LIMIT 3 OFFSET 5;

-- 要点：
-- 1. 不写 ORDER BY，行序是不保证的——SQLite 通常按 rowid 给，
--    但那是"心情"，不是承诺。要稳定顺序就必须显式排序。
-- 2. LIMIT n OFFSET m 就是分页。C++ 里像 vector 的
--    std::next(v.begin(), m) 取 n 个。


-- ================================================================
-- 第 6 节：聚合、GROUP BY 与 HAVING——从行到统计
-- ================================================================
-- 问题：一共有多少条借阅记录？（预期 16）
SELECT COUNT(*) FROM loans;

-- 问题：多少条已经归还了？COUNT(列名) 只数非 NULL。（预期 10）
SELECT COUNT(return_date) AS returned_cnt FROM loans;

-- 问题：全馆图书总库存、平均价格。（75 册；均价 78.0）
SELECT SUM(stock) AS total_stock, AVG(price) AS avg_price FROM books;

-- 问题：每个分类各有多少本书、总价多少？
-- GROUP BY ≈ std::ranges::partition_by 之后逐组归并。
-- （编程4、数据库2、历史2、科幻2、文学2）
SELECT category, COUNT(*) AS cnt, SUM(price) AS total_price
FROM books
GROUP BY category;

-- 问题：只要"分类总价超过 150 元"的分类。HAVING 是对"组"过滤，
-- WHERE 是对"行"过滤——位置不同，语义不同。（预期 2 行：编程 425、数据库 167）
SELECT category, COUNT(*) AS cnt, SUM(price) AS total_price
FROM books
GROUP BY category
HAVING SUM(price) > 150;

-- 要点：
-- 1. 逻辑顺序：FROM → WHERE（过滤行）→ GROUP BY → HAVING（过滤组）
--    → SELECT → ORDER BY → LIMIT。
-- 2. 加了 GROUP BY 之后，SELECT 里只能出现"分组列"或聚合函数，
--    否则那一列"该取组里哪一行的值"根本说不清。
-- 3. COUNT(*) 数行数，COUNT(列) 数非 NULL——差别的根源就是 NULL。


-- ================================================================
-- 第 7 节：JOIN——把表"连"起来查
-- ================================================================
-- 问题：列出借阅明细：谁、借了哪本书、什么时候。（预期 16 行）
-- loans 里只有 id，没有名字；名字在 readers 和 books 里。
-- JOIN 就是"顺着外键把行拼起来"。
SELECT l.loan_id, r.name AS reader, b.title AS book, l.loan_date
FROM loans l
JOIN readers r ON r.reader_id = l.reader_id      -- JOIN = INNER JOIN
JOIN books  b ON b.book_id    = l.book_id
ORDER BY l.loan_id;

-- 问题：每个读者的借阅次数，"从没借过书"的也要显示成 0。
-- INNER JOIN 会把赵磊（没借过书）整个丢掉，LEFT JOIN 保留左表全部行，
-- 右表补 NULL。COUNT(l.loan_id) 不数 NULL，正好得到 0。（预期 10 行）
SELECT r.name, COUNT(l.loan_id) AS borrow_times
FROM readers r
LEFT JOIN loans l ON l.reader_id = r.reader_id
GROUP BY r.reader_id, r.name
ORDER BY borrow_times DESC, r.name;

-- 问题：哪些书从来没被借出去过？LEFT JOIN + IS NULL 是找"没配对"的标准套路。
-- （预期 1 行：设计模式）
SELECT b.title
FROM books b
LEFT JOIN loans l ON l.book_id = b.book_id
WHERE l.loan_id IS NULL;

-- 要点：
-- 1. INNER JOIN：两边都有才留。LEFT JOIN：左边全留，右边没有补 NULL。
--    想亲眼看清五种 JOIN 的行级差异，去打开 join_visualizer.html。
-- 2. 给表起短别名（l/r/b），多表查询会短一半，可读性翻倍。
-- 3. ON 写连接条件，WHERE 写过滤条件，别混着写——尤其 LEFT JOIN
--    时把条件放错地方，结果集完全不同（留个思考：为什么？）。


-- ================================================================
-- 第 8 节：自连接——一张表跟自己连
-- ================================================================
-- 问题：找出"住在同一城市的读者对"，用于同城换书推荐。
-- 同一张表连两次，起两个别名；a.reader_id < b.reader_id 同时干掉
-- 自我配对和重复对。（预期 7 行）
SELECT a.name AS reader_a, b.name AS reader_b, a.city
FROM readers a
JOIN readers b ON a.city = b.city AND a.reader_id < b.reader_id
ORDER BY a.city, a.reader_id, b.reader_id;

-- 要点：
-- 1. 自连接常用于"层级/同伴"关系：同事、同城、上下级
--    （员工表里 manager_id 指向同一张员工表，就是经典自连接场景）。
-- 2. 别被"自己连自己"吓到：它就是两份该表的引用在做嵌套循环。


-- ================================================================
-- 第 9 节：子查询——查询套查询
-- ================================================================
-- 问题：找出价格高于"全馆均价"的书。
-- 标量子查询：内层先算出一个值（均价 = 78.0），外层再比较。（预期 5 行）
SELECT title, price
FROM books
WHERE price > (SELECT AVG(price) FROM books)
ORDER BY price DESC;

-- 问题：找借过"科幻"类图书的读者姓名。
-- IN 子查询：内层给出一个 id 集合，外层做成员检查。（预期 4 行）
SELECT name
FROM readers
WHERE reader_id IN (
    SELECT l.reader_id
    FROM loans l
    JOIN books b ON b.book_id = l.book_id
    WHERE b.category = '科幻'
)
ORDER BY reader_id;

-- 问题：每本书和"它所在分类的均价"比，贵于分类均价的列出。
-- 相关子查询：内层引用了外层的 b1.category，外层每行都重算一次。
-- 像嵌套循环里带捕获的 lambda。（预期 6 行）
SELECT b1.title, b1.category, b1.price,
       (SELECT ROUND(AVG(b2.price), 2)
        FROM books b2
        WHERE b2.category = b1.category) AS category_avg
FROM books b1
WHERE b1.price > (SELECT AVG(b2.price) FROM books b2 WHERE b2.category = b1.category)
ORDER BY b1.category, b1.price DESC;

-- 要点：
-- 1. 子查询位置决定类型：出现在 WHERE 里比较单个值 = 标量子查询；
--    跟在 IN 后面 = 集合子查询；引用外层列 = 相关子查询。
-- 2. 相关子查询逻辑简单但可能很慢（每行重算）。现代数据库多数能
--    自动改写优化，但先写对、再谈优化。


-- ================================================================
-- 第 10 节：CTE（WITH）——给中间结果起名字
-- ================================================================
-- 问题：列出当前"借出未还"的明细（谁、哪本书、借走日期）。
-- WITH 定义一个具名临时结果集，可读性 ≈ 把长表达式拆成变量。
-- （预期 6 行）
WITH active_loans AS (
    SELECT loan_id, reader_id, book_id, loan_date
    FROM loans
    WHERE return_date IS NULL
)
SELECT al.loan_id, r.name AS reader, b.title AS book, al.loan_date
FROM active_loans al
JOIN readers r ON r.reader_id = al.reader_id
JOIN books   b ON b.book_id   = al.book_id
ORDER BY al.loan_date;

-- 问题：统计"至少借过 2 次"的读者：先聚合，再连接回读者表。
-- （预期 4 行：张伟3、李娜3、王强3、刘洋2）
WITH reader_stats AS (
    SELECT reader_id,
           COUNT(*)     AS borrow_times,
           MIN(loan_date) AS first_loan
    FROM loans
    GROUP BY reader_id
)
SELECT r.name, s.borrow_times, s.first_loan
FROM reader_stats s
JOIN readers r ON r.reader_id = s.reader_id
WHERE s.borrow_times >= 2
ORDER BY s.borrow_times DESC, r.name;

-- 要点：
-- 1. CTE 就是"查询内部的命名变量"，把一坨嵌套拆成从上往下读的流水线。
-- 2. CTE 只在当前这条语句里活着，不像临时表要建要删。
-- 3. 同一个 WITH 里可以定义多个 CTE（逗号分隔），还能互相引用。


-- ================================================================
-- 第 11 节：窗口函数——不压缩行的聚合
-- ================================================================
-- 普通聚合把 N 行压成 1 行；窗口函数给每一行附加一个"参考值"，
-- 行数不变。≈ 遍历 vector 时除了当前元素，还能看到"历史累计值"。

-- 问题：全馆图书按价格从高到低给每本书编号。
-- ROW_NUMBER() 发不重复的连续号 1..N。（12 行，第 1 名：深入理解计算机系统 139）
SELECT title, price,
       ROW_NUMBER() OVER (ORDER BY price DESC, title) AS rn
FROM books;

-- 问题：每个分类内部按价格排名。
-- RANK() 并列同名次、跳号；DENSE_RANK() 并列同名次、不跳号。
-- 本例各分类内部价格互不相同，所以两个结果一模一样；
-- 并列的差异留在最后一条全馆排名的语句里看。
SELECT category, title, price,
       RANK()       OVER (PARTITION BY category ORDER BY price DESC) AS rnk,
       DENSE_RANK() OVER (PARTITION BY category ORDER BY price DESC) AS dense_rnk
FROM books
ORDER BY category, rnk;

-- 问题：按价格升序的"滚动合计"（累计卖这么多钱）。
-- SUM() OVER (ORDER BY ...) 默认帧是从头到当前行。最后一行 = 总价 936。
SELECT title, price,
       SUM(price) OVER (ORDER BY price, title) AS running_total
FROM books;

-- 问题：全馆范围内找价格并列：按价格升序给名次，并列的号相同。
-- （39 元的《活着》《流浪地球》同为第 1 名，下一档直接跳到第 3 名——
--   这就是 RANK 的"跳号"；DENSE_RANK 则会给第 2 名）
SELECT title, price,
       RANK() OVER (ORDER BY price) AS rnk
FROM books
ORDER BY price;

-- 要点：
-- 1. OVER (PARTITION BY 分组列 ORDER BY 排序列)：
--    PARTITION 决定"和谁比"，ORDER 决定"按什么比"。
-- 2. ROW_NUMBER / RANK / DENSE_RANK 的区别：并列时给不给同号、
--    同号之后跳不跳号。Top-N 问题的三件套。
-- 3. 窗口函数在 SELECT 阶段算，所以 WHERE 里不能直接引用它的结果——
--    想过滤名次，用 CTE 包一层再筛。


-- ================================================================
-- 第 12 节：索引与视图——快起来，也清楚起来
-- ================================================================
-- 问题：查"3 号读者（王强）的借阅记录"。
-- 先看执行计划：EXPLAIN QUERY PLAN 告诉你数据库"打算怎么干活"。
EXPLAIN QUERY PLAN
SELECT * FROM loans WHERE reader_id = 3;
-- 现在它说 SCAN loans —— 全表扫描：16 行一行行看。
-- C++ 类比：std::find 线性遍历。数据少时无所谓，千万行时就是灾难。

-- 上面这条查询真实跑一下。（预期 3 行：loan_id 4、5、12）
SELECT loan_id, book_id, loan_date
FROM loans
WHERE reader_id = 3
ORDER BY loan_date;

-- 建索引：相当于给 reader_id 列额外维护一份"有序副本"。
-- 类比：在一堆无序 paper 上，另建一张按学号排序的目录（B 树）。
CREATE INDEX idx_loans_reader ON loans(reader_id);

-- 再看执行计划：SEARCH loans USING INDEX idx_loans_reader ——
-- 从"逐行扫描"变成"走树查找"。
EXPLAIN QUERY PLAN
SELECT * FROM loans WHERE reader_id = 3;

-- 视图：把一条常用查询"存"起来起个名，用的时候当表查。
-- 类比：保存下来的查询函数；不复制数据，每次查它都现场执行定义。
CREATE VIEW v_loan_detail AS
SELECT l.loan_id,
       r.name      AS reader,
       b.title     AS book,
       l.loan_date,
       l.return_date
FROM loans l
JOIN readers r ON r.reader_id = l.reader_id
JOIN books   b ON b.book_id   = l.book_id;

-- 问题：用视图查"未归还"明细，一行 JOIN 都不用写。（预期 6 行）
SELECT * FROM v_loan_detail WHERE return_date IS NULL ORDER BY loan_date;

-- 要点：
-- 1. 索引是"以写换读"：读快 N 倍，写变慢一点、占点磁盘。
--    不要"每列都建"，给 WHERE/JOIN/ORDER BY 里真正出现的列建。
-- 2. 普通视图不存数据（SQLite 的物化视图另说），它只是"命名的查询"。
--    视图里再套视图、套子查询都没问题，但别套太深，会看不清。
-- 3. EXPLAIN QUERY PLAN 是你观察"SQL 背后发生了什么"的第一台显微镜，
--    03_sql_cpp.cpp 会把它接到程序里成批打印。


-- ================================================================
-- 第 13 节：事务——要么全成，要么全不做
-- ================================================================
-- 问题：模拟"借书"这个动作的原子性。
-- SQLite 默认每条语句自己就是一个事务（自动提交）。
-- BEGIN ... COMMIT 把多条语句打包成一单。

-- 场景一：赵磊借《设计模式》，但中途发现系统异常，回滚。
BEGIN;
INSERT INTO loans (loan_id, reader_id, book_id, loan_date, return_date)
VALUES (17, 7, 12, '2024-08-01', NULL);
-- 事务内部看：已经插进去了（17 条）
SELECT COUNT(*) AS in_tx_count FROM loans;
ROLLBACK;   -- 撤销整个事务，INSERT 当没发生过
-- 回滚之后回到 16 条
SELECT COUNT(*) AS after_rollback FROM loans;

-- 场景二：这回流程顺利，正式提交。
BEGIN;
INSERT INTO loans (loan_id, reader_id, book_id, loan_date, return_date)
VALUES (17, 7, 12, '2024-08-01', NULL);
COMMIT;     -- 落盘，别人也能看见了
-- 提交之后是 17 条
SELECT COUNT(*) AS after_commit FROM loans;

-- 把演示数据清掉，恢复本文件开头的"16 条借阅"基线，
-- 免得反复 .read 时数据越滚越乱。
DELETE FROM loans WHERE loan_id = 17;
SELECT COUNT(*) AS final_count FROM loans;

-- 要点：
-- 1. 事务 = ACID：原子性（全做/全不做）、一致性（约束守住）、
--    隔离性（并发互不干扰）、持久性（提交掉电不丢）。
-- 2. ROLLBACK 不是"撤销上一条"，是撤销整个未提交事务。
-- 3. 程序里崩溃/断连而未提交的事务，数据库会自动回滚——
--    这就是"要么全成要么全不做"的兜底。


-- =====================================================================
--  跑完检查点：readers 10 行、books 12 行、loans 16 行、
--  索引 idx_loans_reader 与视图 v_loan_detail 存在。
--  接下来：02_schema_design.sql 讲"表该怎么设计"，
--          03_sql_cpp.cpp 带你看"SQL 背后发生了什么"。
-- =====================================================================
