#!/usr/bin/env python3
"""Exercise the compact record limits and each reason to use wide records."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from run_fixtures import SCHEMAS, load_actual, load_expected, rows


def write_input(data, window_start, categories):
    for folder in ("events", "clicks", "links"):
        (data / folder).mkdir(parents=True)
    (data / "manifest.json").write_text(
        f'{{"window_start":"{window_start}T00:00:00Z","window_end":"2026-10-15T00:00:00Z"}}'
    )
    products = [f"p{i:03},category{i:03}\n" for i in range(categories)]
    (data / "products.csv").write_text("product_id,category\n" + "".join(products))
    (data / "campaigns.csv").write_text("campaign_id,channel\nc_1,search\nc_2,social\n")
    (data / "links" / "part.csv").write_text("user_id_a,user_id_b\nlong-device-id,u\n")
    (data / "events" / "part.csv").write_text(
        "event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents\n"
        "e1,long-device-id,2026-10-03T00:09:00Z,2026-10-03T00:09:00Z,view,p000,\n"
        f"e2,u,2026-10-03T00:29:00Z,2026-10-03T00:29:00Z,purchase,p{categories - 1:03},500\n"
        "e3,u,2026-10-03T00:32:00Z,2026-10-03T00:32:00Z,purchase,p000,300\n"
        "e4,v,2026-10-03T00:17:00Z,2026-10-03T00:17:00Z,purchase,missing,10000000\n"
    )
    (data / "clicks" / "part.csv").write_text(
        "click_id,user_id,campaign_id,ts\n"
        "k2,long-device-id,c_2,2026-10-02T00:00:00Z\n"
        "k1,u,c_1,2026-10-03T00:09:00Z\n"
    )


def compare_case(command, window_start, categories):
    with tempfile.TemporaryDirectory() as folder:
        base = Path(folder)
        data, out = base / "data", base / "out"
        write_input(data, window_start, categories)
        expected = {
            "user_daily": (
                "user_id,day,sessions,events,revenue_cents,top_category\n"
                f"long-device-id,2026-10-03,1,3,800,category{categories - 1:03}\n"
                "v,2026-10-03,1,1,10000000,unknown\n"
            ),
            "campaign_daily": (
                "campaign_id,channel,day,sessions,purchasing_sessions,revenue_cents,assisted_sessions\n"
                "c_1,search,2026-10-03,1,1,800,1\n"
                "c_2,social,2026-10-03,0,0,0,1\n"
                "organic,organic,2026-10-03,1,1,10000000,0\n"
            ),
        }
        env = dict(os.environ, DATA_DIR=str(data), OUTPUT_DIR=str(out))
        subprocess.run([*command, data, out], env=env, check=True)
        for table, schema in SCHEMAS.items():
            csv = base / (table + ".csv")
            csv.write_text(expected[table])
            if rows(load_actual(out / table, schema)) != rows(load_expected(csv, schema)):
                raise SystemExit(f"{table} differs for {window_start} with {categories} categories")


def main(command):
    if not command:
        raise SystemExit("usage: check_record_layouts.py COMMAND [ARG...]")
    for start in ("2026-10-01", "2026-08-01"):
        for categories in (255, 256):
            compare_case(command, start, categories)
    print("4/4 record layout checks passed")


if __name__ == "__main__":
    main(sys.argv[1:])
