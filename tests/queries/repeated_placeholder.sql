-- A repeated placeholder names one parameter.
-- name: repeated_placeholder :many
SELECT id FROM users WHERE id = :id OR id = :id ORDER BY id;
