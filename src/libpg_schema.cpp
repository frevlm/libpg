#include "libpg_schema.hpp"

#include "sha256.hpp"

namespace pg::detail {

std::string watch_trigger_base(std::string_view table, std::string_view column) {
    return "libpg_w_" + sha256_hex(std::string(table) + "." + std::string(column)).substr(0, 16);
}

std::string_view libpg_schema_sql() {
    static constexpr std::string_view sql = R"SQL(
CREATE SCHEMA IF NOT EXISTS libpg;

CREATE TABLE IF NOT EXISTS libpg.migrations (
    name       text PRIMARY KEY,
    sha256     text NOT NULL,
    applied_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS libpg.flush_receipts (
    id         uuid PRIMARY KEY,
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS flush_receipts_created_at ON libpg.flush_receipts (created_at);

CREATE OR REPLACE FUNCTION libpg.watch_notify() RETURNS trigger LANGUAGE plpgsql AS $fn$
DECLARE
    col     text := TG_ARGV[0];
    keys    text[] := '{}';
    key_arr text;
    from_   text;
    head    text;
    budget  int;
    buf     text;
    r       record;
BEGIN
    FOR i IN 1 .. TG_NARGS - 1 LOOP
        keys := keys || TG_ARGV[i];
    END LOOP;
    head := '{"t":' || to_json(TG_TABLE_NAME)::text || ',"c":' || to_json(col)::text;
    IF TG_OP = 'TRUNCATE' THEN
        PERFORM pg_notify('libpg_watch', head || ',"all":true}');
        RETURN NULL;
    END IF;
    head := head || ',"k":[';

    IF TG_OP = 'INSERT' THEN
        SELECT 'array_to_json(ARRAY[' || string_agg(format('t.%I::text', k), ',') || '])::text'
          INTO key_arr FROM unnest(keys) AS k;
        from_ := 'FROM libpg_new t';
    ELSIF TG_OP = 'DELETE' THEN
        SELECT 'array_to_json(ARRAY[' || string_agg(format('t.%I::text', k), ',') || '])::text'
          INTO key_arr FROM unnest(keys) AS k;
        from_ := 'FROM libpg_old t';
    ELSE
        SELECT 'array_to_json(ARRAY[' || string_agg(format('coalesce(t.%1$I::text, o.%1$I::text)', k), ',') || '])::text'
          INTO key_arr FROM unnest(keys) AS k;
        from_ := 'FROM libpg_new t FULL JOIN libpg_old o ON ' ||
                 (SELECT string_agg(format('t.%1$I = o.%1$I', k), ' AND ') FROM unnest(keys) AS k) ||
                 format(' WHERE t.%1$I IS NULL OR o.%1$I IS NULL OR t.%2$I IS DISTINCT FROM o.%2$I', keys[1], col);
    END IF;

    budget := 7000 - octet_length(head) - 2;
    FOR r IN EXECUTE 'SELECT ' || key_arr || ' AS k ' || from_ LOOP
        IF buf IS NOT NULL AND octet_length(buf) + 1 + octet_length(r.k) > budget THEN
            PERFORM pg_notify('libpg_watch', head || buf || ']}');
            buf := NULL;
        END IF;
        buf := CASE WHEN buf IS NULL THEN r.k ELSE buf || ',' || r.k END;
    END LOOP;
    IF buf IS NOT NULL THEN
        PERFORM pg_notify('libpg_watch', head || buf || ']}');
    END IF;
    RETURN NULL;
END
$fn$;
)SQL";
    return sql;
}

}  // namespace pg::detail
