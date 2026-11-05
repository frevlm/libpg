-- Covers every type pg_tablegen maps, plus keys, nullability and awkward names.
CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);

CREATE SEQUENCE ticket_seq;

CREATE TABLE users (
    id       INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username TEXT COLLATE ci NOT NULL UNIQUE,
    email    VARCHAR(100),
    nick     CHAR(8) NOT NULL DEFAULT '',
    active   BOOLEAN NOT NULL DEFAULT true,
    rank     SMALLINT NOT NULL DEFAULT 0,
    points   BIGINT NOT NULL DEFAULT 0,
    score    DOUBLE PRECISION,
    balance  NUMERIC(12, 2) NOT NULL DEFAULT 0,
    profile  JSONB NOT NULL DEFAULT '{}',
    raw      JSON,
    joined   TIMESTAMPTZ NOT NULL DEFAULT now(),
    birthday DATE,
    ratio    REAL,
    ip       INET
);

CREATE TABLE memberships (
    user_id  INTEGER NOT NULL REFERENCES users (id),
    group_id INTEGER NOT NULL,
    role     TEXT NOT NULL,
    since    TIMESTAMPTZ,
    PRIMARY KEY (group_id, user_id),
    UNIQUE (user_id, role),
    CHECK (role <> '')
);

CREATE TABLE events (
    ticket BIGINT NOT NULL DEFAULT nextval('ticket_seq'),
    type   TEXT NOT NULL,
    time   TIMESTAMPTZ NOT NULL,
    class  TEXT,
    name   TEXT
);

CREATE TABLE groups (
    id    SMALLINT PRIMARY KEY,
    title TEXT COLLATE ci NOT NULL
);

-- The script may manage its own transactions and schemas.
BEGIN;
CREATE TABLE after_commit_probe (id INTEGER PRIMARY KEY);
COMMIT;

CREATE DOMAIN positive_int AS INTEGER NOT NULL CHECK (VALUE > 0);

CREATE TABLE quotas (
    id    INTEGER PRIMARY KEY,
    limit_ positive_int,
    soft  positive_int NOT NULL
);

CREATE TABLE measurements (
    id   INTEGER NOT NULL,
    day  TIMESTAMPTZ NOT NULL,
    val  DOUBLE PRECISION,
    PRIMARY KEY (id, day)
) PARTITION BY RANGE (day);

CREATE TABLE measurements_2026 PARTITION OF measurements
    FOR VALUES FROM ('2026-01-01') TO ('2027-01-01');
