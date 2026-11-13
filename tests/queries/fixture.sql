-- Queries over tests/fixture_schema.sql; pg_tablegen turns them into
-- tests/generated/fixture_queries.hpp and querygen_test runs them.

-- name: user_by_id :one
SELECT id, username, email, balance, stake, profile, uid, mood, joined
FROM users
WHERE id = :id;

-- name: user_by_name :opt
SELECT id, username AS "login?" FROM users WHERE username = :username;

-- name: group_members :many
SELECT m.user_id, m.role, u.username, g.title
FROM memberships m
LEFT JOIN users u ON u.id = m.user_id
LEFT JOIN groups g ON g.id = m.group_id
WHERE m.group_id = :group_id /*@filter*/
ORDER BY m.user_id;

-- name: user_stats :one
SELECT count(*) AS "total!", max(points) AS top FROM users;

-- name: users_with_profile :many
SELECT id, profile
FROM users
WHERE profile @> :wanted::jsonb OR profile @> '{"vip": true}'
ORDER BY id;

-- name: set_active :exec
UPDATE users SET active = :active WHERE id = :id;

-- name: add_points :one
UPDATE users SET points = points + :delta WHERE id = :id
RETURNING old.points AS before, new.points AS after, points;

-- name: rollup_totals :many
SELECT group_id AS "group_id?", count(*) AS n FROM memberships GROUP BY ROLLUP (group_id) ORDER BY group_id;

-- name: left_join_false :many
SELECT u.id AS user_id, g.id AS group_id FROM users u LEFT JOIN groups g ON false ORDER BY u.id;

-- name: left_join_pruned :many
SELECT u.id AS user_id, m.id AS measurement_id
FROM users u
LEFT JOIN measurements m ON m.id = u.id AND m.day < '2000-01-01'
ORDER BY u.id;

-- name: add_group :one
INSERT INTO groups (id, title) VALUES (:id, :title) RETURNING id, title;
