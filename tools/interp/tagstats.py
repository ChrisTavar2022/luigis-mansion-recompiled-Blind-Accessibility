"""tagstats.py <LM_TAG_LOG file>: values by kind and category, and the most frequent untagged call chains
(docs/INTERPOLATION.md "Tagged interpolation", finding the next tag source)."""
import collections, re, sys
cat = collections.Counter()
untag = collections.Counter()
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().split("\n")
for l in lines:
    if l.startswith("==") or l.startswith("   view"):
        print(l)
        continue
    m = re.match(r"f\d+ #\d+ (\w+) (\w+) key (\w+) site (\w+) slot (\d+) src (\w+) depth (\d+) view (-?\d+)(?: t \S+ \S+ \S+)?(?: chain (\S+))?", l)
    if not m:
        continue
    kind, c, key, site, slot, src, depth, view, chain = m.groups()
    cat[(kind, c)] += 1
    if c == "none" and kind not in ("light", "proj"):
        ch = (chain or "").split("<")
        untag[(kind, "<".join(ch[:5]))] += 1
for k, v in sorted(cat.items()):
    print(v, k)
print("--- untagged by chain")
for k, v in untag.most_common(40):
    print(v, k)
