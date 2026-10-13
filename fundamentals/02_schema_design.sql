-- =====================================================================
--  02_schema_design.sql —— 表到底该怎么设计
--  老师傅带徒弟系列 · 第 1 关 · 第 2 课
-- =====================================================================
--
--  上一课学的是"怎么查"，这一课学的是"怎么建"。
--  顺序不能反：schema 定歪了，后面所有 SQL 都在给这个错误打补丁。
--
--  用 C++ 打比方：
--    schema ≈ 你的 struct / class 定义。
--    约束（CONSTRAINT）≈ 构造函数里的参数校验 + 编译期类型检查。
--    没有约束的表 ≈ 一堆裸 int 互相传，谁都能塞垃圾进去。
--
--  怎么跑（PowerShell）：
--     sqlite3 :memory: ".read 02_schema_design.sql"
-- 注意：本文件里有【一条故意写错的外键插入】（第 2 节 ★★★ 那行），
-- 它会打印报错但脚本会继续往下跑完。所以别加 -bail
-- （-bail 是"一遇错就退出"），否则到那儿就停了。
--
-- 正因为那条故意失败，跑完之后 sqlite3 的退出码是 1，不是 0。
-- 这是【预期现象】，不是脚本坏了。判断标准：
--   1) stderr 只有一条 "Runtime error near line 1xx: FOREIGN KEY constraint failed"
--   2) stdout 一直输出到最后一节的反范式查询，中间没断
-- 两条都满足，就是全对。
-- =====================================================================

-- SQLite 的外键强制执行默认关闭！每个连接都要开。
-- 忘了开会怎样：外键声明形同注释，垃圾数据照收不误。这是 SQLite 头号大坑。
PRAGMA foreign_keys = ON;

-- 顺手看看当前设置：1 = 开，0 = 关。
SELECT '外键检查是否开启: ' || (SELECT * FROM pragma_foreign_keys) AS fk_state;


-- ================================================================
-- 第 1 节：六种约束，各治一种脏数据
-- ================================================================
-- 建一张干净的表，把常用约束一次配齐。
DROP TABLE IF EXISTS members;

CREATE TABLE members (
    member_id  INTEGER PRIMARY KEY,                    -- ① 主键：唯一 + 非空，一行的身份证
    card_no    TEXT    NOT NULL UNIQUE,                -- ② 唯一键：业务编号，不许重复
    name       TEXT    NOT NULL,                       -- ③ 非空：这列必须有值
    gender     TEXT    NOT NULL DEFAULT '未知'
                        CHECK (gender IN ('男', '女', '未知')),  -- ④ 检查：枚举白名单
    credit     INTEGER NOT NULL DEFAULT 0
                        CHECK (credit >= 0 AND credit <= 100),   -- ④ 检查：数值范围
    phone      TEXT    UNIQUE,                         -- ② 唯一（允许 NULL，NULL 不参与唯一性判断）
    email      TEXT CHECK (email IS NULL OR email LIKE '%_@_%.__%'),  -- 粗糙的格式校验
    join_date  TEXT    NOT NULL DEFAULT (date('now'))  -- ⑤ 默认值：可以是表达式（要加括号）
);

-- 正常插入，全过。
INSERT INTO members (card_no, name, gender, credit, phone, email)
VALUES ('M001', '张伟', '男', 60, '13800000001', 'zhangwei@example.com');

INSERT INTO members (card_no, name, gender, credit, phone, email)
VALUES ('M002', '李娜', '女', 85, '13800000002', 'lina@example.com');

-- join_date 没给，走默认值。
INSERT INTO members (card_no, name) VALUES ('M003', '王强');
SELECT member_id, card_no, name, gender, credit, join_date FROM members;

-- 【约束是干嘛的】接下来这一批全都会失败——每一条都在替你拦住一次线上事故。
-- 想看单条报错，复制任意一条到 sqlite3 里回车即可。这里全部注释掉，
-- 免得跑整个文件时刷屏（老师傅把它们留在这儿当"约束速查表"）：

