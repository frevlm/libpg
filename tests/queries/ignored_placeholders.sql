-- Colons inside strings, quoted identifiers, dollar quotes and comments are text.
-- name: ignored_placeholders :one
SELECT id AS ":id"
FROM users
WHERE username = ':name'
  AND E':escaped' = E':escaped'
  AND $$:dollar$$ = $$:dollar$$
  -- :line
  AND id > 0 /* :block */;
