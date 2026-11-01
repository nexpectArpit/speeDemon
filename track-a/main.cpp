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
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
static constexpr size_t H=72,TS=8192; static constexpr uint32_t TM=TS-1;
struct DE{uint32_t h=0;uint16_t l=0,id=0;const char*p=nullptr;};
struct Rec{uint32_t lat;uint16_t ep;uint8_t err;};
struct Sh{DE tb[TS];std::vector<std::string_view>ep;std::array<std::vector<Rec>,H>hr;std::array<std::string,H>hs;
uint16_t in(const char*s,uint16_t l){uint32_t h=2166136261u;for(uint16_t i=0;i<l;++i)h=(h^uint8_t(s[i]))*16777619u;
uint32_t x=h&TM;while(tb[x].p){if(tb[x].l==l&&tb[x].h==h&&!memcmp(tb[x].p,s,l))return tb[x].id;x=(x+1)&TM;}
uint16_t id=uint16_t(ep.size());ep.push_back({s,l});tb[x]={h,l,id,s};return id;}};
inline size_t hi(const char*t){int d=(t[8]-'0')*10+t[9]-'0',ds=(t[6]=='0')?(d-30):(d+1);return size_t(ds*24+(t[11]-'0')*10+t[12]-'0'-18);}
void pc(const char*c,const char*e,Sh&s){for(size_t h=0;h<H;++h)s.hr[h].reserve(4096);
while(c<e){size_t h=hi(c);if(s.hs[h].empty())s.hs[h]=std::string(c,13)+":00:00Z";c+=28;
while(*c!=' ')++c;const char*es=++c;while(*c!=' ')++c;uint16_t el=uint16_t(c-es),ei=s.in(es,el);
bool er=c[1]=='5';c+=5;uint32_t l=0;while(*c!='\n'){l=l*10+uint32_t(*c-'0');++c;}++c;
s.hr[h].push_back({l,ei,uint8_t(er)});}}
inline void wu(std::string&o,uint64_t v){char b[24];o.append(b,std::to_chars(b,b+24,v).ptr);}
int main(int ac,char**av){try{
const char*dd=ac==3?av[1]:"/data",*od=ac==3?av[2]:"/out";
int fd=open((std::string(dd)+"/requests.log").c_str(),O_RDONLY);struct stat st{};fstat(fd,&st);
size_t sz=size_t(st.st_size);const char*da=sz?(const char*)mmap(0,sz,PROT_READ,MAP_PRIVATE,fd,0):nullptr;close(fd);
size_t W=12;if(auto*s=getenv("SD_THREADS")){size_t v;auto r=std::from_chars(s,s+strlen(s),v);
if(r.ec==std::errc{}&&r.ptr==s+strlen(s)&&v>=1&&v<=12)W=v;}
std::vector<Sh>sh(W);if(sz){std::vector<const char*>bn(W+1);bn[0]=da;bn[W]=da+sz;
for(size_t i=1;i<W;++i){auto*p=da+sz*i/W;while(p<bn[W]&&p[-1]!='\n')++p;bn[i]=p;}
std::vector<std::thread>th;for(size_t i=1;i<W;++i)th.emplace_back(pc,bn[i],bn[i+1],std::ref(sh[i]));
pc(bn[0],bn[1],sh[0]);for(auto&t:th)t.join();}
std::vector<std::string_view>ge;std::unordered_map<std::string_view,uint16_t>ed;
std::array<std::string,H>gh;
for(auto&s:sh){for(size_t h=0;h<H;++h)if(!s.hs[h].empty())gh[h]=s.hs[h];
for(auto&e:s.ep)if(ed.try_emplace(e,uint16_t(ge.size())).second)ge.push_back(e);}
std::sort(ge.begin(),ge.end());for(uint16_t i=0;i<uint16_t(ge.size());++i)ed[ge[i]]=i;
std::vector<std::vector<uint16_t>>sr(W);
for(size_t w=0;w<W;++w){sr[w].resize(sh[w].ep.size());for(size_t i=0;i<sh[w].ep.size();++i)sr[w][i]=ed[sh[w].ep[i]];}
size_t ne=ge.size();std::array<std::string,H>ob;std::atomic<size_t>hc{0};
auto work=[&](size_t){std::vector<uint32_t>ec(ne,0),ee(ne,0),eo(ne+1,0);std::vector<uint32_t>la;
for(size_t h=hc.fetch_add(1);h<H;h=hc.fetch_add(1)){if(gh[h].empty())continue;
size_t tr=0;std::fill(ec.begin(),ec.end(),0);std::fill(ee.begin(),ee.end(),0);
for(size_t w=0;w<W;++w){auto&r=sh[w].hr[h];tr+=r.size();for(auto&c:r){uint16_t g=sr[w][c.ep];ec[g]++;ee[g]+=c.err;}}
if(!tr)continue;eo[0]=0;for(size_t e=0;e<ne;++e)eo[e+1]=eo[e]+ec[e];la.resize(tr);
auto co=eo;for(size_t w=0;w<W;++w)for(auto&c:sh[w].hr[h]){uint16_t g=sr[w][c.ep];la[co[g]++]=c.lat;}
auto&buf=ob[h];buf.reserve(tr*40);
for(size_t e=0;e<ne;++e){uint32_t cnt=ec[e];if(!cnt)continue;uint32_t*v=la.data()+eo[e];
size_t r50=(50ULL*cnt+99)/100-1,r95=(95ULL*cnt+99)/100-1,r99=(99ULL*cnt+99)/100-1;
uint32_t p50,p95,p99;if(cnt<=32){std::sort(v,v+cnt);p50=v[r50];p95=v[r95];p99=v[r99];
}else{std::nth_element(v,v+r50,v+cnt);p50=v[r50];if(r95>r50)std::nth_element(v+r50+1,v+r95,v+cnt);
p95=v[r95];if(r99>r95)std::nth_element(v+r95+1,v+r99,v+cnt);p99=v[r99];}
uint64_t ppm=uint64_t(ee[e])*1000000ULL/cnt;
buf+=gh[h];buf+=' ';buf.append(ge[e]);buf+=' ';wu(buf,cnt);buf+=' ';wu(buf,p50);buf+=' ';
wu(buf,p95);buf+=' ';wu(buf,p99);buf+=' ';wu(buf,ee[e]);buf+=' ';wu(buf,ppm);buf+='\n';}}};
std::vector<std::thread>th;for(size_t i=1;i<W;++i)th.emplace_back(work,i);work(0);for(auto&t:th)t.join();
std::filesystem::create_directories(od);FILE*f=fopen((std::string(od)+"/summary.txt").c_str(),"wb");
for(auto&b:ob)if(!b.empty())fwrite(b.data(),1,b.size(),f);fclose(f);
_exit(0);
}catch(std::exception&e){fprintf(stderr,"%s\n",e.what());return 1;}}