-- ⑥ 主键冲突（显式给了已存在的 id）：
--    INSERT INTO members (member_id, card_no, name) VALUES (1, 'M900', '重复ID');
--    → UNIQUE constraint failed: members.member_id

-- ② 唯一键冲突：
--    INSERT INTO members (card_no, name) VALUES ('M001', '重复卡号');
--    → UNIQUE constraint failed: members.card_no

-- ③ 非空违约：
--    INSERT INTO members (card_no) VALUES ('M004');
--    → NOT NULL constraint failed: members.name

-- ④ CHECK 违约（枚举白名单）：
--    INSERT INTO members (card_no, name, gender) VALUES ('M005', '赵六', 'X');
--    → CHECK constraint failed: gender IN ('男', '女', '未知')

-- ④ CHECK 违约（数值越界）：
--    INSERT INTO members (card_no, name, credit) VALUES ('M006', '孙七', 101);
--    → CHECK constraint failed: credit >= 0 AND credit <= 100

-- 要点：
-- 1. 主键 vs 唯一键：主键"必须有且只有一个"，唯一键"可以有几个、可以有 NULL"。
--    C++ 类比：主键 ≈ 容器的 key（身份），唯一索引 ≈ 额外维护的 std::set 查重。
-- 2. 约束写在数据库里，才叫"数据完整性"；写在业务代码里，
--    叫"祈祷每个入口都记得校验"。多一个入口就多一份风险。


-- ================================================================
-- 第 2 节：外键——表与表之间的引用关系
-- ================================================================
-- 一对多：一个部门（department）有多个员工（employee）。
-- "多"的那一方持外键 —— 就像 C++ 里从属对象持有父对象的 id / 指针，
-- 指向谁必须真实存在，否则就是悬垂引用。
DROP TABLE IF EXISTS employee;
DROP TABLE IF EXISTS department;

CREATE TABLE department (
    dept_id   INTEGER PRIMARY KEY,
    dept_name TEXT NOT NULL UNIQUE,
    floor_no  INTEGER CHECK (floor_no BETWEEN 1 AND 100)
);

CREATE TABLE employee (
    emp_id    INTEGER PRIMARY KEY,
    emp_name  TEXT    NOT NULL,
    dept_id   INTEGER NOT NULL REFERENCES department(dept_id)
              -- 外键 + 级联策略：部门没了，员工这一行跟着删
              ON DELETE CASCADE,
    mgr_id    INTEGER REFERENCES employee(emp_id),  -- 自引用外键：上级的 id 在同一张表里
    salary    REAL NOT NULL CHECK (salary > 0)
);

INSERT INTO department (dept_id, dept_name, floor_no) VALUES
    (10, '研发部', 3),
    (20, '测试部', 3),
    (30, '市场部', 5);

INSERT INTO employee (emp_id, emp_name, dept_id, mgr_id, salary) VALUES
    (1, '张伟', 10, NULL, 30000),   -- 顶层，没有上级
    (2, '李娜', 10, 1,    22000),
    (3, '王强', 10, 1,    21000),
    (4, '刘洋', 20, NULL, 26000),
    (5, '陈静', 20, 4,    19000),
    (6, '杨帆', 30, NULL, 28000),
    (7, '赵磊', 30, 6,    18000);

-- 连表看：员工 + 部门名 + 上级名（自连接）。
SELECT e.emp_name AS 员工, d.dept_name AS 部门, m.emp_name AS 上级, e.salary
FROM employee e
JOIN department d ON d.dept_id = e.dept_id
LEFT JOIN employee m ON m.emp_id = e.mgr_id     -- 上级可能没有，必须 LEFT JOIN
ORDER BY d.dept_id, e.emp_id;

