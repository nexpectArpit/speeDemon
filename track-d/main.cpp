#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
static constexpr uint64_t INF=~0ULL;
struct MF{const char*d=0;size_t s=0;MF(const std::string&p){int fd=open(p.c_str(),O_RDONLY);struct stat st{};fstat(fd,&st);s=st.st_size;d=s?(char*)mmap(0,s,PROT_READ,MAP_PRIVATE,fd,0):0;close(fd);}~MF(){if(d)munmap((void*)d,s);}};
struct Edge{uint32_t to=0,w=0;};struct RE{uint32_t u=0,v=0,w=0;};struct Q{uint32_t s=0,t=0;};
struct HP{std::vector<std::pair<uint64_t,uint32_t>>d;bool em()const{return d.empty();}void cl(){d.clear();}auto tp()const{return d[0];}
void ps(uint64_t w,uint32_t u){d.push_back({w,u});size_t i=d.size()-1;while(i>0){size_t p=(i-1)>>1;if(d[i].first<d[p].first){std::swap(d[i],d[p]);i=p;}else break;}}
auto pp(){auto t=d[0];auto b=d.back();d.pop_back();if(!d.empty()){d[0]=b;size_t i=0,n=d.size();while(1){size_t l=(i<<1)+1,r=l+1,s=i;if(l<n&&d[l].first<d[s].first)s=l;if(r<n&&d[r].first<d[s].first)s=r;if(s!=i){std::swap(d[i],d[s]);i=s;}else break;}}return t;}};
template<class F>void par(size_t W,F f){if(W<=1){f(0);return;}std::vector<std::thread>th;for(size_t i=1;i<W;++i)th.emplace_back(f,i);f(0);for(auto&x:th)x.join();}
inline uint32_t pu(const char*&p,const char*e){uint32_t v=0;while(p<e&&*p>='0'&&*p<='9')v=v*10+(*p++-'0');return v;}
int main(int ac,char**av){try{
const char*dd=ac==3?av[1]:"/data",*od=ac==3?av[2]:"/out";
size_t W=12;if(auto*s=getenv("SD_THREADS")){size_t v;auto r=std::from_chars(s,s+strlen(s),v);if(r.ec==std::errc{}&&r.ptr==s+strlen(s)&&v>=1&&v<=12)W=v;}
MF gf(std::string(dd)+"/graph.txt");const char*gp=gf.d,*ge=gp+gf.s;
uint32_t n=pu(gp,ge);if(gp<ge&&*gp==' ')++gp;uint32_t m=pu(gp,ge);while(gp<ge&&*gp!='\n')++gp;if(gp<ge)++gp;
const char*es=gp;size_t eb=ge-es;std::vector<const char*>bn(W+1,ge);bn[0]=es;
for(size_t i=1;i<W;++i){const char*p=es+eb*i/W;while(p<ge&&*(p-1)!='\n')++p;bn[i]=p;}
std::vector<std::vector<RE>>lre(W);
par(W,[&](size_t wid){const char*sc=bn[wid],*lim=bn[wid+1];auto&v=lre[wid];v.reserve(m/W+4096);
while(sc<lim){if(*sc=='\n'||*sc=='\r'){++sc;continue;}uint32_t u=pu(sc,lim);if(sc<lim&&*sc==' ')++sc;uint32_t v_=pu(sc,lim);if(sc<lim&&*sc==' ')++sc;uint32_t w=pu(sc,lim);while(sc<lim&&*sc!='\n')++sc;if(sc<lim)++sc;v.push_back({u,v_,w});}});
std::vector<uint32_t>odg(n,0),idg(n,0);for(size_t w=0;w<W;++w)for(const auto&e:lre[w]){if(e.u<n)odg[e.u]++;if(e.v<n)idg[e.v]++;}
std::vector<uint32_t>fo(n+1,0),ro(n+1,0);for(uint32_t i=0;i<n;++i){fo[i+1]=fo[i]+odg[i];ro[i+1]=ro[i]+idg[i];}
auto fc=fo,rc=ro;std::vector<Edge>fe(m),re(m);
for(size_t w=0;w<W;++w){for(const auto&e:lre[w])if(e.u<n&&e.v<n){fe[fc[e.u]++]={e.v,e.w};re[rc[e.v]++]={e.u,e.w};}std::vector<RE>().swap(lre[w]);}
MF qf(std::string(dd)+"/queries.txt");const char*qp=qf.d,*qe=qp+qf.s;std::vector<Q>qs;qs.reserve(4096);
while(qp<qe){if(*qp=='\n'||*qp=='\r'){++qp;continue;}uint32_t s=pu(qp,qe);if(qp<qe&&*qp==' ')++qp;uint32_t t=pu(qp,qe);while(qp<qe&&*qp!='\n')++qp;if(qp<qe)++qp;qs.push_back({s,t});}
size_t NQ=qs.size();std::vector<int64_t>ans(NQ,-1);
struct WS{std::vector<uint64_t>fd,rd;std::vector<uint32_t>vs;HP fh,rh;WS(size_t N):fd(N,INF),rd(N,INF){vs.reserve(16384);}};
std::vector<WS>ws;ws.reserve(W);for(size_t w=0;w<W;++w)ws.emplace_back(n);std::atomic<size_t>qi{0};
par(W,[&](size_t wid){WS&st=ws[wid];auto&fd=st.fd;auto&rd=st.rd;auto&vs=st.vs;auto&fh=st.fh;auto&rh=st.rh;
for(size_t q=qi.fetch_add(1);q<NQ;q=qi.fetch_add(1)){uint32_t s=qs[q].s,t=qs[q].t;
if(s==t){ans[q]=0;continue;}if(s>=n||t>=n||!odg[s]||!idg[t]){ans[q]=-1;continue;}
fd[s]=0;rd[t]=0;vs.push_back(s);vs.push_back(t);fh.ps(0,s);rh.ps(0,t);uint64_t mu=INF;
while(!fh.em()&&!rh.em()){uint64_t fm=fh.tp().first,rm=rh.tp().first;if(fm+rm>=mu)break;
if(fm<=rm){auto top=fh.pp();uint64_t d=top.first;uint32_t u=top.second;if(d>fd[u]||d>=mu)continue;
for(uint32_t e=fo[u];e<fo[u+1];++e){uint32_t v=fe[e].to;uint64_t nd=d+fe[e].w;if(nd<fd[v]){if(fd[v]==INF&&rd[v]==INF)vs.push_back(v);fd[v]=nd;fh.ps(nd,v);if(rd[v]!=INF&&nd+rd[v]<mu)mu=nd+rd[v];}}}
else{auto top=rh.pp();uint64_t d=top.first;uint32_t v=top.second;if(d>rd[v]||d>=mu)continue;
for(uint32_t e=ro[v];e<ro[v+1];++e){uint32_t u=re[e].to;uint64_t nd=d+re[e].w;if(nd<rd[u]){if(fd[u]==INF&&rd[u]==INF)vs.push_back(u);rd[u]=nd;rh.ps(nd,u);if(fd[u]!=INF&&fd[u]+nd<mu)mu=fd[u]+nd;}}}}
ans[q]=(mu==INF)?-1:int64_t(mu);for(uint32_t nd:vs){fd[nd]=INF;rd[nd]=INF;}vs.clear();fh.cl();rh.cl();}});
std::string out;out.reserve(NQ*12);char buf[24];
for(size_t q=0;q<NQ;++q){if(ans[q]<0)out+="-1\n";else{out.append(buf,std::to_chars(buf,buf+24,uint64_t(ans[q])).ptr);out+='\n';}}
std::filesystem::create_directories(od);FILE*f=fopen((std::string(od)+"/distances.txt").c_str(),"wb");fwrite(out.data(),1,out.size(),f);fclose(f);_exit(0);}
catch(std::exception&e){fprintf(stderr,"%s\n",e.what());return 1;}}
