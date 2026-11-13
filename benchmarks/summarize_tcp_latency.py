#!/usr/bin/env python3
"""Summarize retained trace deltas; percentile values are descriptive only."""
import csv
import math
import statistics
import sys
from collections import defaultdict

if len(sys.argv) != 2:
    raise SystemExit('usage: summarize_tcp_latency.py CSV')
groups = defaultdict(list)
with open(sys.argv[1], newline='', encoding='utf-8') as source:
    for row in csv.DictReader(source):
        groups[(row['server_nd'], row['client_nd'], row['background'], row['fill'])].append(row)

def percentile(values, fraction):
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)] / 1000

metrics = ('total_ns', 'gateway_ns', 'queue_ns', 'transaction_ns', 'report_queue_ns',
           'write_wait_ns', 'delivery_ns')
print('server_nd,client_nd,background,fill,count,metric,p50_us,p95_us,p99_us')
for key, rows in sorted(groups.items()):
    for metric in metrics:
        values = [(int(row['total_ns']) - int(row['send_ns']) - int(row['network_in_ns']) -
                   int(row['delivery_ns'])) if metric == 'gateway_ns' else int(row[metric])
                  for row in rows]
        print(*key, len(rows), metric, f'{statistics.median(values) / 1000:.3f}',
              f'{percentile(values, .95):.3f}', f'{percentile(values, .99):.3f}', sep=',')
