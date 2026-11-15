#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include <orbis/Sysmodule.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Http.h>
#include "ui.h"

static int pad=-1,pad_connected;
static uint64_t pad_retry,g_start,last_launch_try;

/* All file I/O uses sceKernel* (never stdio): the app runs in a restricted
 * sandbox where a blocked libc syscall raises SIGSYS. app.log confirms these
 * kernel calls succeed. */
static void logline(const char*s){
 uint64_t now=sceKernelGetProcessTime();
 if(!g_start)g_start=now;
 uint64_t t=(now>=g_start?(now-g_start):0)/100000;
 char buf[320];
 int n=snprintf(buf,sizeof(buf),"[+%llu.%llus] %s\n",(unsigned long long)(t/10),(unsigned long long)(t%10),s);
 if(n<=0)return;
 if(n>(int)sizeof(buf))n=(int)sizeof(buf);
 int fd=sceKernelOpen("/data/bta_audio/app.log",0x209,0644);
 if(fd>=0){sceKernelWrite(fd,buf,(size_t)n);sceKernelClose(fd);}
}
static void log_rotate(void){
 int fd=sceKernelOpen("/data/bta_audio/app.log",0,0);
 if(fd<0)return;
 off_t sz=sceKernelLseek(fd,0,2);
 sceKernelClose(fd);
 if(sz>=131072)sceKernelRename("/data/bta_audio/app.log","/data/bta_audio/app.prev.log");
}
static unsigned controller(uint64_t now){
 OrbisPadData data;memset(&data,0,sizeof(data));
 if(pad>=0){
  int rc=scePadReadState(pad,&data);
  if(rc>=0&&data.connected){pad_connected=1;return data.buttons;}
  if(pad_connected){char msg[96];snprintf(msg,sizeof(msg),"Controller disconnected (rc=0x%x connected=%d)",rc,(int)data.connected);logline(msg);}
  pad_connected=0;
 }
 if(now<pad_retry)return 0;pad_retry=now+2000000;
 if(pad>=0){scePadClose(pad);pad=-1;}
 OrbisUserServiceLoginUserIdList users;for(int i=0;i<4;i++)users.userId[i]=-1;
 int rc=sceUserServiceGetLoginUserIdList(&users);
 if(rc<0)sceUserServiceGetInitialUser(&users.userId[0]);
 for(int i=0;i<4;i++)if(users.userId[i]>=0){
  int h=scePadGetHandle(users.userId[i],ORBIS_PAD_PORT_TYPE_STANDARD,0);
  if(h<0)h=scePadOpen(users.userId[i],ORBIS_PAD_PORT_TYPE_STANDARD,0,0);
  if(h<0)continue;memset(&data,0,sizeof(data));
  if(scePadReadState(h,&data)>=0&&data.connected){pad=h;pad_connected=1;logline("Controller acquired");return data.buttons;}
  scePadClose(h);
 }
 return 0;
}

/* ---- HTTP client: the audio service owns Bluetooth; this app only displays and requests ---- */
static int local_socket(int port){
 int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
 struct timeval t={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));
 int one=1;setsockopt(fd,SOL_SOCKET,0x800,&one,sizeof(one));
 struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_len=sizeof(a);a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(0x7f000001);
 if(connect(fd,(void*)&a,sizeof(a))<0){close(fd);return -1;}return fd;
}
static int write_all(int fd,const void*p,size_t n){const char*b=p;while(n){ssize_t k=send(fd,b,n,0);if(k<=0)return -1;b+=k;n-=k;}return 0;}
/* Returns the HTTP status code (e.g. 200, 409) or -1 on transport error. Copies
 * the response body (NUL-terminated) into out when provided. Every path closes fd. */
static int http(const char*method,const char*path,void*out,size_t cap){
 int fd=local_socket(8097);if(fd<0)return -1;
 char request[256];int len=snprintf(request,sizeof(request),"%s %s HTTP/1.1\r\nHost: 127.0.0.1:8097\r\nX-BTA-Control: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",method,path);
 if(len<0||len>=(int)sizeof(request)||write_all(fd,request,(size_t)len)){close(fd);return -1;}
 char response[16384];size_t used=0;
 while(used+1<sizeof(response)){ssize_t n=recv(fd,response+used,sizeof(response)-used-1,0);if(n==0)break;if(n<0){close(fd);return -1;}used+=(size_t)n;}
 close(fd);response[used]=0;
 if(used<12||strncmp(response,"HTTP/1.1 ",9))return -1;
 int code=atoi(response+9);
 char*b=strstr(response,"\r\n\r\n");
 if(out&&cap){size_t bytes=0;if(b){b+=4;bytes=used-(size_t)(b-response);if(bytes>cap-1)bytes=cap-1;memcpy(out,b,bytes);}((char*)out)[bytes]=0;}
 return code>0?code:-1;
}
/* Reads /api/native. Older services send a shorter reply without the link fields;
 * those are accepted (ext_ok=0) and the UI falls back to the basic flags. */
