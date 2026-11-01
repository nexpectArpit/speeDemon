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
struct MF{const char*d=0;size_t s=0;MF(const std::string&p){int fd=open(p.c_str(),O_RDONLY);struct stat st{};fstat(fd,&st);s=st.st_size;d=s?(char*)mmap(0,s,PROT_READ,MAP_PRIVATE,fd,0):0;close(fd);}~MF(){if(d)munmap((void*)d,s);}};
static constexpr size_t LTS=262144,GTS=524288;static constexpr uint32_t LTM=LTS-1,GTM=GTS-1;
struct Agg{uint64_t c=0;int64_t is=0,as=0,ami=INT64_MAX,amx=INT64_MIN;uint64_t nb=0,ob=0,nd=0;};
struct DE{uint32_t h=0,id=0;};
struct WS{std::vector<Agg>ag;std::vector<DE>tb;std::vector<std::string>ln;std::vector<std::vector<uint64_t>>cs;std::string ub;
WS():tb(LTS){}
uint32_t in(const char*s,uint16_t l){uint32_t h=2166136261u;for(uint16_t i=0;i<l;++i)h=(h^uint8_t(s[i]))*16777619u;
uint32_t x=h&LTM;while(tb[x].id){uint32_t lid=tb[x].id-1;const auto&e=ln[lid];if(e.size()==l&&tb[x].h==h&&!memcmp(e.data(),s,l))return lid;x=(x+1)&LTM;}
uint32_t id=uint32_t(ln.size());ln.emplace_back(s,l);tb[x]={h,id+1};return id;}
void ms(uint16_t cid,uint32_t nid){size_t w=nid>>6;if(w>=cs[cid].size())cs[cid].resize(std::max(w+1,cs[cid].size()*2),0);cs[cid][w]|=(1ULL<<(nid&63));}};
template<class F>void par(size_t W,F f){if(W<=1){f(0);return;}std::vector<std::thread>th;for(size_t i=1;i<W;++i)th.emplace_back(f,i);f(0);for(auto&x:th)x.join();}
inline void wu(std::string&o,uint64_t v){char b[24];o.append(b,std::to_chars(b,b+24,v).ptr);}
inline void wc(std::string&o,int64_t c){if(c<0){o+='-';c=-c;}uint64_t w=uint64_t(c/100),f=uint64_t(c%100);wu(o,w);o+='.';o+=char('0'+f/10);o+=char('0'+f%10);}
int main(int ac,char**av){try{
const char*dd=ac==3?av[1]:"/data",*od=ac==3?av[2]:"/out";
MF rf(std::string(dd)+"/regions.csv");std::vector<uint16_t>r2c(1000000,65535);std::vector<std::string>cnames;uint16_t uid=0;
{const char*p=rf.d,*end=p+rf.s;while(p<end&&*p!='\n')++p;if(p<end)++p;
auto get_c=[&](std::string_view name)->uint16_t{for(size_t i=0;i<cnames.size();++i)if(cnames[i]==name)return uint16_t(i);uint16_t id=uint16_t(cnames.size());cnames.emplace_back(name);return id;};
while(p<end){uint32_t reg=0;while(p<end&&*p>='0'&&*p<='9')reg=reg*10+(*p++-'0');if(p>=end||*p!=',')break;++p;
const char*cs=p;while(p<end&&*p!='\n'&&*p!='\r')++p;std::string_view cn(cs,p-cs);if(p<end&&*p=='\r')++p;if(p<end)++p;
uint16_t cid=get_c(cn);if(reg>=r2c.size())r2c.resize(std::max(size_t(reg+1),r2c.size()*2),65535);r2c[reg]=cid;}
uid=uint16_t(cnames.size());cnames.emplace_back("_UNMATCHED");}
size_t NC=cnames.size();MF f(std::string(dd)+"/records.csv");const char*d=f.d;size_t sz=f.s;
size_t W=12;if(auto*s=getenv("SD_THREADS")){size_t v;auto r=std::from_chars(s,s+strlen(s),v);if(r.ec==std::errc{}&&r.ptr==s+strlen(s)&&v>=1&&v<=12)W=v;}
size_t fs=0;if(sz>=3&&uint8_t(d[0])==0xEF&&uint8_t(d[1])==0xBB&&uint8_t(d[2])==0xBF)fs=3;
while(fs<sz&&d[fs]!='\n')++fs;if(fs<sz)++fs;if(sz-fs<65536)W=1;
std::vector<const char*>bn(W+1,d+sz);bn[0]=d+fs;
for(size_t i=1;i<W;++i){size_t pos=fs+(sz-fs)*i/W;bool fd=0;
while(pos<sz){if(d[pos-1]=='\n'){size_t p=pos;if(p+3<=sz&&uint8_t(d[p])==0xEF&&uint8_t(d[p+1])==0xBB&&uint8_t(d[p+2])==0xBF)p+=3;
size_t q=p;while(q<sz&&d[q]>='0'&&d[q]<='9')++q;
if(q>p&&q<sz&&d[q]==','){size_t pr=pos-1;if(pr>0&&d[pr-1]=='\r')--pr;
if(pr>=3&&d[pr-3]=='.'&&d[pr-2]>='0'&&d[pr-2]<='9'&&d[pr-1]>='0'&&d[pr-1]<='9'){bn[i]=d+pos;fd=1;break;}}}++pos;}
if(!fd)bn[i]=d+sz;}
for(size_t i=1;i<W;++i)if(bn[i]<bn[i-1])bn[i]=bn[i-1];
std::vector<WS>sh(W);for(auto&s:sh){s.ag.assign(NC,{});s.cs.resize(NC);}
par(W,[&](size_t wid){WS&s=sh[wid];const char*c=bn[wid],*end=bn[wid+1];
while(c<end){if(c+3<=end&&uint8_t(c[0])==0xEF&&uint8_t(c[1])==0xBB&&uint8_t(c[2])==0xBF)c+=3;
uint64_t id=0;while(c<end&&*c>='0'&&*c<='9')id=id*10+(*c++-'0');if(c<end&&*c==',')++c;
const char*np=c;uint16_t nl=0;
if(c<end&&*c=='"'){++c;const char*st=c;bool esc=0;while(c<end){if(*c=='"'){if(c+1<end&&c[1]=='"'){esc=1;c+=2;}else break;}else++c;}
const char*cq=c;if(c<end&&*c=='"')++c;if(!esc){np=st;nl=uint16_t(cq-st);}else{s.ub.clear();s.ub.reserve(cq-st);
for(const char*x=st;x<cq;++x){if(*x=='"'&&x+1<cq&&x[1]=='"'){s.ub+='"';++x;}else s.ub+=*x;}np=s.ub.data();nl=uint16_t(s.ub.size());}}
else{const char*st=c;while(c<end&&*c!=',')++c;np=st;nl=uint16_t(c-st);}
if(c<end&&*c==',')++c;
uint32_t ntl=0;if(c<end&&*c=='"'){++c;while(c<end){if(*c=='"'){if(c+1<end&&c[1]=='"'){ntl++;c+=2;}else break;}else{ntl++;++c;}}if(c<end)++c;}
else{const char*st=c;while(c<end&&*c!=',')++c;ntl=uint32_t(c-st);}
if(c<end&&*c==',')++c;
uint32_t reg=0;while(c<end&&*c>='0'&&*c<='9')reg=reg*10+(*c++-'0');if(c<end&&*c==',')++c;
uint16_t cid=(reg<r2c.size())?r2c[reg]:uid;if(cid==65535)cid=uid;
bool neg=0;if(c<end&&*c=='-'){neg=1;++c;}int64_t cts=0;while(c<end&&*c>='0'&&*c<='9')cts=cts*10+(*c++-'0');
if(c<end&&*c=='.'){++c;if(c<end)cts=cts*10+(*c++-'0');if(c<end)cts=cts*10+(*c++-'0');}if(neg)cts=-cts;
if(c<end&&*c=='\r')++c;if(c<end&&*c=='\n')++c;
uint32_t nid=s.in(np,nl);s.ms(cid,nid);Agg&a=s.ag[cid];a.c++;a.is+=int64_t(id);a.as+=cts;
if(cts<a.ami)a.ami=cts;if(cts>a.amx)a.amx=cts;a.nb+=nl;a.ob+=ntl;}});
std::vector<Agg>ta(NC);for(uint16_t k=0;k<NC;++k)for(size_t w=0;w<W;++w){const Agg&sa=sh[w].ag[k];if(!sa.c)continue;ta[k].c+=sa.c;ta[k].is+=sa.is;ta[k].as+=sa.as;if(sa.ami<ta[k].ami)ta[k].ami=sa.ami;if(sa.amx>ta[k].amx)ta[k].amx=sa.amx;ta[k].nb+=sa.nb;ta[k].ob+=sa.ob;}
std::vector<std::string>gn;std::vector<DE>gtb(GTS);
auto gin=[&](const std::string&s)->uint32_t{uint32_t h=2166136261u;for(char ch:s)h=(h^uint8_t(ch))*16777619u;
uint32_t x=h&GTM;while(gtb[x].id){uint32_t g=gtb[x].id-1;if(gtb[x].h==h&&gn[g]==s)return g;x=(x+1)&GTM;}
uint32_t id=uint32_t(gn.size());gn.push_back(s);gtb[x]={h,id+1};return id;};
std::vector<std::vector<uint32_t>>srm(W);for(size_t w=0;w<W;++w){srm[w].resize(sh[w].ln.size());for(size_t i=0;i<sh[w].ln.size();++i)srm[w][i]=gin(sh[w].ln[i]);}
size_t NGN=gn.size(),WPC=(NGN+63)/64;std::atomic<size_t>ci{0};
par(W,[&](size_t){std::vector<uint64_t>cb(WPC);for(size_t k=ci.fetch_add(1);k<NC;k=ci.fetch_add(1)){if(!ta[k].c)continue;
std::fill(cb.begin(),cb.end(),0);for(size_t w=0;w<W;++w){const auto&seen=sh[w].cs[k];const auto&rm=srm[w];
for(size_t widx=0;widx<seen.size();++widx){uint64_t m=seen[widx];if(!m)continue;size_t bl=widx<<6;while(m){int bit=__builtin_ctzll(m);uint32_t g=rm[bl+bit];cb[g>>6]|=(1ULL<<(g&63));m&=(m-1);}}}
uint64_t dcnt=0;for(uint64_t w:cb)dcnt+=__builtin_popcountll(w);ta[k].nd=dcnt;}});
std::vector<uint16_t>sc;for(uint16_t k=0;k<NC;++k)if(ta[k].c)sc.push_back(k);
std::sort(sc.begin(),sc.end(),[&](uint16_t a,uint16_t b){return cnames[a]<cnames[b];});
std::string out;out.reserve(sc.size()*120);
for(uint16_t k:sc){const Agg&a=ta[k];out+=cnames[k];out+=' ';wu(out,a.c);out+=' ';wu(out,a.is);out+=' ';wc(out,a.as);out+=' ';wc(out,a.ami);out+=' ';wc(out,a.amx);out+=' ';wu(out,a.nd);out+=' ';wu(out,a.nb);out+=' ';wu(out,a.ob);out+='\n';}
std::filesystem::create_directories(od);FILE*fout=fopen((std::string(od)+"/aggregates.txt").c_str(),"wb");fwrite(out.data(),1,out.size(),fout);fclose(fout);return 0;}
catch(std::exception&e){fprintf(stderr,"%s\n",e.what());return 1;}}
