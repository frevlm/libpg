-- A bare $1 is rejected.
-- name: bare_dollar :one
SELECT id FROM users WHERE id = $1;