-- ★★★ 故意违反外键（这一条会报错，是教学的一部分，别慌）★★★
-- 999 号部门不存在，插进来必须被拦住。报错形如：
--  Runtime error near line XX: FOREIGN KEY constraint failed
INSERT INTO employee (emp_id, emp_name, dept_id, salary)
VALUES (99, '幽灵员工', 999, 100);

-- 上面报错后，脚本继续往下跑——这条能查出来，说明数据库没崩、
-- 脏数据也没进去。
SELECT COUNT(*) AS 幽灵员工有没有进去_应当是0 FROM employee WHERE emp_id = 99;

-- CASCADE 演示：删掉"市场部"，它下面的员工行被自动删除。
BEGIN;
DELETE FROM department WHERE dept_id = 30;
SELECT COUNT(*) AS 删部门后市场部员工数_应当是0
FROM employee WHERE emp_id IN (7, 6);
ROLLBACK;   -- 演示完回滚，保持基线数据完整

-- 级联策略三兄弟（按需挑一个写在 REFERENCES 后面）：
--   ON DELETE CASCADE     父行删了，子行一起删
--   ON DELETE SET NULL    父行删了，子行的外键列置 NULL（该列必须允许 NULL）
--   ON DELETE RESTRICT    有子行就不许删父行（默认行为，最安全）
-- 要点：CASCADE 很爽也很危险——删一条主数据，可能连带蒸发上万行。
--       生产库里用 CASCADE 之前，先想清楚"我删的到底是什么"。


-- ================================================================
-- 第 3 节：三种关系怎么建表
-- ================================================================
-- 关系只有三种：一对一、一对多、多对多。建表方法固定，背下来。

-- 【一对多】上面 department / employee 就是。规则：
--   外键放在"多"的那一方。绝对不要在"一"的那一方存一列 id 列表。
SELECT d.dept_name, COUNT(e.emp_id) AS 人数
FROM department d
LEFT JOIN employee e ON e.dept_id = d.dept_id
GROUP BY d.dept_id, d.dept_name;

-- 【一对一】规则：外键 + UNIQUE（或者直接让主键同时是外键）。
-- 场景：员工的基本表很大，把不常用的档案信息拆出去，按需加载。
-- 类比 C++：把冷数据拆到另一个 struct，用 id 关联（数据局部性优化）。
DROP TABLE IF EXISTS emp_profile;
CREATE TABLE emp_profile (
    emp_id      INTEGER PRIMARY KEY REFERENCES employee(emp_id),  -- 主键=外键 ⇒ 一对一
    education   TEXT,
    entry_date  TEXT,
    emergency   TEXT
);
INSERT INTO emp_profile (emp_id, education, entry_date, emergency) VALUES
    (1, '硕士', '2019-07-01', '张三丰'),
    (2, '本科', '2021-03-15', '李母'),
    (4, '本科', '2020-09-01', '刘父');
SELECT COUNT(*) AS 档案条数 FROM emp_profile;

-- 【多对多】规则：中间表（junction table），两个外键 + 联合唯一。
-- 场景：一个员工会多种技能，一种技能多个员工会。
-- 千万别在员工表里存 'C++,Python,SQL' 这种逗号串——
-- 那等于放弃了数据库所有的查询、约束和索引能力。
DROP TABLE IF EXISTS emp_skill;
DROP TABLE IF EXISTS skill;

CREATE TABLE skill (
    skill_id  INTEGER PRIMARY KEY,
    skill_name TEXT NOT NULL UNIQUE
);
INSERT INTO skill (skill_id, skill_name) VALUES
    (1, 'C++'), (2, 'SQL'), (3, 'Python'), (4, 'Go'), (5, 'Rust');

