import glob, json, statistics, sys
d = sys.argv[1]; bins = sys.argv[2:]
data = {}
for b in bins:
    for f in glob.glob(f"{d}/{b}_r*.json"):
        for r in json.load(open(f))["results"]:
            data.setdefault((r["engine"], r["workload"]), {}).setdefault(b, []).extend(r["throughput_ns_per_op_runs"])
base = bins[0]
print(f"{'engine/workload':20}" + "".join(f"{b:>22}" for b in bins))
for key in sorted(data):
    row = data[key]; m0 = statistics.median(row[base])
    cells = []
    for b in bins:
        v = row[b]; m = statistics.median(v)
        spread = (max(v) - min(v)) / m * 100
        cells.append(f"{m:6.2f} ({(m/m0-1)*100:+5.1f}%) ±{spread:3.0f}%" if b != base else f"{m:6.2f}          ±{spread:3.0f}%")
    print(f"{key[0]+'/'+key[1]:20}" + "".join(f"{c:>22}" for c in cells))
print(f"(median ns/cmd over {len(row[base])} runs each; ± = max-min spread / median)")
