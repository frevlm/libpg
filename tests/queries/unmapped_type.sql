-- A type outside libpg's codec table is an error naming the column and the type.
-- name: unmapped_type :one
SELECT '127.0.0.1'::inet AS addr, 1::numeric AS amount;