static int prev_online=-1,prev_link=-1,prev_streaming=-1,prev_conn_rc=0,prev_audio_rc=0;
static void log_service_change(void){
 if(online!=prev_online){
  prev_online=online;
  if(!online){logline("service: offline (port 8097 unreachable)");prev_link=-1;return;}
  char msg[128];snprintf(msg,sizeof(msg),"service: online ext_ok=%d count=%d selected=%012lX",ext_ok,state.count,state.selected);
  logline(msg);
 }
 if(!online)return;
 int l=ui_link();
 if(l!=prev_link||state.streaming!=prev_streaming||state.connect_rc!=prev_conn_rc||(state.audio_rc<0&&state.audio_rc!=prev_audio_rc)){
  prev_link=l;prev_streaming=state.streaming;prev_conn_rc=state.connect_rc;prev_audio_rc=state.audio_rc;
  char msg[260];
  snprintf(msg,sizeof(msg),"state: link=%d streaming=%d attempts=%d connect_rc=%d audio_rc=%d msg=\"%.110s\"",
   l,state.streaming,state.attempts,state.connect_rc,state.audio_rc,state.message);
  logline(msg);
 }
}
static int refresh(void){
 struct bt_native next;memset(&next,0,sizeof(next));
 int fd=local_socket(8097);if(fd<0){online=0;log_service_change();return -1;}
 const char*req="GET /api/native HTTP/1.1\r\nHost: 127.0.0.1:8097\r\nConnection: close\r\n\r\n";
 if(write_all(fd,req,strlen(req))){close(fd);online=0;log_service_change();return -1;}
 char resp[8192];size_t used=0;
 while(used<sizeof(resp)){ssize_t n=recv(fd,resp+used,sizeof(resp)-used,0);if(n<=0)break;used+=(size_t)n;}
 close(fd);
 if(used<12||strncmp(resp,"HTTP/1.1 200",12)){online=0;log_service_change();return -1;}
 char*body=0;for(size_t i=0;i+3<used;i++)if(resp[i]=='\r'&&resp[i+1]=='\n'&&resp[i+2]=='\r'&&resp[i+3]=='\n'){body=resp+i+4;break;}
 size_t avail=body?(size_t)(resp+used-body):0;
 if(!body||avail<offsetof(struct bt_native,link)){online=0;log_service_change();return -1;}
 memcpy(&next,body,avail<sizeof(next)?avail:sizeof(next));
 if(next.magic!=BT_NATIVE_MAGIC||next.version!=2||next.count<0||next.count>BT_DEVICE_LIMIT){online=0;log_service_change();return -1;}
 unsigned long selected=(focus>=0&&focus<state.count)?state.devices[focus].address:0;
 state=next;ext_ok=avail>=sizeof(next);state.message[127]=0;for(int i=0;i<state.count;i++)state.devices[i].name[95]=0;
 if(selected){for(int i=0;i<state.count;i++)if(state.devices[i].address==selected)focus=i;}
 if(focus<0||focus>=state.count)focus=0;
 online=1;log_service_change();return 0;
}
static void set_notice(const char*s){snprintf(notice,sizeof(notice),"%s",s);notice_until=g_now+6000000;}
/* The audio service normally starts at boot (GoldHEN AutoRun). If it is not running
 * when the app opens, send the bundled payload to GoldHEN's local BinLoader (port 9090)
 * once. The payload's launcher refuses to start twice, and we only try while port 8097
 * is not answering. The payload goes out on a single connection: never probe 9090. */
