# statement_timeout 默认 30 秒

池的 `statement_timeout` 默认 30 秒，而不是 PG 的 0（不限）。库的默认值应该能防事故。
这会改变使用方现有行为，所以两个使用方迁移时先开一周 `log_min_duration_statement` 找出长查询，
给它们显式设置超时，而不是让行为悄悄改变。
