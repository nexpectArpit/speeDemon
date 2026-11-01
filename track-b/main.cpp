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
inline uint8_t tc(char c){return(c>='a'&&c<='z')?c:(c>='0'&&c<='9')?c:(c>='A'&&c<='Z')?(c+32):0;}
struct Span{const char*p;uint16_t l;};
inline bool mt(Span s,std::string_view t){if(s.l!=t.size())return 0;for(size_t i=0;i<s.l;++i)if(tc(s.p[i])!=uint8_t(t[i]))return 0;return 1;}
static constexpr size_t TS=262144;static constexpr uint32_t TM=TS-1;
struct DE{uint32_t h=0,id=0;};
struct Dict{std::vector<DE>tb;std::vector<std::string>tm;Dict():tb(TS){}
uint32_t in(std::string_view s){uint32_t h=2166136261u;for(char c:s)h=(h^uint8_t(c))*16777619u;uint32_t x=h&TM;while(tb[x].id){uint32_t tid=tb[x].id-1;if(tb[x].h==h&&tm[tid]==s)return tid;x=(x+1)&TM;}uint32_t id=uint32_t(tm.size());tm.emplace_back(s);tb[x]={h,id+1};return id;}
uint32_t fn(const char*s,size_t l)const{uint32_t h=2166136261u;for(size_t i=0;i<l;++i)h=(h^uint8_t(s[i]))*16777619u;uint32_t x=h&TM;while(tb[x].id){uint32_t tid=tb[x].id-1;if(tb[x].h==h&&tm[tid].size()==l&&!memcmp(tm[tid].data(),s,l))return tid;x=(x+1)&TM;}return~0u;}};
enum class Op:uint8_t{AND,OR,NOT};
struct QItem{bool ph=0;std::vector<uint32_t>t;std::vector<Op>op;std::vector<std::string>rw;};
struct DocRef{const char*p;uint32_t l;};
template<class F>void par(size_t W,F f){if(W<=1){f(0);return;}std::vector<std::thread>th;for(size_t i=1;i<W;++i)th.emplace_back(f,i);f(0);for(auto&x:th)x.join();}
int main(int ac,char**av){try{
const char*dd=ac==3?av[1]:"/data",*od=ac==3?av[2]:"/out";
size_t W=12;if(auto*s=getenv("SD_THREADS")){size_t v;auto r=std::from_chars(s,s+strlen(s),v);if(r.ec==std::errc{}&&r.ptr==s+strlen(s)&&v>=1&&v<=12)W=v;}
MF qf(std::string(dd)+"/queries.txt");const char*qp=qf.d,*qe=qp+qf.s;Dict dt;std::vector<QItem>qs;qs.reserve(50000);
while(qp<qe){while(qp<qe&&(*qp=='\n'||*qp=='\r'))++qp;if(qp>=qe)break;const char*ls=qp;while(qp<qe&&*qp!='\n')++qp;std::string_view l(ls,qp-ls);if(qp<qe)++qp;
if(l.starts_with("BOOL ")){QItem q;size_t p=5;bool eo=0;while(p<l.size()){size_t sp=l.find(' ',p);if(sp==std::string_view::npos)sp=l.size();std::string_view w=l.substr(p,sp-p);if(eo){q.op.push_back(w=="AND"?Op::AND:w=="OR"?Op::OR:Op::NOT);eo=0;}else{q.t.push_back(dt.in(w));eo=1;}p=sp+1;}qs.push_back(std::move(q));}
else if(l.starts_with("PHRASE \"")){QItem q;q.ph=1;size_t p=8,qe_=l.rfind('"');if(qe_==std::string_view::npos)qe_=l.size();while(p<qe_){size_t sp=l.find(' ',p);if(sp==std::string_view::npos||sp>qe_)sp=qe_;std::string_view w=l.substr(p,sp-p);q.t.push_back(dt.in(w));q.rw.emplace_back(w);p=sp+1;}qs.push_back(std::move(q));}}
size_t NT=dt.tm.size();MF df(std::string(dd)+"/docs.txt");const char*dp=df.d,*de=dp+df.s;std::vector<DocRef>dr;dr.reserve(260000);
const char*p=dp;while(p<de){while(p<de&&*p!='\t')++p;if(p>=de)break;++p;const char*ts=p;while(p<de&&*p!='\n')++p;dr.push_back({ts,uint32_t(p-ts)});if(p<de)++p;}
size_t ND=dr.size(),NW=(ND+63)/64;std::vector<std::vector<std::vector<uint32_t>>>wp(W);for(auto&v:wp)v.resize(NT);
par(W,[&](size_t wid){size_t s=ND*wid/W,e=ND*(wid+1)/W;std::vector<uint32_t>lt;lt.reserve(256);char tb[64];
for(size_t id=s;id<e;++id){lt.clear();const char*tp=dr[id].p,*te=tp+dr[id].l;while(tp<te){while(tp<te&&!tc(*tp))++tp;if(tp>=te)break;size_t tl=0;while(tp<te&&tc(*tp)){if(tl<63)tb[tl++]=tc(*tp);++tp;}uint32_t tid=dt.fn(tb,tl);if(tid!=~0u)lt.push_back(tid);}
std::sort(lt.begin(),lt.end());lt.erase(std::unique(lt.begin(),lt.end()),lt.end());for(uint32_t tid:lt)wp[wid][tid].push_back(uint32_t(id));}});
std::vector<std::vector<uint32_t>>pos(NT);for(size_t t=0;t<NT;++t){size_t sz=0;for(size_t w=0;w<W;++w)sz+=wp[w][t].size();pos[t].reserve(sz);for(size_t w=0;w<W;++w)pos[t].insert(pos[t].end(),wp[w][t].begin(),wp[w][t].end());}
std::vector<std::vector<uint64_t>>bs(NT);for(size_t t=0;t<NT;++t)if(pos[t].size()>=256){bs[t].assign(NW,0);for(uint32_t id:pos[t])bs[t][id>>6]|=(1ULL<<(id&63));}
size_t NQ=qs.size();std::vector<std::string>res(NQ);std::atomic<size_t>qc{0};
par(W,[&](size_t){std::vector<uint64_t>cb(NW),tb(NW);std::vector<uint32_t>pc;std::vector<Span>dtk;dtk.reserve(512);char b[32];
for(size_t q=qc.fetch_add(1);q<NQ;q=qc.fetch_add(1)){auto&it=qs[q];auto&rs=res[q];
if(!it.ph){std::fill(cb.begin(),cb.end(),0);uint32_t t0=it.t[0];
if(t0!=~0u){if(!bs[t0].empty())std::copy(bs[t0].begin(),bs[t0].end(),cb.begin());else for(uint32_t d:pos[t0])cb[d>>6]|=(1ULL<<(d&63));}
for(size_t i=0;i<it.op.size();++i){Op o=it.op[i];uint32_t ti=it.t[i+1];if(ti==~0u||pos[ti].empty()){if(o==Op::AND)std::fill(cb.begin(),cb.end(),0);continue;}
if(!bs[ti].empty()){const auto&b_=bs[ti];if(o==Op::AND)for(size_t w=0;w<NW;++w)cb[w]&=b_[w];else if(o==Op::OR)for(size_t w=0;w<NW;++w)cb[w]|=b_[w];else for(size_t w=0;w<NW;++w)cb[w]&=~b_[w];}
else{const auto&pl=pos[ti];if(o==Op::AND){std::fill(tb.begin(),tb.end(),0);for(uint32_t d:pl)if(cb[d>>6]&(1ULL<<(d&63)))tb[d>>6]|=(1ULL<<(d&63));std::copy(tb.begin(),tb.end(),cb.begin());}
else if(o==Op::OR){for(uint32_t d:pl)cb[d>>6]|=(1ULL<<(d&63));}else for(uint32_t d:pl)cb[d>>6]&=~(1ULL<<(d&63));}}
uint32_t cnt=0,f10[10],n10=0;uint64_t isum=0;
for(size_t w=0;w<NW;++w){uint64_t m=cb[w];if(!m)continue;cnt+=__builtin_popcountll(m);size_t base=w<<6;while(m){int bit=__builtin_ctzll(m);uint32_t d=uint32_t(base+bit);isum+=d;if(n10<10)f10[n10++]=d;m&=(m-1);}}
rs.append(b,std::to_chars(b,b+32,cnt).ptr);rs+=' ';rs.append(b,std::to_chars(b,b+32,isum).ptr);for(uint32_t i=0;i<n10;++i){rs+=' ';rs.append(b,std::to_chars(b,b+32,f10[i]).ptr);}}
else{const auto&ts=it.t;const auto&rw=it.rw;size_t K=ts.size();bool emp=0;for(uint32_t ti:ts)if(ti==~0u||pos[ti].empty()){emp=1;break;}
if(emp){rs="0 0";continue;}size_t mi=0,msz=pos[ts[0]].size();for(size_t i=1;i<K;++i)if(pos[ts[i]].size()<msz){msz=pos[ts[i]].size();mi=i;}
pc=pos[ts[mi]];for(size_t i=0;i<K;++i){if(i==mi)continue;uint32_t ti=ts[i];if(!bs[ti].empty()){const auto&b_=bs[ti];size_t o=0;for(uint32_t d:pc)if(b_[d>>6]&(1ULL<<(d&63)))pc[o++]=d;pc.resize(o);}
else{const auto&pl=pos[ti];size_t o=0;for(uint32_t d:pc)if(std::binary_search(pl.begin(),pl.end(),d))pc[o++]=d;pc.resize(o);}if(pc.empty())break;}
uint32_t cnt=0,f10[10],n10=0;uint64_t isum=0;
for(uint32_t d:pc){const char*tp=dr[d].p,*te=tp+dr[d].l;dtk.clear();while(tp<te){while(tp<te&&!tc(*tp))++tp;if(tp>=te)break;const char*st=tp;while(tp<te&&tc(*tp))++tp;dtk.push_back({st,uint16_t(tp-st)});}
if(dtk.size()<K)continue;bool ok=0;for(size_t p=0;p+K<=dtk.size();++p){bool eq=1;for(size_t j=0;j<K;++j)if(!mt(dtk[p+j],rw[j])){eq=0;break;}if(eq){ok=1;break;}}
if(ok){cnt++;isum+=d;if(n10<10)f10[n10++]=d;}}
rs.append(b,std::to_chars(b,b+32,cnt).ptr);rs+=' ';rs.append(b,std::to_chars(b,b+32,isum).ptr);for(uint32_t i=0;i<n10;++i){rs+=' ';rs.append(b,std::to_chars(b,b+32,f10[i]).ptr);}}}});
std::string out;out.reserve(NQ*32);for(size_t q=0;q<NQ;++q){out.append(res[q]);out+='\n';}
std::filesystem::create_directories(od);FILE*f=fopen((std::string(od)+"/results.txt").c_str(),"wb");fwrite(out.data(),1,out.size(),f);fclose(f);return 0;}
catch(std::exception&e){fprintf(stderr,"%s\n",e.what());return 1;}}