static void service_check(void){refresh();}
static int launch_attempted;
static void start_service(void){
 if(online||launch_attempted)return;launch_attempted=1;last_launch_try=g_now;
 static char payload[131072];
 int f=sceKernelOpen("/app0/assets/BTA-Audio.bin",0,0);
 if(f<0){logline("start: bundled payload not found");return;}
 size_t used=0;
 while(used<sizeof(payload)){long n=sceKernelRead(f,payload+used,sizeof(payload)-used);if(n<=0)break;used+=(size_t)n;}
 sceKernelClose(f);
 if(used<1024||used>=sizeof(payload)){logline("start: bundled payload size invalid");return;}
 int fd=local_socket(9090);
 if(fd<0){logline("start: BinLoader (9090) not reachable; enable it in GoldHEN and jailbreak again");return;}
 int rc=write_all(fd,payload,used);close(fd);
 {char msg[128];snprintf(msg,sizeof(msg),rc?"start: send failed (%zu bytes)":"start: payload sent to BinLoader (%zu bytes)",used);logline(msg);}
 if(!rc)set_notice("Starting audio service...");
}
static void action(const char*path){
 char body[160];int code=http("POST",path,body,sizeof(body));
 if(code==200)notice[0]=0;
 else if(code<0){
  char msg[160];snprintf(msg,sizeof(msg),"http: POST %.80s failed (transport)",path);logline(msg);
  set_notice("No response from the audio service.");
 }else{
  for(int i=0;body[i];i++)if(body[i]=='\n'||body[i]=='\r')body[i]=0;
  char msg[240];snprintf(msg,sizeof(msg),"http: POST %.60s -> %d %.100s",path,code,body);logline(msg);
  set_notice(body[0]?body:"That didn't work.");
 }
 refresh();
}
static void volume_change(int amount){
 if(!online)return;int value=state.volume+amount;if(value<0)value=0;if(value>200)value=200;
 char path[40];snprintf(path,sizeof(path),"/api/volume/%d",value);action(path);
}
static void activate(void){
 if(!online){
  if(last_launch_try&&g_now-last_launch_try<5000000){
   logline("activate: BinLoader retry skipped (cooldown; BinLoader may only accept 1 payload per jailbreak)");
   set_notice("Service offline. Wait a moment or re-enable BinLoader.");
   return;
  }
  launch_attempted=0;service_check();start_service();return;
 }
 if(state.count<=0||focus<0||focus>=state.count)return;
 const struct bt_device*d=&state.devices[focus];int l=ui_link();
 if(d->address==state.selected&&l!=BT_LINK_IDLE){action("/api/stop");return;}
 char path[64];snprintf(path,sizeof(path),"/api/select/%012lX",d->address);action(path);
}

static unsigned char snaprow[WIDTH*3];
static void snapshot(void){
 int fd=sceKernelOpen("/data/bta_audio/app-frame.ppm",0x601,0644);
 if(fd<0){logline("snapshot: open failed");return;}
 char hdr[32];int hl=snprintf(hdr,sizeof(hdr),"P6\n%d %d\n255\n",WIDTH,HEIGHT);sceKernelWrite(fd,hdr,(size_t)hl);
 for(int y=0;y<HEIGHT;y++){
  for(int x=0;x<WIDTH;x++){uint32_t v=back[y*WIDTH+x];snaprow[x*3]=(unsigned char)(v>>16);snaprow[x*3+1]=(unsigned char)(v>>8);snaprow[x*3+2]=(unsigned char)v;}
  sceKernelWrite(fd,snaprow,sizeof(snaprow));
 }
 sceKernelClose(fd);logline("snapshot: written");
}

static int g_upd_active,g_upd_ready,g_upd_result;
static char g_upd_latest[64],g_upd_log[160],g_upd_pkg_name[128],g_upd_pkg_url[512],last_upd_log[160];
static uint64_t g_upd_pkg_size;

static int g_dl_active,g_dl_state,g_dl_cancel,g_dl_finished,dl_modal_dismissed;
static uint64_t g_dl_bytes,g_dl_total;
static char g_dl_err[160],g_dl_log[160],g_dl_pkg_name[128],g_dl_pkg_url[512];

static int32_t skip_ssl_cb(int32_t libssl_id,uint32_t verify_err,void*const ssl_cert[],int32_t cert_num,void*user_arg){
 (void)libssl_id;(void)verify_err;(void)ssl_cert;(void)cert_num;(void)user_arg;
 return 1;
}

struct http_ctx{int net_id,ssl_id,http_id,tpl;};

static int open_http_ctx(struct http_ctx*ctx,const char*pool_name,uint32_t timeout_us,char*err,size_t err_cap){
 ctx->net_id=ctx->ssl_id=ctx->http_id=ctx->tpl=-1;
 if(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET)<0||
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP)<0||
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL)<0){
  snprintf(err,err_cap,"sysmodule load failed");
  return -1;
 }
 sceNetInit();
 ctx->net_id=sceNetPoolCreate(pool_name,16*1024,0);
 if(ctx->net_id<0){snprintf(err,err_cap,"sceNetPoolCreate=0x%x",ctx->net_id);return -1;}
 ctx->ssl_id=sceSslInit(SSL_POOLSIZE);
 if(ctx->ssl_id<0){snprintf(err,err_cap,"sceSslInit=0x%x",ctx->ssl_id);return -1;}
 ctx->http_id=sceHttpInit(ctx->net_id,ctx->ssl_id,LIBHTTP_POOLSIZE);
 if(ctx->http_id<0){snprintf(err,err_cap,"sceHttpInit=0x%x",ctx->http_id);return -1;}
 ctx->tpl=sceHttpCreateTemplate(ctx->http_id,"BTAudio4/" BTA_VERSION,ORBIS_HTTP_VERSION_1_1,1);
 if(ctx->tpl<0){snprintf(err,err_cap,"template=0x%x",ctx->tpl);return -1;}
 sceHttpSetResolveTimeOut(ctx->tpl,timeout_us);
 sceHttpSetConnectTimeOut(ctx->tpl,timeout_us);
 sceHttpSetSendTimeOut(ctx->tpl,timeout_us);
 sceHttpsSetSslCallback(ctx->tpl,skip_ssl_cb,0);
 return 0;
}

