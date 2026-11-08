-- The schema the migrations in match/ build and the ones in mismatch/ do not.
CREATE TABLE accounts (
    id      BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    email   TEXT NOT NULL UNIQUE,
    balance NUMERIC(12, 2) NOT NULL DEFAULT 0 CHECK (balance >= 0)
);

CREATE TABLE transfers (
    id      BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    source  BIGINT NOT NULL REFERENCES accounts (id),
    amount  NUMERIC(12, 2) NOT NULL,
    note    TEXT
);
