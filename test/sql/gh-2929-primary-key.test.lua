test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
box.sql.execute('pragma sql_default_engine=\''..engine..'\'')
box.sql.execute('pragma interactive_mode=0;')

-- All tables in SQL are now WITHOUT ROW ID, so if user
-- tries to create table without a primary key, an appropriate error message
-- should be raised. This tests checks it.

box.cfg{}

box.sql.execute("CREATE TABLE t1(a INT PRIMARY KEY, b UNIQUE)")
box.sql.execute("CREATE TABLE t2(a UNIQUE, b)")

box.sql.execute("CREATE TABLE t3(a)")
box.sql.execute("CREATE TABLE t4(a, b)")
box.sql.execute("CREATE TABLE t5(a, b UNIQUE)")

box.sql.execute("DROP TABLE t1")

--
-- gh-3522: invalid primary key name
--
box.sql.execute("CREATE TABLE tx (a INT, PRIMARY KEY (b));")