static void close_http_ctx(struct http_ctx*ctx){
 if(ctx->tpl>=0)sceHttpDeleteTemplate(ctx->tpl);
 if(ctx->http_id>=0)sceHttpTerm(ctx->http_id);
 if(ctx->ssl_id>=0)sceSslTerm(ctx->ssl_id);
 if(ctx->net_id>=0)sceNetPoolDestroy(ctx->net_id);
 ctx->net_id=ctx->ssl_id=ctx->http_id=ctx->tpl=-1;
}

static int http_get_follow(int tpl,const char*start_url,const char*accept_hdr,int*conn_out,int*req_out,int32_t*status_out,char*err,size_t err_cap){
 char url[1024];
 snprintf(url,sizeof(url),"%s",start_url);
 *conn_out=-1;*req_out=-1;*status_out=0;

 for(int hop=0;hop<6;hop++){
  int conn=sceHttpCreateConnectionWithURL(tpl,url,0);
  if(conn<0){snprintf(err,err_cap,"Connection failed (0x%x)",conn);return -1;}
  int req=sceHttpCreateRequestWithURL(conn,ORBIS_METHOD_GET,url,0);
  if(req<0){sceHttpDeleteConnection(conn);snprintf(err,err_cap,"Request failed (0x%x)",req);return -1;}
  if(accept_hdr)sceHttpAddRequestHeader(req,"Accept",accept_hdr,0);

  int rc=sceHttpSendRequest(req,0,0);
  if(rc<0){sceHttpDeleteRequest(req);sceHttpDeleteConnection(conn);snprintf(err,err_cap,"Send failed (0x%x)",rc);return -1;}
  int32_t code=0;
  rc=sceHttpGetStatusCode(req,&code);
  if(rc<0){sceHttpDeleteRequest(req);sceHttpDeleteConnection(conn);snprintf(err,err_cap,"Status read failed (0x%x)",rc);return -1;}

  if(code==301||code==302||code==303||code==307||code==308){
   char*hdrs=0;size_t hlen=0;char next_url[1024];
   int hrc=sceHttpGetAllResponseHeaders(req,&hdrs,&hlen);
   int got=(hrc>=0&&hdrs&&bta_extract_location_header(hdrs,hlen,next_url,sizeof(next_url))==0);
   sceHttpDeleteRequest(req);sceHttpDeleteConnection(conn);
   if(!got){snprintf(err,err_cap,"Redirect missing Location header");return -1;}
   snprintf(url,sizeof(url),"%s",next_url);
   continue;
  }

  *conn_out=conn;*req_out=req;*status_out=code;
  return 0;
 }
 snprintf(err,err_cap,"Too many redirects");
 return -1;
}

static void*update_check_thread(void*arg){
 (void)arg;
 int res=BTA_UPD_NONE;
 struct http_ctx ctx;
 int conn=-1,req=-1;
 char*buf=0;
 char err[128]={0};
 g_upd_latest[0]=0;
 g_upd_log[0]=0;
 g_upd_pkg_name[0]=0;
 g_upd_pkg_url[0]=0;
 g_upd_pkg_size=0;

 if(open_http_ctx(&ctx,"bta_upd",5000000,err,sizeof(err))<0){
  snprintf(g_upd_log,sizeof(g_upd_log),"update: %s",err);
  goto done;
 }

 int32_t status_code=0;
 if(http_get_follow(ctx.tpl,BTA_GITHUB_LATEST_URL,"application/vnd.github+json",&conn,&req,&status_code,err,sizeof(err))<0){
  snprintf(g_upd_log,sizeof(g_upd_log),"update: %s",err);
  goto done;
 }
 if(status_code!=200){snprintf(g_upd_log,sizeof(g_upd_log),"update: http=%d",(int)status_code);goto done;}

 buf=malloc(65536);
 if(!buf){snprintf(g_upd_log,sizeof(g_upd_log),"update: oom");goto done;}
 size_t total=0;
 while(total+1<65536){
  int n=sceHttpReadData(req,buf+total,(uint32_t)(65535-total));
  if(n<=0)break;
  total+=(size_t)n;
 }
 buf[total]=0;

 char tag[64];
 if(bta_extract_tag_name(buf,total,tag,sizeof(tag))<0){
  snprintf(g_upd_log,sizeof(g_upd_log),"update: tag_name missing (%zu bytes)",total);
  goto done;
 }
 bta_format_v(tag,g_upd_latest,sizeof(g_upd_latest));
 int cmp=bta_compare_versions(tag,BTA_VERSION);
 if(cmp>0){
  res=BTA_UPD_OUTDATED;
  unsigned long long psz=0;
  bta_extract_pkg_asset(buf,total,g_upd_pkg_name,sizeof(g_upd_pkg_name),g_upd_pkg_url,sizeof(g_upd_pkg_url),&psz);
  g_upd_pkg_size=(uint64_t)psz;
 }else if(cmp==-1)res=BTA_UPD_MISMATCH;
 else if(cmp==0)res=BTA_UPD_LATEST;
 snprintf(g_upd_log,sizeof(g_upd_log),"update: current=v%s latest=%s state=%d pkg=%s",BTA_VERSION,g_upd_latest,res,g_upd_pkg_name[0]?g_upd_pkg_name:"none");

done:
 free(buf);
 if(req>=0)sceHttpDeleteRequest(req);
 if(conn>=0)sceHttpDeleteConnection(conn);
 close_http_ctx(&ctx);
 g_upd_result=res;
 __atomic_store_n(&g_upd_ready,1,__ATOMIC_RELEASE);
 __atomic_store_n(&g_upd_active,0,__ATOMIC_RELEASE);
 return 0;
}

