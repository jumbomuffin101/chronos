#!/usr/bin/env python3
"""Run bounded, independent gateway fixtures and retain the raw trace deltas."""
import argparse
import csv
import io
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--samples', type=int, default=50)
parser.add_argument('--repeats', type=int, default=3)
parser.add_argument('--output', required=True)
args = parser.parse_args()
if not 1 <= args.samples <= 150 or not 1 <= args.repeats <= 10:
    parser.error('samples must be 1..150 and repeats 1..10')

with open(args.output, 'w', newline='', encoding='utf-8') as destination:
    writer = None
    for server in (0, 1):
        for client in (0, 1):
            for background in (10, 100):
                for fill in (0, 1):
                    for repeat in range(args.repeats):
                        command = ['./build/release/chronos_tcp_latency', str(server), str(client),
                                   str(background), str(fill), str(args.samples)]
                        print(f'server={server} client={client} n={background} fill={fill} '
                              f'run={repeat}', file=sys.stderr, flush=True)
                        try:
                            run = subprocess.run(command, text=True, capture_output=True,
                                                 timeout=120, check=True)
                        except subprocess.CalledProcessError as error:
                            sys.stderr.write(error.stderr)
                            raise
                        rows = list(csv.DictReader(io.StringIO(run.stdout)))
                        if len(rows) != args.samples:
                            raise RuntimeError('incomplete run')
                        for row in rows:
                            row['run'] = str(repeat)
                            if writer is None:
                                writer = csv.DictWriter(destination, fieldnames=['run', *[k for k in row if k != 'run']])
                                writer.writeheader()
                            writer.writerow(row)
                        destination.flush()
