#!/usr/bin/env python3
import argparse, random

def ring(n, core_rate, core_delay, spur_rate, spur_delay, tree=False):
    edges = []
    # core edges
    for i in range(n-1):
        edges.append((f"s{i}", f"s{i+1}", "core", core_rate, core_delay))
    if not tree and n > 2:
        edges.append((f"s{n-1}", "s0", "core", core_rate, core_delay))
    # spurs (one host per switch)
    for i in range(n):
        edges.append((f"h{i}", f"s{i}", "spur", spur_rate, spur_delay))
    return edges

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--preset", choices=["ring","line"], default="ring")
    p.add_argument("--n", type=int, default=8)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--tree", action="store_true", help="break loops (use line)")
    p.add_argument("--coreRate", default="10Gbps"); p.add_argument("--coreDelay", default="0.5ms")
    p.add_argument("--spurRate", default="1Gbps");  p.add_argument("--spurDelay", default="0.2ms")
    p.add_argument("--out", required=True)
    args = p.parse_args()
    random.seed(args.seed)

    if args.preset == "ring" and not args.tree:
        edges = ring(args.n, args.coreRate, args.coreDelay, args.spurRate, args.spurDelay, tree=False)
    else:
        edges = ring(args.n, args.coreRate, args.coreDelay, args.spurRate, args.spurDelay, tree=True)

    with open(args.out, "w") as f:
        print("# u,v,kind,rate,delay", file=f)
        for u,v,k,r,d in edges:
            print(f"{u},{v},{k},{r},{d}", file=f)
