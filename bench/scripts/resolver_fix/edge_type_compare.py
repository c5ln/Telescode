import sqlite3, subprocess, sys, shutil, collections
# 이름 해석 개편 후 재측정 — 실제 실행 (2026-11-01): 인스턴스 6개를 두 pin으로 스캔해 파일 그래프 간선을 종류별로 비교 (§13.1).
# 사용: bench/.venv/bin/python bench/scripts/resolver_fix/edge_type_compare.py <빈 작업 폴더>
from pathlib import Path
sys.path.insert(0,'/home/cheonsh/Telescode')
from bench.features.extract import _file_level_edges, _imports_are_file_ids, _infer_package_roots, _resolve_module
import pandas as pd
from bench.collect.swebench import load_instances
W=Path(sys.argv[1])
B='/home/cheonsh/Telescode/bench/bin'
def typed(db):
    c=sqlite3.connect(db); fids={r[0] for r in c.execute('select file_id from file')}
    out=collections.defaultdict(set)
    for s,t,ty in c.execute("select distinct substr(source_id,1,instr(source_id||'::','::')-1), substr(target_id,1,instr(target_id||'::','::')-1), link_type from link where link_type in ('CALLS','INHERITS')"):
        if s!=t and s in fids and t in fids: out[ty].add((s,t))
    rows=c.execute("select distinct substr(source_id,1,instr(source_id||'::','::')-1), target_id from link where link_type='IMPORTS'").fetchall()
    if _imports_are_file_ids(c):
        out['IMPORTS']={(s,t) for s,t in rows if s!=t and s in fids and t in fids}
    else:
        roots=_infer_package_roots(fids)
        for s,m in rows:
            if s in fids:
                t=_resolve_module(m,s,fids,roots)
                if t and t!=s: out['IMPORTS'].add((s,t))
    allp=set().union(*out.values()); c.close(); return out, allp
res=[]
for repo,d in [('pydata/xarray','xarray'),('pytest-dev/pytest','pytest')]:
    inst=load_instances(repo)
    for _,r in inst.iloc[[0,len(inst)//2,len(inst)-1]].iterrows():
        tree=W/r.instance_id; shutil.rmtree(tree,ignore_errors=True); tree.mkdir(parents=True)
        subprocess.run(f"git -C /home/cheonsh/Telescode/bench/repos/{d} archive {r.base_commit} | tar -x -C {tree}",shell=True,check=True)
        o=W/'o.db'; n=W/'n.db'
        for p in (o,n): p.unlink(missing_ok=True)
        subprocess.run([f'{B}/e2501cb/TelescodeScanner',str(tree),str(o)],capture_output=True,check=True)
        subprocess.run([f'{B}/93f9cf2/TelescodeHeadless','scan',str(tree),str(n)],capture_output=True,check=True)
        to,ao=typed(o); tn,an=typed(n)
        rec={'instance':r.instance_id}
        for ty in ['CALLS','INHERITS','IMPORTS']:
            rec[f'{ty}_old']=len(to[ty]); rec[f'{ty}_new']=len(tn[ty])
        rec['all_old']=len(ao); rec['all_new']=len(an); rec['lost']=len(ao-an); rec['gained']=len(an-ao)
        rec['gained_only_inherits']=len((an-ao)&tn['INHERITS']-tn['CALLS']-tn['IMPORTS'])
        rec['lost_were_calls_only']=len((ao-an)&(to['CALLS']-to['IMPORTS']-to['INHERITS']))
        res.append(rec)
        for p in (o,n): p.unlink(missing_ok=True)
        shutil.rmtree(tree)
print(pd.DataFrame(res).to_string(index=False))
