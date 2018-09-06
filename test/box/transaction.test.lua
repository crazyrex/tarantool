env = require('test_run')
test_run = env.new()
test_run:cmd("setopt delimiter ';'")
-- empty transaction - ok
box.begin() box.commit();
-- gh-3518 no active transaction - false
box.is_in_txn();
-- start transaction
box.begin();
-- active transaction - true
box.is_in_txn();
-- close transaction
box.commit();
-- double begin
box.begin() box.begin();
-- no active transaction since exception rolled it back
box.commit();
-- double commit - implicit start of transaction
box.begin() box.commit() box.commit();
-- commit if not started - implicit start of transaction
box.commit();
-- rollback if not started - ok
box.rollback()
-- double rollback - ok
box.begin() box.rollback() box.rollback();
-- rollback of an empty trans - ends transaction
box.begin() box.rollback();
-- no current transaction - implicit begin
box.commit();
fiber = require('fiber');
function sloppy()
    box.begin()
end;
f = fiber.create(sloppy);
-- when the sloppy fiber ends, its session has an active transction
-- ensure it's rolled back automatically
while f:status() ~= 'dead' do fiber.sleep(0) end;
-- transactions and system spaces
box.begin() box.schema.space.create('test');
box.rollback();
box.begin() box.schema.func.create('test');
box.rollback();
box.begin() box.schema.user.create('test');
box.rollback();
box.begin() box.schema.user.grant('guest', 'read', 'space', '_priv');
box.rollback();
box.begin() box.space._user:delete{box.schema.GUEST_ID};
box.rollback();
box.begin() box.space._space:delete{box.schema.CLUSTER_ID};
box.rollback();
box.begin() box.space._sequence:insert{1, 1, 'test', 1, 1, 2, 1, 0, false};
box.rollback();
box.begin() box.space._schema:insert{'test'};
box.rollback();
box.begin() box.space._cluster:insert{123456789, 'abc'};
box.rollback();
s = box.schema.space.create('test');
box.begin() index = s:create_index('primary');
box.rollback();
index = s:create_index('primary');
t = nil
function multi()
    box.begin()
    s:auto_increment{'first row'}
    s:auto_increment{'second row'}
    t = s:select{}
    box.commit()
end;
multi();
t;
s:select{};
s:truncate();
function multi()
    box.begin()
    s:auto_increment{'first row'}
    s:auto_increment{'second row'}
    t = s:select{}
    box.rollback()
end;
multi();
t;
s:select{};
function multi()
    box.begin()
    s:insert{1, 'first row'}
    pcall(s.insert, s, {1, 'duplicate'})
    t = s:select{}
    box.commit()
end;
multi();
t;
s:select{};
s:truncate();
--
-- Test that fiber yield causes a transaction rollback
-- but only if the transaction has changed any data
--
-- Test admin console
box.begin();
-- should be ok - active transaction, and we don't
-- know, maybe it will use vinyl engine, which
-- may support yield() in the future, so we don't roll
-- back a transction with no statements.
box.commit();
box.begin() s:insert{1, 'Must be rolled back'};
-- nothing - the transaction was rolled back
while s:get{1} ~= nil do fiber.sleep(0) end
-- nothing to commit because of yield
box.commit();
-- Test background fiber
--
function sloppy()
    box.begin()
    s:insert{1, 'From background fiber'}
end;
f = fiber.create(sloppy);
while f:status() == 'running' do
    fiber.sleep(0)
end;
-- When the sloppy fiber ends, its session has an active transction
-- It's rolled back automatically
s:select{};
t = nil;
function sloppy()
    box.begin()
    s:insert{1, 'From background fiber'}
    fiber.sleep(0)
    pcall(box.commit)
    t = s:select{}
end;
f = fiber.create(sloppy);
while f:status() ~= 'dead' do
    fiber.sleep(0)
end;
t;
s:select{};
s:drop();
test_run:cmd("setopt delimiter ''");
test = box.schema.space.create('test')
tindex = test:create_index('primary')
box.begin() test:insert{1} box.rollback()
test:select{1}
box.begin() test:insert{1} box.commit()
test:select{1}

--
-- Test statement-level rollback
--
box.space.test:truncate()
function insert(a) box.space.test:insert(a) end
test_run:cmd("setopt delimiter ';'")
function dup_key()
    box.begin()
    box.space.test:insert{1}
    local status, _ = pcall(insert, {1})
    if not status then
        if box.error.last().code ~= box.error.TUPLE_FOUND then
            box.error()
        end
        box.space.test:insert{2}
    end
    box.commit()
end;
test_run:cmd("setopt delimiter ''");
dup_key()
box.space.test:select{}
--
-- transaction which uses a non-existing space (used to crash in
-- rollbackStatement)
--
test = box.space.test
box.space.test:drop()
status, message = pcall(function() box.begin() test:put{1} test:put{2} box.commit() end)
status
message:match('does not exist')
if not status then box.rollback() end
test = nil

test_run:cmd("setopt delimiter ';'")
function tx_limit(n)
    box.begin()
    for i=0,n do
        box.space.test:insert{i}
    end
    box.commit()
end;
test_run:cmd("setopt delimiter ''");
_ = box.schema.space.create('test');
_ = box.space.test:create_index('primary');
tx_limit(10000)
box.space.test:len()
box.space.test:drop()

--
-- gh-1638: box.rollback on a JIT-ed code path crashes LuaJIT
-- (ffi call + yield don't mix well, rollback started to yield recently)
-- Note: don't remove gh_1638(), it's necessary to trigger JIT-compilation.
--
function gh_1638() box.begin(); box.rollback() end
for i = 1, 1000 do fiber.create(function() gh_1638() end) end

--
--gh-818 add atomic()
--
space = box.schema.space.create('atomic')
index = space:create_index('primary')
test_run:cmd("setopt delimiter ';'")

function args(...)
    return 'args', ...
end;
box.atomic(args, 1, 2, 3, 4, 5);

function tx()
    space:auto_increment{'first row'}
    space:auto_increment{'second row'}
    return space:select{}
end;
box.atomic(tx);

function tx_error(space)
    space:auto_increment{'third'}
    space:auto_increment{'fourth'}
    error("some error")
end;
box.atomic(tx_error, space);

function nested(space)
    box.begin()
end;
box.atomic(nested, space);

function rollback(space)
    space:auto_increment{'fifth'}
    box.rollback()
end;
box.atomic(rollback, space);

test_run:cmd("setopt delimiter ''");
space:select{}

space:drop()

--
-- gh-3528: sysview engine shouldn't participate in transaction control
--
space = box.schema.space.create('test', {id = 9000})
index = space:create_index('primary')

test_run:cmd("setopt delimiter ';'")
box.begin()
space:auto_increment{box.space._vspace:get{space.id}}
space:auto_increment{box.space._vindex:get{space.id, index.id}}
box.commit();
test_run:cmd("setopt delimiter ''");

space:select()
space:drop()