static void start_update_check(void){
 if(__atomic_load_n(&g_upd_active,__ATOMIC_ACQUIRE)||__atomic_load_n(&g_dl_active,__ATOMIC_ACQUIRE))return;
 __atomic_store_n(&g_upd_ready,0,__ATOMIC_RELEASE);
 __atomic_store_n(&g_upd_active,1,__ATOMIC_RELEASE);
 OrbisPthread th=0;
 int rc=scePthreadCreate(&th,0,update_check_thread,0,"BTAudio4Update");
 if(rc==0)scePthreadDetach(th);
 else{__atomic_store_n(&g_upd_active,0,__ATOMIC_RELEASE);logline("update: thread start failed");}
}

static void*pkg_download_thread(void*arg){
 (void)arg;
 struct http_ctx ctx;
 int conn=-1,req=-1,fd=-1,final_state=BTA_DL_ERROR;
 char*chunk=0;
 char tmp_path[256],final_path[256],err[160]={0};
 snprintf(tmp_path,sizeof(tmp_path),"/data/pkg/%s.part",g_dl_pkg_name);
 snprintf(final_path,sizeof(final_path),"/data/pkg/%s",g_dl_pkg_name);
 g_dl_err[0]=0;
 g_dl_log[0]=0;

 if(open_http_ctx(&ctx,"bta_dl",10000000,err,sizeof(err))<0){
  snprintf(g_dl_err,sizeof(g_dl_err),"Network initialization failed (%s).",err);
  goto done;
 }

 int32_t status_code=0;
 if(http_get_follow(ctx.tpl,g_dl_pkg_url,0,&conn,&req,&status_code,err,sizeof(err))<0){
  snprintf(g_dl_err,sizeof(g_dl_err),"Could not connect to GitHub (%s).",err);
  goto done;
 }
 if(status_code!=200){
  snprintf(g_dl_err,sizeof(g_dl_err),"GitHub returned HTTP %d.",(int)status_code);
  goto done;
 }

 int len_type=0;uint64_t content_len=0;
 if(sceHttpGetResponseContentLength(req,&len_type,&content_len)>=0&&len_type==ORBIS_HTTP_CONTENTLEN_EXIST&&content_len>0){
  __atomic_store_n(&g_dl_total,content_len,__ATOMIC_RELAXED);
 }

 sceKernelMkdir("/data/pkg",0777);
 fd=sceKernelOpen(tmp_path,0x601,0644);
 if(fd<0){
  snprintf(g_dl_err,sizeof(g_dl_err),"Could not create %s (0x%x).",tmp_path,fd);
  goto done;
 }

 chunk=malloc(32768);
 if(!chunk){snprintf(g_dl_err,sizeof(g_dl_err),"Out of memory.");goto done;}

 uint64_t written=0;
 int cancelled=0,read_err=0,write_err=0;
 for(;;){
  if(__atomic_load_n(&g_dl_cancel,__ATOMIC_ACQUIRE)){cancelled=1;break;}
  int n=sceHttpReadData(req,chunk,32768);
  if(n<0){read_err=n;break;}
  if(n==0)break;
  size_t off=0;
  while(off<(size_t)n){
   ssize_t w=(ssize_t)sceKernelWrite(fd,chunk+off,(size_t)n-off);
   if(w<=0){write_err=(int)w;break;}
   off+=(size_t)w;
  }
  if(write_err)break;
  written+=(uint64_t)n;
  __atomic_store_n(&g_dl_bytes,written,__ATOMIC_RELAXED);
 }

 sceKernelClose(fd);fd=-1;
 if(cancelled){
  sceKernelUnlink(tmp_path);
  final_state=BTA_DL_NONE;
  snprintf(g_dl_log,sizeof(g_dl_log),"download: cancelled");
  goto done;
 }
 if(read_err<0){
  sceKernelUnlink(tmp_path);
  snprintf(g_dl_err,sizeof(g_dl_err),"Download interrupted (0x%x).",read_err);
  goto done;
 }
 if(write_err){
  sceKernelUnlink(tmp_path);
  snprintf(g_dl_err,sizeof(g_dl_err),"Disk write failed (%d).",write_err);
  goto done;
 }
 if(written<1024||(len_type==ORBIS_HTTP_CONTENTLEN_EXIST&&content_len>0&&written!=content_len)){
  sceKernelUnlink(tmp_path);
  snprintf(g_dl_err,sizeof(g_dl_err),"Incomplete package downloaded (%llu bytes).",(unsigned long long)written);
  goto done;
 }

 sceKernelUnlink(final_path);
 int rrc=sceKernelRename(tmp_path,final_path);
 if(rrc<0){
  sceKernelUnlink(tmp_path);
  snprintf(g_dl_err,sizeof(g_dl_err),"Could not save %s (0x%x).",final_path,rrc);
  goto done;
 }

 final_state=BTA_DL_DONE;
 snprintf(g_dl_log,sizeof(g_dl_log),"download: saved %s (%llu bytes)",final_path,(unsigned long long)written);

done:
 if(fd>=0){sceKernelClose(fd);sceKernelUnlink(tmp_path);}
 free(chunk);
 if(req>=0)sceHttpDeleteRequest(req);
 if(conn>=0)sceHttpDeleteConnection(conn);
 close_http_ctx(&ctx);
 if(final_state==BTA_DL_ERROR&&!g_dl_log[0])snprintf(g_dl_log,sizeof(g_dl_log),"download: error: %s",g_dl_err);
 __atomic_store_n(&g_dl_state,final_state,__ATOMIC_RELEASE);
 __atomic_store_n(&g_dl_finished,1,__ATOMIC_RELEASE);
 __atomic_store_n(&g_dl_active,0,__ATOMIC_RELEASE);
 return 0;
}

