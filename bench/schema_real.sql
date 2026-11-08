-- Real-application read shapes (HHHOJ, revlm). Loaded once into the `bench` database,
-- next to `submissions`. Deterministic; generate_series only.

CREATE TABLE oj_users (
    id int PRIMARY KEY,
    username text UNIQUE NOT NULL,
    usergroup int NOT NULL,
    email text,
    password_hash text NOT NULL,
    rating int NOT NULL,
    ac_num int NOT NULL
);
INSERT INTO oj_users
SELECT g, 'u' || g, CASE WHEN g % 50 = 0 THEN 2 ELSE 1 END, 'u' || g || '@x.org', md5(g::text), 1500 + g % 800, g % 300
FROM generate_series(1, 10000) g;
CREATE INDEX oj_users_usergroup ON oj_users (usergroup);

CREATE TABLE oj_sessions (
    token_hash text PRIMARY KEY,
    username text NOT NULL,
    expires timestamptz NOT NULL
);
INSERT INTO oj_sessions
SELECT 'tok' || g, 'u' || g, now() + interval '30 days' FROM generate_series(1, 10000) g;

CREATE TABLE oj_msg (
    id int PRIMARY KEY,
    sender text NOT NULL,
    receiver text NOT NULL,
    message text NOT NULL,
    send_time timestamptz NOT NULL,
    read_time timestamptz
);
INSERT INTO oj_msg
SELECT g, 'u' || ((g * 7) % 10000 + 1), 'u' || (g % 10000 + 1), repeat('m', 80),
       timestamptz '2026-01-01 00:00:00+00' + g * interval '1 minute',
       CASE WHEN g % 10 = 0 THEN NULL ELSE timestamptz '2026-01-01 00:00:00+00' + g * interval '1 minute' END
FROM generate_series(1, 200000) g;
CREATE INDEX oj_msg_unread ON oj_msg (receiver) WHERE read_time IS NULL;

CREATE TABLE oj_sysmsg (
    id int PRIMARY KEY,
    title text NOT NULL,
    content text NOT NULL,
    receiver text NOT NULL,
    send_time timestamptz NOT NULL,
    read_time timestamptz
);
INSERT INTO oj_sysmsg
SELECT g, 'title ' || g, repeat('c', 200), 'u' || (g % 10000 + 1),
       timestamptz '2026-01-01 00:00:00+00' + g * interval '1 minute',
       CASE WHEN g % 10 = 0 THEN NULL ELSE timestamptz '2026-01-01 00:00:00+00' + g * interval '1 minute' END
FROM generate_series(1, 100000) g;
CREATE INDEX oj_sysmsg_unread ON oj_sysmsg (receiver) WHERE read_time IS NULL;

CREATE TABLE oj_groups (name text PRIMARY KEY);
INSERT INTO oj_groups SELECT 'g' || g FROM generate_series(1, 50) g;

CREATE TABLE oj_group_members (
    group_name text,
    username text,
    PRIMARY KEY (username, group_name)
);
INSERT INTO oj_group_members
SELECT DISTINCT 'g' || gn, 'u' || g
FROM generate_series(1, 10000) g,
     LATERAL (VALUES (g % 50 + 1), ((g * 3) % 50 + 1)) v(gn);

CREATE TABLE oj_problems (
    id int PRIMARY KEY,
    title text NOT NULL,
    statement text NOT NULL
);
INSERT INTO oj_problems SELECT g, 'Problem ' || g, repeat(md5(g::text), 600) FROM generate_series(1, 500) g;

CREATE TABLE oj_queue (
    id bigint PRIMARY KEY,
    status text NOT NULL,
    hack_of bigint,
    judge_time timestamptz
);
INSERT INTO oj_queue
SELECT g, 'Accepted', CASE WHEN g % 100 = 0 THEN g - 1 ELSE NULL END, now() FROM generate_series(1, 100000) g;
CREATE INDEX oj_queue_waiting ON oj_queue (id) WHERE status = 'Waiting' AND hack_of IS NULL;
CREATE INDEX oj_queue_rejudge ON oj_queue (id) WHERE status = 'Waiting Rejudge' AND hack_of IS NULL;
CREATE INDEX oj_queue_judged_waiting ON oj_queue (id) WHERE status = 'Judged, Waiting';
CREATE INDEX oj_queue_hack ON oj_queue (id) WHERE hack_of IS NOT NULL AND status IN ('Waiting', 'Waiting Rejudge');

CREATE TABLE oj_custom_tests (
    id bigint PRIMARY KEY,
    judge_time timestamptz
);
INSERT INTO oj_custom_tests SELECT g, now() FROM generate_series(1, 20000) g;
CREATE INDEX oj_custom_tests_waiting ON oj_custom_tests (id) WHERE judge_time IS NULL;

CREATE TABLE oj_judgers (
    name text PRIMARY KEY,
    password_hash text NOT NULL
);
INSERT INTO oj_judgers SELECT 'j' || g, md5('j' || g) FROM generate_series(1, 4) g;

-- The dense, append-only log: each user's rows are scattered over the whole heap.
CREATE TABLE rv_requests (
    id bigint PRIMARY KEY,
    time timestamptz NOT NULL,
    user_id bigint NOT NULL,
    token_id bigint NOT NULL,
    channel_id int NOT NULL,
    model text NOT NULL,
    endpoint text NOT NULL,
    request_id text NOT NULL,
    response_id text,
    status int NOT NULL,
    input_tokens int NOT NULL,
    output_tokens int NOT NULL,
    cache_read_tokens int NOT NULL,
    latency_ms int NOT NULL,
    usd numeric(18,6) NOT NULL,
    error text
);
INSERT INTO rv_requests
SELECT g,
       timestamptz '2026-08-01 00:00:00+00' + (g * interval '90 days') / 4000000,
       (g::bigint * 2654435761) % 100 + 1,
       ((g::bigint * 2654435761) % 100 + 1) * 10 + g % 3,
       g % 20,
       (ARRAY['gpt-5','claude-opus','claude-sonnet','gemini','deepseek','qwen','glm','kimi'])[g % 8 + 1],
       '/v1/chat/completions',
       md5(g::text),
       'resp_' || md5((g + 1)::text),
       200,
       g % 4000,
       g % 1500,
       g % 800,
       g % 30000,
       (g % 5000)::numeric / 100000,
       CASE WHEN g % 97 = 0 THEN 'upstream timeout' ELSE NULL END
FROM generate_series(1, 4000000) g;
CREATE INDEX rv_requests_user ON rv_requests (user_id, id);
CREATE INDEX rv_requests_time ON rv_requests (time);

VACUUM ANALYZE oj_users;
VACUUM ANALYZE oj_sessions;
VACUUM ANALYZE oj_msg;
VACUUM ANALYZE oj_sysmsg;
VACUUM ANALYZE oj_groups;
VACUUM ANALYZE oj_group_members;
VACUUM ANALYZE oj_problems;
VACUUM ANALYZE oj_queue;
VACUUM ANALYZE oj_custom_tests;
VACUUM ANALYZE oj_judgers;
VACUUM ANALYZE rv_requests;