CREATE TABLE emp_skill (
    emp_id    INTEGER NOT NULL REFERENCES employee(emp_id) ON DELETE CASCADE,
    skill_id  INTEGER NOT NULL REFERENCES skill(skill_id)  ON DELETE CASCADE,
    level     INTEGER NOT NULL DEFAULT 1 CHECK (level BETWEEN 1 AND 5),
    PRIMARY KEY (emp_id, skill_id)          -- 联合主键：同一组合只能出现一次
);
INSERT INTO emp_skill (emp_id, skill_id, level) VALUES
    (1, 1, 5), (1, 2, 4), (1, 3, 2),
    (2, 1, 3), (2, 2, 5),
    (3, 2, 2), (3, 4, 3),
    (4, 2, 4), (4, 3, 3),
    (5, 3, 4),
    (6, 2, 3),
    (7, 5, 2);

-- 典型多对多查询：谁会 SQL 且等级 >= 4？（预期 3 行：李娜5、张伟4、刘洋4）
SELECT e.emp_name, es.level
FROM employee e
JOIN emp_skill es ON es.emp_id = e.emp_id
JOIN skill s      ON s.skill_id = es.skill_id
WHERE s.skill_name = 'SQL' AND es.level >= 4
ORDER BY es.level DESC;

-- 反向查：每人掌握几项技能？（赵磊在 30 号部门，前面回滚过，还在）
SELECT e.emp_name, COUNT(es.skill_id) AS 技能数
FROM employee e
LEFT JOIN emp_skill es ON es.emp_id = e.emp_id
GROUP BY e.emp_id, e.emp_name
ORDER BY 技能数 DESC, e.emp_name;

-- 要点：
-- 1. 中间表除了两个外键，还可以带"关系自身的属性"（本例的 level）。
--    这一列既不属于员工也不属于技能，只属于"这条关系"。
-- 2. PRIMARY KEY (emp_id, skill_id) 一箭双雕：防重复 + 自带索引。


-- ================================================================
-- 第 4 节：三范式——同一个反例拆到底
-- ================================================================
-- 反例：图书馆的"一张大宽表"，很多老项目真是这么存的。
DROP TABLE IF EXISTS borrow_wide;
CREATE TABLE borrow_wide (
    reader_id   INTEGER,
    reader_name TEXT,
    reader_city TEXT,
    book_id     INTEGER,
    book_title  TEXT,
    category    TEXT,
    borrow_date TEXT,
    PRIMARY KEY (reader_id, book_id, borrow_date)   -- 勉强凑的"主键"
);

INSERT INTO borrow_wide VALUES
    (1, '张伟', '北京', 1, 'C++ Primer',         '编程',   '2024-01-05'),
    (1, '张伟', '北京', 4, '数据库系统概念',       '数据库', '2024-02-10'),
    (2, '李娜', '上海', 8, '三体',                '科幻',   '2024-02-15'),
    (2, '李娜', '上海', 8, '三体',                '科幻',   '2024-03-01'),
    (1, '张伟', '北京', 8, '三体',                '科幻',   '2024-05-01'),
    (3, '王强', '北京', 2, 'Effective C++',       '编程',   '2024-03-01');
SELECT * FROM borrow_wide;

-- 【病灶在哪】三条病，对着数据看：
--   插入异常：新书还没人借 → reader_id 为空 → 主键含它 → 这条书存不进去。
--   删除异常：删掉李娜最后一条记录 → "三体是科幻类"这条信息跟着蒸发。
--   更新异常：张伟搬家，得改他所有借阅行；改漏一行 → 数据自相矛盾。
--   空间浪费：'张伟'、'北京'、'三体'、'科幻' 反复存了十几遍。

-- 【1NF】第一范式：每格只放一个值（原子性），不能有"一格多值"。
-- 如果上面这张表再加一列 book_titles = 'C++ Primer,三体'，就违反 1NF。
-- 拆法：多值拆成多行。本表已经是 1NF 了（每格单值），病在更高层。