static void start_pkg_download(void){
 if(!g_upd_pkg_url[0]||!g_upd_pkg_name[0])return;
 if(__atomic_load_n(&g_dl_active,__ATOMIC_ACQUIRE))return;
 dl_modal_dismissed=0;
 snprintf(g_dl_pkg_name,sizeof(g_dl_pkg_name),"%s",g_upd_pkg_name);
 snprintf(g_dl_pkg_url,sizeof(g_dl_pkg_url),"%s",g_upd_pkg_url);
 __atomic_store_n(&g_dl_cancel,0,__ATOMIC_RELEASE);
 __atomic_store_n(&g_dl_finished,0,__ATOMIC_RELEASE);
 __atomic_store_n(&g_dl_bytes,0,__ATOMIC_RELAXED);
 __atomic_store_n(&g_dl_total,g_upd_pkg_size,__ATOMIC_RELAXED);
 __atomic_store_n(&g_dl_state,BTA_DL_RUNNING,__ATOMIC_RELEASE);
 __atomic_store_n(&g_dl_active,1,__ATOMIC_RELEASE);
 snprintf(dl_pkg_name,sizeof(dl_pkg_name),"%s",g_upd_pkg_name);
 dl_bytes=0;
 dl_total=g_upd_pkg_size;
 dl_err[0]=0;
 dl_state=BTA_DL_RUNNING;

 OrbisPthread th=0;
 int rc=scePthreadCreate(&th,0,pkg_download_thread,0,"BTAudio4Download");
 if(rc==0){scePthreadDetach(th);logline("download: started");}
 else{
  __atomic_store_n(&g_dl_active,0,__ATOMIC_RELEASE);
  __atomic_store_n(&g_dl_state,BTA_DL_ERROR,__ATOMIC_RELEASE);
  snprintf(dl_err,sizeof(dl_err),"Could not start download thread (0x%x).",rc);
  dl_state=BTA_DL_ERROR;
  logline("download: thread start failed");
 }
}

