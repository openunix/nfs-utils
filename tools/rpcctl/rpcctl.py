#!/usr/bin/python3
import argparse
import pathlib
import sys

with open("/proc/mounts", 'r') as f:
    mount = [ line.split()[1] for line in f if "sysfs" in line ]
    if len(mount) == 0:
        print("ERROR: sysfs is not mounted")
        sys.exit(1)

sunrpc = pathlib.Path(mount[0]) / "kernel" / "sunrpc"
if not sunrpc.is_dir():
    print("ERROR: sysfs does not have sunrpc directory")
    sys.exit(1)

parser = argparse.ArgumentParser()

def show_small_help(args):
    parser.print_usage()
    print("sunrpc dir:", sunrpc)
parser.set_defaults(func=show_small_help)

args = parser.parse_args()
args.func(args)
