#!/usr/bin/env python3
import sys

start_parsing = False
line_cnt = 0
for line in sys.stdin:
    line = line.strip()
    if not line:
        if line_cnt > 0:
            break
        continue
    # Expect lines like:
    # Table stats.flows has 1 actions: stats_count_1/count
    if (line.startswith("Action Parameter Metrics:")):
        start_parsing = True
        continue

    if start_parsing:
        line_cnt = line_cnt + 1
        if line_cnt >= 8 and (line_cnt % 3) == 2:
            print(line[:-1])
