-- `::` is a cast after a placeholder, not another placeholder.
-- name: cast_placeholder :many
SELECT id FROM users WHERE profile @> :wanted::jsonb ORDER BY id;