-- 【2NF】第二范式：在 1NF 之上，非主键列必须完整依赖【整个】联合主键，
-- 不能只依赖其中一部分（部分依赖 = 病）。
-- 看这张表：联合主键是 (reader_id, book_id, borrow_date)，但
--   reader_name / reader_city 只依赖 reader_id          ← 部分依赖！
--   book_title  / category    只依赖 book_id            ← 部分依赖！
-- 拆法：把"只依赖一部分"的列连同那部分主键单独立表。
DROP TABLE IF EXISTS w_reader;
DROP TABLE IF EXISTS w_book;
DROP TABLE IF EXISTS w_borrow;

CREATE TABLE w_reader (
    reader_id   INTEGER PRIMARY KEY,
    reader_name TEXT NOT NULL,
    reader_city TEXT NOT NULL
);
CREATE TABLE w_book (
    book_id   INTEGER PRIMARY KEY,
    book_title TEXT NOT NULL,
    category   TEXT NOT NULL
);
CREATE TABLE w_borrow (
    reader_id   INTEGER NOT NULL REFERENCES w_reader(reader_id),
    book_id     INTEGER NOT NULL REFERENCES w_book(book_id),
    borrow_date TEXT NOT NULL,
    PRIMARY KEY (reader_id, book_id, borrow_date)   -- 剩下的确实要三者才能定唯一
);

INSERT INTO w_reader SELECT DISTINCT reader_id, reader_name, reader_city FROM borrow_wide;
INSERT INTO w_book   SELECT DISTINCT book_id, book_title, category      FROM borrow_wide;
INSERT INTO w_borrow SELECT DISTINCT reader_id, book_id, borrow_date    FROM borrow_wide;
SELECT '1NF→2NF 拆完：读者' || (SELECT COUNT(*) FROM w_reader)
       || ' 行、图书' || (SELECT COUNT(*) FROM w_book)
       || ' 行、借阅' || (SELECT COUNT(*) FROM w_borrow) || ' 行';

-- 【3NF】第三范式：在 2NF 之上，非主键列不能依赖【其他非主键列】
-- （传递依赖 = 病）。口诀：任何非主键列"直接、全体"依赖主键，别拐两道弯。
-- 在上面 w_book 上加一列试试：category_avg_price（分类均价）。
--   book_id → category → category_avg_price   ← 传递依赖！
-- 病灶：编程类新增一本书，全表所有编程书的 avg_price 都得改。
-- 拆法：category 自己成为一张表，均价挂在它身上（甚至干脆不存，现算）。
DROP TABLE IF EXISTS n_reader;
DROP TABLE IF EXISTS n_category;
DROP TABLE IF EXISTS n_book;
DROP TABLE IF EXISTS n_borrow;

CREATE TABLE n_category (
    category_id   INTEGER PRIMARY KEY,
    category_name TEXT NOT NULL UNIQUE
);
CREATE TABLE n_book (
    book_id     INTEGER PRIMARY KEY,
    book_title  TEXT NOT NULL,
    category_id INTEGER NOT NULL REFERENCES n_category(category_id)
    -- 注意：category_avg_price 不在这里。它由 category 派生，
    -- 要么挪进 n_category，要么干脆别存，用查询现算（见 01 第 9 节）。
);
CREATE TABLE n_reader (
    reader_id   INTEGER PRIMARY KEY,
    reader_name TEXT NOT NULL,
    city_id     INTEGER NOT NULL          -- 连"城市"都可以再抽一张表，避免'北京'重复存
);
CREATE TABLE n_borrow (
    reader_id   INTEGER NOT NULL REFERENCES n_reader(reader_id),
    book_id     INTEGER NOT NULL REFERENCES n_book(book_id),
    borrow_date TEXT NOT NULL,
    PRIMARY KEY (reader_id, book_id, borrow_date)
);

INSERT INTO n_category VALUES (1,'编程'), (2,'数据库'), (3,'科幻');
INSERT INTO n_book     VALUES (1,'C++ Primer',1), (2,'Effective C++',1),
                              (4,'数据库系统概念',2), (8,'三体',3);