int main(void){
 sceKernelMkdir("/data/bta_audio",0777);sceKernelChmod("/data/bta_audio",0777);log_rotate();g_start=sceKernelGetProcessTime();g_now=g_start;logline("BTAudio4 " BTA_VERSION " native app starting");
 int video=sceVideoOutOpen(0xff,0,0,0);if(video<0){logline("VideoOutOpen failed");return 1;}
 off_t memory;size_t size=8*1024*1024;void*base=0;
 if(sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),size,0x200000,3,&memory)<0||sceKernelMapDirectMemory(&base,size,0x33,0,memory,0x200000)<0){logline("Video memory failed");return 2;}
 void*buffers[2]={base,(char*)base+WIDTH*HEIGHT*4};OrbisVideoOutBufferAttribute attr;
 sceVideoOutSetBufferAttribute(&attr,0x80000000,1,0,WIDTH,HEIGHT,WIDTH);
 if(sceVideoOutRegisterBuffers(video,0,buffers,2,&attr)<0){logline("Register buffers failed");return 3;}
 sceVideoOutSetFlipRate(video,1);
 OrbisUserServiceInitializeParams userparams={ORBIS_KERNEL_PRIO_FIFO_LOWEST};
 int user_rc=sceUserServiceInitialize(&userparams);int pad_rc=scePadInit();
 char diag[128];snprintf(diag,sizeof(diag),"User init=0x%x Pad init=0x%x",user_rc,pad_rc);logline(diag);
 sceSystemServiceHideSplashScreen();
 logline("Checking audio service");service_check();start_service();
 start_update_check();
 logline("Starting display loop");
 focus=0;for(int i=0;i<state.count;i++)if(state.devices[i].address==state.selected)focus=i;
 unsigned previous=0;int buffer=0,armed=0;long flip=0;uint64_t poll=0,repeat=0,first_draw=0,snap_cd=0,next_upd=sceKernelGetProcessTime()+60000000ULL;int pinmode=0,pinignore=0,digit=0,auto_snapped=0;char pin[5]="0000";
 for(;;){
  uint64_t now=sceKernelGetProcessTime();g_now=now;
  if(now>=next_upd){next_upd=now+60000000ULL;start_update_check();}
  if(__atomic_exchange_n(&g_upd_ready,0,__ATOMIC_ACQ_REL)){
   if(g_upd_result!=BTA_UPD_NONE){
    snprintf(upd_latest,sizeof(upd_latest),"%s",g_upd_latest);
    upd_state=g_upd_result;
    upd_has_pkg=(g_upd_pkg_url[0]!=0);
    if(g_upd_pkg_name[0]&&!__atomic_load_n(&g_dl_active,__ATOMIC_ACQUIRE))snprintf(dl_pkg_name,sizeof(dl_pkg_name),"%s",g_upd_pkg_name);
   }
   if(g_upd_log[0]&&strcmp(g_upd_log,last_upd_log)!=0){snprintf(last_upd_log,sizeof(last_upd_log),"%s",g_upd_log);logline(g_upd_log);}
  }
  if(!dl_modal_dismissed&&(__atomic_load_n(&g_dl_active,__ATOMIC_ACQUIRE)||__atomic_load_n(&g_dl_finished,__ATOMIC_ACQUIRE))){
   dl_bytes=__atomic_load_n(&g_dl_bytes,__ATOMIC_RELAXED);
   dl_total=__atomic_load_n(&g_dl_total,__ATOMIC_RELAXED);
   dl_state=__atomic_load_n(&g_dl_state,__ATOMIC_ACQUIRE);
  }
  if(__atomic_exchange_n(&g_dl_finished,0,__ATOMIC_ACQ_REL)){
   snprintf(dl_err,sizeof(dl_err),"%s",g_dl_err);
   if(!dl_modal_dismissed)dl_state=__atomic_load_n(&g_dl_state,__ATOMIC_ACQUIRE);
   if(g_dl_log[0])logline(g_dl_log);
  }
  int l=ui_link();
  if(now>=poll){refresh();poll=now+(!online?2000000:(l==BT_LINK_STREAMING?300000:500000));}
  if(!state.pin_pending)pinignore=0;if(state.pin_pending&&!pinmode&&!pinignore){pinmode=1;digit=0;memcpy(pin,"0000",5);}
  unsigned buttons=controller(now);
  /* A button still held from launching or resuming the app is not a new press. */
  if(!pad_connected)armed=0;
  if(!armed&&pad_connected){previous=buttons;armed=1;}
  unsigned pressed=buttons&~previous;
  if(buttons==previous&&now>=repeat){pressed=buttons&(ORBIS_PAD_BUTTON_UP|ORBIS_PAD_BUTTON_DOWN|ORBIS_PAD_BUTTON_LEFT|ORBIS_PAD_BUTTON_RIGHT|ORBIS_PAD_BUTTON_L1|ORBIS_PAD_BUTTON_R1);repeat=now+150000;}
  else if(buttons!=previous)repeat=now+450000;
  previous=buttons;
  if(!armed)pressed=0;
  if(pinmode){
   if(pressed&ORBIS_PAD_BUTTON_LEFT)digit=(digit+3)%4;if(pressed&ORBIS_PAD_BUTTON_RIGHT)digit=(digit+1)%4;
   if(pressed&ORBIS_PAD_BUTTON_UP)pin[digit]=(char)('0'+(pin[digit]-'0'+1)%10);if(pressed&ORBIS_PAD_BUTTON_DOWN)pin[digit]=(char)('0'+(pin[digit]-'0'+9)%10);
   if(pressed&ORBIS_PAD_BUTTON_CIRCLE){pinmode=0;pinignore=1;}
   if(pressed&ORBIS_PAD_BUTTON_CROSS){char path[40];snprintf(path,sizeof(path),"/api/pin/%s",pin);logline("action: pin");action(path);pinmode=0;pinignore=1;}
  }else if(dl_state==BTA_DL_RUNNING){
   if(pressed&ORBIS_PAD_BUTTON_CIRCLE){__atomic_store_n(&g_dl_cancel,1,__ATOMIC_RELEASE);dl_state=BTA_DL_NONE;dl_modal_dismissed=1;logline("download: cancel requested");}
  }else if(dl_state==BTA_DL_DONE||dl_state==BTA_DL_ERROR){
   int prev_dl=dl_state;
   if(pressed&(ORBIS_PAD_BUTTON_CROSS|ORBIS_PAD_BUTTON_CIRCLE)){dl_state=BTA_DL_NONE;dl_modal_dismissed=1;}
   else if(prev_dl==BTA_DL_ERROR&&(pressed&ORBIS_PAD_BUTTON_R2)){start_pkg_download();}
  }else{
   if(pressed&ORBIS_PAD_BUTTON_CIRCLE)break;
   if((pressed&ORBIS_PAD_BUTTON_R2)&&upd_state==BTA_UPD_OUTDATED&&upd_has_pkg){start_pkg_download();}
   if(state.count>0){
    if(pressed&ORBIS_PAD_BUTTON_DOWN)focus=(focus+1)%state.count;
    if(pressed&ORBIS_PAD_BUTTON_UP)focus=(focus+state.count-1)%state.count;
   }
   if(pressed&ORBIS_PAD_BUTTON_CROSS){logline("action: connect/disconnect");activate();}
   if(pressed&ORBIS_PAD_BUTTON_TRIANGLE){if(!online){logline("action: scan ignored (service offline)");}else if(l!=BT_LINK_IDLE){logline("action: disconnect");action("/api/stop");}else{logline("action: scan");action("/api/scan");}}
   if(pressed&ORBIS_PAD_BUTTON_SQUARE){if(!online){logline("action: tv ignored (service offline)");}else{logline("action: tv");action(state.tv?"/api/tv-off":"/api/tv-on");}}
   if(pressed&ORBIS_PAD_BUTTON_OPTIONS){if(!online){logline("action: test ignored (service offline)");}else{logline("action: test sound");action("/api/test");if(!notice[0])testing_until=g_now+2200000;}}
   if(pressed&(ORBIS_PAD_BUTTON_L1|ORBIS_PAD_BUTTON_LEFT))volume_change(-5);
   if(pressed&(ORBIS_PAD_BUTTON_R1|ORBIS_PAD_BUTTON_RIGHT))volume_change(5);
  }
  if(focus<0||focus>=state.count)focus=0;
  if(focus<top)top=focus;if(focus>=top+5)top=focus-4;if(top<0)top=0;
  {int target=(ext_ok&&ui_link()==BT_LINK_STREAMING)?state.meter:0;if(target<0)target=0;if(target>100)target=100;
   if(target>meter_s)meter_s+=(target-meter_s+1)/2;else meter_s-=(meter_s-target+3)/4;}
  if(notice[0]&&g_now>=notice_until)notice[0]=0;
  draw(pinmode,pin,digit);
  memcpy(buffers[buffer],back,sizeof(back));
  if(!first_draw)first_draw=now;
  if(!auto_snapped&&now-first_draw>2000000){snapshot();auto_snapped=1;}
  if((buttons&(ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_R3))==(ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_R3)&&now>=snap_cd){snapshot();snap_cd=now+2000000;}
  if(sceVideoOutSubmitFlip(video,buffer,1,flip)<0){sceKernelUsleep(33000);continue;}
  for(int tries=0;tries<1000;tries++){OrbisVideoOutFlipStatus status;if(!sceVideoOutGetFlipStatus(video,&status)&&status.flipArg==flip)break;sceKernelUsleep(1000);}
  buffer^=1;flip++;
 }
 logline("Native app closed; audio service remains running");if(pad>=0)scePadClose(pad);sceVideoOutClose(video);return 0;
}
