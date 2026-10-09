import subprocess, random, re, os, sys
from concurrent.futures import ThreadPoolExecutor
from collections import Counter
# usage: python rate.py <pe> <c|cpp> <N> <seed> <outdir>   (one function per clang-cl compile)
RV=r"C:\Users\User\RustroverProjects\Revenant\llvm-project\build-rv\tools\revenant-decompile\revenant-decompile.exe"
CLCL=r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe"
pe,lang,n,seed,out=sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4]),sys.argv[5]
os.makedirs(out,exist_ok=True)
lst=subprocess.run([RV,pe,"--list"],capture_output=True,text=True).stdout
vas=[l.split("\t")[0] for l in lst.splitlines() if l.startswith("0x")]
random.seed(seed); sample=random.sample(vas,min(n,len(vas)))
ext="cpp" if lang=="cpp" else "c"
def one(va):
    base=os.path.join(out,"f"+va)
    r=subprocess.run([RV,pe,"--lang",lang,"--emit","source","-o",base,"--func",va],capture_output=True,text=True)
    if r.returncode: return va,"EMIT",[r.stderr.splitlines()[0][:150]]
    a=[CLCL,"/nologo","/c","/Fo"+base+".obj"]+(["/TP","/EHsc"] if lang=="cpp" else [])+[base+".source."+ext]
    c=subprocess.run(a,capture_output=True,text=True)
    return va,c.returncode,[re.sub(r"^.*?error: ","",l)[:110] for l in (c.stdout+c.stderr).splitlines() if "error:" in l]
with ThreadPoolExecutor(4) as ex: res=list(ex.map(one,sample))
ok=sum(1 for r in res if r[1]==0)
print(f"{ok}/{len(res)} compile")
for va,rc,e in res:
    if rc!=0: print(va,e[:3])
print(Counter(x for _,rc,e in res if rc!=0 for x in e).most_common(10))
