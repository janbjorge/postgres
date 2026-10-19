import re, sys
t = open(sys.argv[1]).read()
yes, no = [], []
for e in re.findall(r"\{(?:[^{}']|'[^']*')*\}", t):
    m = re.search(r"aggfnoid => '([^']+)'", e)
    if not m:
        continue
    k = re.search(r"aggkind => '(.)'", e)
    comb = re.search(r"aggcombinefn => '([^']+)'", e)
    ser = re.search(r"aggserialfn => '([^']+)'", e)
    tt = re.search(r"aggtranstype => '([^']+)'", e).group(1)
    (yes if comb else no).append((m.group(1), k.group(1) if k else 'n', tt, bool(ser)))
print("WITH combine:", len(yes))
print(" ".join(f"{n}[{tt}{'+ser' if s else ''}]" for n, k, tt, s in yes))
print("\nWITHOUT combine:", len(no))
print(" ".join(f"{n}<{k}>" for n, k, tt, s in no))
