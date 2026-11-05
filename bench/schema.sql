-- Benchmark schema: 100k seeded accounts and an empty events log.
CREATE TABLE accounts (
  id         bigint PRIMARY KEY,
  balance    numeric(18,6) NOT NULL DEFAULT 0,
  name       text NOT NULL,
  updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE events (
  id         bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  account_id bigint NOT NULL,
  amount     numeric(18,6) NOT NULL,
  at         timestamptz NOT NULL DEFAULT now(),
  note       text
);

INSERT INTO accounts (id, balance, name)
SELECT g, round((random() * 10000)::numeric, 6), 'account-' || g
FROM generate_series(1, 100000) AS g;

VACUUM ANALYZE accounts;