INSERT INTO n_reader   VALUES (1,'张伟',110000), (2,'李娜',310000), (3,'王强',110000);
INSERT INTO n_borrow   VALUES (1,1,'2024-01-05'), (1,4,'2024-02-10'),
                              (2,8,'2024-02-15'), (1,8,'2024-05-01');

-- 同一件事（查借阅明细），规范化之后靠 JOIN 拼回来：
SELECT r.reader_name, b.book_title, c.category_name, br.borrow_date
FROM n_borrow br
JOIN n_reader  r  ON r.reader_id = br.reader_id
JOIN n_book    b  ON b.book_id   = br.book_id
JOIN n_category c ON c.category_id = b.category_id
ORDER BY br.borrow_date;

-- 要点（老师傅版三范式人话）：
--   1NF：一格一个值。别在格子里塞逗号串。
--   2NF：主键是组合的，别的列必须依赖【整套】主键，不能只靠半截。
--   3NF：非主键列直接靠主键，别"靠别的非主键列拐弯靠主键"。
-- 一句总结：每个事实只说一遍（Don't Repeat Yourself），
--          而且要说在"它真正归属"的那张表里。


-- ================================================================
-- 第 5 节：什么时候该反范式
-- ================================================================
-- 规范化换来的是"不重复、好维护"，代价是"查询要 JOIN 好几张表"。
-- 读多写少、且某几个报表查询是绝对热点时，可以故意存点冗余。

-- 例子：热榜接口天天算"每本书被借多少次"，JOIN + COUNT 太慢，
-- 就在 books 上冗余一列 borrow_count，写入时顺手更新（用触发器或业务代码维护）。
DROP TABLE IF EXISTS books_denorm;
CREATE TABLE books_denorm (
    book_id      INTEGER PRIMARY KEY,
    title        TEXT NOT NULL,
    borrow_count INTEGER NOT NULL DEFAULT 0    -- 冗余列：可以从 loans 算出来，这里存下来
);
INSERT INTO books_denorm (book_id, title, borrow_count)
SELECT b.book_id, b.book_title,
       (SELECT COUNT(*) FROM n_borrow br WHERE br.book_id = b.book_id)
FROM n_book b;
SELECT * FROM books_denorm ORDER BY borrow_count DESC;

-- 常见反范式场景（都是有正当理由的"破例"）：
--   1. 报表/统计冗余列：如上面的 borrow_count、总金额、UV。
--   2. 历史快照：订单里的"商品价格"必须抄一份存下来。
--      商品后来改价，老订单的价格不能被"规范化"掉——
--      这不是冗余，这是"另一个事实"。
--   3. 跨库/跨服务：JOIN 不动，只能把字段复制过去。
--   4. 高频列表页：把详情页字段带一份，省一次 JOIN。
-- 反范式的铁律（背下来）：
--   ① 想清楚冗余列的"唯一真相来源"是谁，只允许一条路径写它；
--   ② 用触发器或事务保证它跟真相同步，别指望人记得更新；
--   ③ 只在被证明是瓶颈的地方反范式，别一上来就铺平大宽表。
--   （② 里的"触发器"，下一关会讲；这一关先记住原则。）


-- =====================================================================
--  本文件回顾：
--    约束 = 数据库层的 assert，主键/唯一/非空/检查/外键/默认值 六种。
--    关系 = 三种：一对一(外键+UNIQUE)、一对多(外键在"多"方)、多对多(中间表)。
--    范式 = 1NF 原子、2NF 消灭部分依赖、3NF 消灭传递依赖。
--    反范式 = 有正当理由、有同步机制、有性能证据时才做的破例。
--  下一步：跑 03_sql_cpp.cpp，看看这些 SQL 到了数据库里到底怎么被执行。
-- =====================================================================
