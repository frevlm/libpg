-- The old positional parameter line is rejected.
-- name: old_style :one
-- params: id
SELECT id FROM users WHERE id = :id;
