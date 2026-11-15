#include "runtime.h"
#include "web_page.h"
#include "protocol.h"
#include "kresolve.h"
#include "../version.h"
int bta_apply_patch(void);

#define WORKER_LOG "/data/bta_audio/app.log"

/* Keep HDMI unmuted for this long after streaming starts so the system audio
 * mix that feeds /shm_acap.full actually activates, THEN apply the user's
 * mute preference. Starting muted leaves the capture ring empty (silence). */
#define PRIME_US 2000000
/* One cycle of a sine at ~8000/32767 (~24%) full scale for the safe test tone. */
static const short SINE32[32]={0,1561,3061,4445,5657,6652,7391,7846,8000,7846,7391,6652,5657,4445,3061,1561,0,-1561,-3061,-4445,-5657,-6652,-7391,-7846,-8000,-7846,-7391,-6652,-5657,-4445,-3061,-1561};

struct audio_req{u64 address;void*samples;u64 length;u32 mode,pad;};
struct client{long fd;u64 deadline;u32 used,sent,total;char input[2048];char output[32768];};
struct app{
 int(*sleep)(unsigned);u64(*clock)(void);int(*shm)(const char*,int,int);
 void*(*map)(void*,u64,int,int,int,u64);int(*unmap)(void*,u64);
 long listener,bt,hdmi,shmfd;u8*ring;u32 readpos;int wanted,streaming,tv,muted,quit,phase;
 long patch_rc,packet_rc,connect_rc,last_rc,hdmi_rc,btstate,list_rc;
 u64 next_try,deadline,scan_at,heartbeat,poll_at,net_at,packets,errors,resyncs;
 unsigned level;int volume;char message[128];
 /* Per-packet timing, summarised per second (GET /api/timing). gap: time between two
  * sends (4 ms is on time). send: how long the driver call took. backlog: capture audio
  * waiting to be sent. refused: the driver returned 0. failed: it returned an error. */
 struct second{u32 seq,t,pkts,refused,failed,late8,late16,late50,maxgap_us,maxsend_us,maxback_ms,peak;}secs[128],cur;
 u32 sec_seq;u64 sec_start,last_send;int joint;u64 refused_since;u32 relinks,relinks_total;
 int auto_recover,recovery_warned;unsigned recover_ms,recover_limit,sender_sleep_us;
 u64 stream_at,test_until,test_next;u32 test_phase;
 void*mutex;int(*mlock)(void**);int(*munlock)(void**);volatile int sender_run,sender_alive;int sender_prio;long thread_rc,prio_rc,aff_rc,pi_rc;
 /* Truthful link state (see link_state). dropped: the link was lost while
  * streaming. failed: the last attempt failed. announced: a "disconnected"
  * notice was shown. session: this connection has already been announced. */
 int dropped,failed,announced,session,meter;u32 attempts;u64 ok_at,meter_at,notify_at[3];unsigned meter_peak;
 struct bt_device target;struct bt_devices devices;int scanning,pin_pending;u64 scan_end,list_at;
 struct client clients[4];
 /* Buffered HDD-safe diagnostic log (flushed outside mutex on threshold/interval). */
 char logbuf[3072];
 unsigned logused,log_dropped;
 int log_urgent,koff_logged,patch_logged,refused_warned,err_warned,no_widen;
 u64 log_flush_at,log_urgent_at,log_file_bytes,health_at,health_warn_at,resync_warn_at;
 u64 bt_ev_last_at;u8 bt_ev_last_type,bt_ev_last_sub;u32 bt_ev_last_data;u64 bt_ev_last_addr;
 u32 health_resyncs,health_ok_skips;
 struct second health;
};
static void lock_state(struct app*a){if(a->mlock)a->mlock(&a->mutex);}
static void unlock_state(struct app*a){if(a->munlock)a->munlock(&a->mutex);}
__attribute__((noinline)) static void wlog_raw(struct app*a,const char*s){while(*s&&a->logused+1<sizeof(a->logbuf))a->logbuf[a->logused++]=*s++;a->logbuf[a->logused]=0;}
__attribute__((noinline)) static void wlog_dec(struct app*a,long n){char b[24];unsigned i=0;u64 v;if(n<0){wlog_raw(a,"-");v=0-(u64)n;}else v=(u64)n;do{b[i++]=(char)('0'+v%10);v/=10;}while(v);while(i){char s[2]={b[--i],0};wlog_raw(a,s);}}
__attribute__((noinline)) static void wlog_hex(struct app*a,u64 v){const char*h="0123456789abcdef";wlog_raw(a,"0x");char b[16];int i=0;do{b[i++]=h[v&15];v>>=4;}while(v);while(i){char s[2]={b[--i],0};wlog_raw(a,s);}}
__attribute__((noinline)) static void wlog_mac(struct app*a,u64 mac){const char*h="0123456789ABCDEF";for(int i=0;i<12;i++){char s[2]={h[(mac>>(44-4*i))&15],0};wlog_raw(a,s);}}
__attribute__((noinline)) static void wlog_str(struct app*a,const char*s){wlog_raw(a,"\"");for(unsigned i=0;s[i]&&i<80;i++){char c=s[i];if(c=='"'||c=='\\')wlog_raw(a,"\\");char b[2]={(c>=32&&c<127)?c:'?',0};wlog_raw(a,b);}wlog_raw(a,"\"");}
__attribute__((noinline)) static int wlog_begin(struct app*a,u64 now,const char*tag){
 if(sizeof(a->logbuf)-a->logused<280){a->log_dropped++;return 0;}
 if(a->log_dropped){wlog_raw(a,"[dropped=");wlog_dec(a,(long)a->log_dropped);wlog_raw(a,"]\n");a->log_dropped=0;}
 u64 tenths=now/100000;
 wlog_raw(a,"[+");wlog_dec(a,(long)(tenths/10));wlog_raw(a,".");wlog_dec(a,(long)(tenths%10));wlog_raw(a,"s] ");wlog_raw(a,tag);wlog_raw(a,": ");
 return 1;
}
__attribute__((noinline)) static void wlog_end(struct app*a,int urgent){wlog_raw(a,"\n");if(urgent)a->log_urgent=1;}
__attribute__((noinline)) static void wlog_koff(struct app*a,u64 now,int force){
 const struct bt_koff*k=bt_koff_info();
 if(!k||(!force&&a->koff_logged&&k->valid))return;
 a->koff_logged=1;
 if(wlog_begin(a,now,k->valid?"koff":"ERR koff")){
  wlog_raw(a,"valid=");wlog_dec(a,(long)k->valid);wlog_raw(a," fail_stage=");wlog_dec(a,(long)k->fail_stage);
  wlog_raw(a," list_rc=");wlog_dec(a,a->list_rc);wlog_raw(a," base=");wlog_hex(a,k->base);
  wlog_raw(a," accept=");wlog_hex(a,k->accept_site);wlog_raw(a," copyout=");wlog_hex(a,k->copyout_fn);
  wlog_raw(a," lock=");wlog_hex(a,k->lock_fn);wlog_raw(a," unlock=");wlog_hex(a,k->unlock_fn);
  wlog_raw(a," cmd=");wlog_hex(a,k->cmd_fn);wlog_raw(a," cache=");wlog_hex(a,k->cache_fn);wlog_raw(a," ctx=");wlog_hex(a,k->ctx_ptr);
  wlog_end(a,!k->valid||a->list_rc!=0);
 }
 if(k->valid&&wlog_begin(a,now,"koff_layout")){
  wlog_raw(a,"cache_off=");wlog_hex(a,k->cache_off);wlog_raw(a," cache_end=");wlog_hex(a,k->cache_end);wlog_raw(a," stride=");wlog_hex(a,k->cache_stride);
  wlog_raw(a," conn_dev=");wlog_hex(a,k->conn_dev_off);wlog_raw(a," st=");wlog_hex(a,k->conn_state_off);
  wlog_raw(a," pol=");wlog_hex(a,k->conn_policy_off);wlog_raw(a," hdl=");wlog_hex(a,k->conn_handle_off);
  wlog_end(a,0);
 }
}
static void wlog_flush(struct app*a,u64 now,int force){
 char tmp[3072];unsigned n=0;
 lock_state(a);
 if(a->logused){
  int due=force||(a->logused>=1536)||(a->log_urgent&&now>=a->log_urgent_at)||(now>=a->log_flush_at);
  if(due){n=a->logused;memcpy(tmp,a->logbuf,n);a->logused=0;a->log_urgent=0;a->log_flush_at=now+15000000;a->log_urgent_at=now+2000000;}
 }
 unlock_state(a);
 if(!n)return;
 long fd=sc3(5,(long)WORKER_LOG,0x209,0644);
 if(fd>=0){long w=sc3(4,fd,(long)tmp,n);closefd(fd);if(w>0)a->log_file_bytes+=(u64)w;}
}
static void message(struct app*a,const char*s){memset(a->message,0,sizeof(a->message));u64 n=length(s);if(n>127)n=127;memcpy(a->message,s,n);}
/* pre + device name (clipped to 40 bytes) + suf, always NUL-terminated within cap. */
static u64 compose(char*out,u64 cap,const char*pre,const char*name,const char*suf){
 u64 n=0;
 while(*pre&&n+1<cap)out[n++]=*pre++;
 for(u64 k=0;name[k]&&k<40&&n+1<cap;k++)out[n++]=name[k];
 while(*suf&&n+1<cap)out[n++]=*suf++;
 out[n]=0;return n;
}
static const char*target_name(struct app*a){return a->target.name[0]?a->target.name:"your headphones";}
static void say(struct app*a,const char*pre,const char*suf){char b[128];compose(b,sizeof(b),pre,target_name(a),suf);message(a,b);}
/* On-screen notices only on real transitions; a flapping link must not spam them. */
static int may_announce(struct app*a,int kind){u64 now=a->clock();if(a->notify_at[kind]&&now-a->notify_at[kind]<10000000)return 0;a->notify_at[kind]=now;return 1;}
static void mute(struct app*a,int on){
 if(a->hdmi<0||a->muted==on)return;int v=on;a->hdmi_rc=sc3(54,a->hdmi,0xc0048d05,(long)&v);
 if(!a->hdmi_rc)a->muted=on;
 long result[4]={a->hdmi,a->hdmi_rc,on,v};put("/data/bt_hdmi_mute_status.bin",result,sizeof(result));
 if(wlog_begin(a,a->clock(),a->hdmi_rc?"WARN mute":"mute")){wlog_raw(a,"on=");wlog_dec(a,on);wlog_raw(a," hdmi_rc=");wlog_dec(a,a->hdmi_rc);wlog_end(a,a->hdmi_rc!=0);}
}
static void stop_audio(struct app*a){
 int was_streaming=a->streaming;
 if(a->phase==4&&a->bt>=0){sc3(54,a->bt,0x20046205,0);a->scanning=0;}
 if(a->streaming){short silence[384];memset(silence,0,sizeof(silence));struct audio_req q={a->target.address,silence,sizeof(silence),0,0};sc3(54,a->bt,0x80206216,(long)&q);}
 a->streaming=0;a->phase=0;a->level=0;a->meter=0;a->meter_peak=0;a->pin_pending=0;a->test_until=0;mute(a,0);
 if(a->ring){a->unmap(a->ring,0x10000);a->ring=0;}closefd(a->shmfd);a->shmfd=-1;
 if(was_streaming&&wlog_begin(a,a->clock(),"audio_stop")){wlog_raw(a,"packets=");wlog_dec(a,(long)a->packets);wlog_raw(a," resyncs=");wlog_dec(a,(long)a->resyncs);wlog_raw(a," relinks=");wlog_dec(a,(long)a->relinks_total);wlog_end(a,1);}
}
/* A failed attempt: stop, explain, and try again after delay_us. */
static void retry_after(struct app*a,const char*s,u64 delay_us){
 stop_audio(a);message(a,s);a->failed=1;a->next_try=a->clock()+delay_us;
 if(wlog_begin(a,a->clock(),"WARN retry")){wlog_raw(a,"delay_ms=");wlog_dec(a,(long)(delay_us/1000));wlog_raw(a," msg=");wlog_str(a,s);wlog_end(a,1);}
}
static void retry(struct app*a,const char*s){retry_after(a,s,10000000);}
static void retry_named(struct app*a,const char*pre,const char*suf,u64 delay_us){char m[128];compose(m,sizeof(m),pre,target_name(a),suf);retry_after(a,m,delay_us);}
/* The PS4 audio stream was rebuilt (e.g. a game started). The headphone link is
 * fine, so this is a quiet reconnect of the audio path, not a failure or a drop. */
static void restart_audio(struct app*a,const char*s){
 if(!a->auto_recover){stop_audio(a);a->wanted=0;a->failed=1;message(a,"Audio stream changed. Automatic recovery is disabled.");if(wlog_begin(a,a->clock(),"WARN recovery_disabled")){wlog_raw(a,"reason=audio_stream_reset");wlog_end(a,1);}return;}
 stop_audio(a);message(a,s);a->next_try=a->clock()+2000000;
 if(wlog_begin(a,a->clock(),"restart_audio")){wlog_raw(a,"msg=");wlog_str(a,s);wlog_end(a,1);}
}
/* The headphones went away mid-stream. Detected only by consecutive audio-send
 * failures (never by polling the driver while streaming: that starved the
 * controller). stop_audio restores TV sound. */
/* Drop the Bluetooth link itself, not just the audio. The driver keeps per-link stream
 * state (e.g. a stuck format renegotiation) that survives an audio stop. */
static void drop_link(struct app*a){if(a->bt>=0&&a->target.address){u64 address=a->target.address;sc3(54,a->bt,0x8008620b,(long)&address);}}
/* Automatic recovery: the PS4 refused every packet for REFUSED_US. Do what a user would
 * do by hand: drop the link and reconnect. At most 3 times in a row; any accepted packet
 * resets the count. */
#define REFUSED_US 4000000
static void relink(struct app*a){
 a->relinks++;a->relinks_total++;
 if(wlog_begin(a,a->clock(),"WARN relink")){wlog_raw(a,"attempt=");wlog_dec(a,(long)a->relinks);wlog_raw(a," total=");wlog_dec(a,(long)a->relinks_total);wlog_end(a,1);}
 stop_audio(a);drop_link(a);a->phase=0;a->next_try=a->clock()+3000000;a->refused_since=0;
 say(a,"The PS4 stopped accepting audio for ",". Reconnecting...");
}
static void link_lost(struct app*a){
 if(wlog_begin(a,a->clock(),"ERR link_lost")){wlog_raw(a,"errors=");wlog_dec(a,(long)a->errors);wlog_raw(a," last_rc=");wlog_dec(a,a->last_rc);wlog_end(a,1);}
 stop_audio(a);a->dropped=1;a->failed=0;a->next_try=a->clock()+2000000;
 say(a,""," disconnected. TV sound is back. Reconnecting...");
 if(may_announce(a,1)){notify("Bluetooth headphones disconnected. TV sound is back on. Reconnecting...");a->announced=1;}
}
static struct bt_device*device(struct app*a,u64 address){for(unsigned i=0;i<a->devices.count;i++)if(a->devices.items[i].address==address)return &a->devices.items[i];return 0;}
static int ensure_bt(struct app*a){if(a->bt>=0)return 0;a->bt=sc3(5,(long)"/dev/bt",6,0);if(a->bt<0)return -1;u64 mask=0xffffff;sc3(4,a->bt,(long)&mask,8);return 0;}
static void refresh_devices(struct app*a){
 struct bt_devices d;memset(&d,0,sizeof(d));
 int rc=bt_list(&d);
 a->list_rc=rc;
 if(!rc&&d.count<=BT_DEVICE_LIMIT)a->devices=d;
 wlog_koff(a,a->clock(),rc!=0);
 struct bt_device*t=device(a,a->target.address);if(t)a->target=*t;
}
static void end_scan(struct app*a){
 if(a->scanning&&a->bt>=0){
  sc3(54,a->bt,0x20046205,0);
  if(wlog_begin(a,a->clock(),"scan_end")){wlog_raw(a,"devices=");wlog_dec(a,(long)a->devices.count);wlog_end(a,0);}
 }
 a->scanning=0;
}
static int scan(struct app*a){
 if(ensure_bt(a)){if(wlog_begin(a,a->clock(),"ERR scan")){wlog_raw(a,"ensure_bt=-1");wlog_end(a,1);}return -1;}
 long rc=sc3(54,a->bt,0x20046204,0);
 if(rc<0){if(wlog_begin(a,a->clock(),"ERR scan")){wlog_raw(a,"ioctl_rc=");wlog_dec(a,rc);wlog_end(a,1);}return -1;}
 a->scanning=1;a->scan_end=a->clock()+16000000;a->list_at=0;
 if(wlog_begin(a,a->clock(),"scan_start")){wlog_raw(a,"ok");wlog_end(a,0);}
 return 0;
}
struct saved_device{u64 magic;struct bt_device device;};
static int save_target(struct app*a,const struct bt_device*d){
 struct saved_device s={.magic=0x3254454752415442UL,.device=*d};
 if(put("/data/bta_audio/device.tmp",&s,sizeof(s)))return -1;
 if(sc3(128,(long)"/data/bta_audio/device.tmp",(long)"/data/bta_audio/device.bin",0)<0)return -1;
 a->target=*d;return 0;
}
static void load_target(struct app*a){
 a->target.address=0;a->target.services=0;
 long fd=sc3(5,(long)"/data/bta_audio/device.bin",0,0);if(fd<0)return;
 struct saved_device s;long n=sc3(3,fd,(long)&s,sizeof(s));closefd(fd);
 if(n==sizeof(s)&&s.magic==0x3254454752415442UL&&s.device.address&&!(s.device.address>>48)){s.device.name[95]=0;a->target=s.device;}
}
static int save_volume(struct app*a,int volume){
 if(volume<0||volume>100)return -1;unsigned value=(unsigned)volume;
 if(put("/data/bta_audio/volume.tmp",&value,sizeof(value)))return -1;
 if(sc3(128,(long)"/data/bta_audio/volume.tmp",(long)"/data/bta_audio/volume.bin",0)<0)return -1;
 a->volume=volume;return 0;
}
static void load_volume(struct app*a){
 a->volume=40;unsigned value=101;long fd=sc3(5,(long)"/data/bta_audio/volume.bin",0,0);
 if(fd<0)return;long n=sc3(3,fd,(long)&value,sizeof(value));closefd(fd);
 if(n==sizeof(value)&&value<=100)a->volume=(int)value;
}
#define SETTINGS_MAGIC 0x31535442UL
struct saved_settings{u32 magic,version,auto_recover,recover_ms,recover_limit,sender_sleep_us;};
static void load_settings(struct app*a){
 a->auto_recover=1;a->recover_ms=4000;a->recover_limit=3;a->sender_sleep_us=500;
 struct saved_settings s;long fd=sc3(5,(long)"/data/bta_audio/settings.bin",0,0);
 if(fd<0)return;long n=sc3(3,fd,(long)&s,sizeof(s));closefd(fd);
 if(n!=(long)sizeof(s)||s.magic!=SETTINGS_MAGIC||s.version!=1)return;
 if(s.auto_recover<=1)a->auto_recover=(int)s.auto_recover;
 if(s.recover_ms==2000||s.recover_ms==4000||s.recover_ms==6000||s.recover_ms==8000)a->recover_ms=s.recover_ms;
 if(s.recover_limit==1||s.recover_limit==3||s.recover_limit==5)a->recover_limit=s.recover_limit;
 if(s.sender_sleep_us==250||s.sender_sleep_us==500||s.sender_sleep_us==1000||s.sender_sleep_us==2000)a->sender_sleep_us=s.sender_sleep_us;
}
static int save_settings(struct app*a){
 struct saved_settings s={SETTINGS_MAGIC,1,(u32)a->auto_recover,a->recover_ms,a->recover_limit,a->sender_sleep_us};
 if(put("/data/bta_audio/settings.tmp",&s,sizeof(s)))return -1;
 return sc3(128,(long)"/data/bta_audio/settings.tmp",(long)"/data/bta_audio/settings.bin",0)<0?-1:0;
}
static void wlog_patch(struct app*a,u64 now){
 if(a->patch_logged&&!a->patch_rc)return;
 a->patch_logged=1;
 struct{long status;u64 before,after;}pr={0,0,0};
 long fd=sc3(5,(long)"/data/bt_accept_patch_result.bin",0,0);
 if(fd>=0){sc3(3,fd,(long)&pr,sizeof(pr));closefd(fd);}
 if(wlog_begin(a,now,a->patch_rc?"ERR patch":"patch")){
  wlog_raw(a,"rc=");wlog_dec(a,a->patch_rc);wlog_raw(a," status=");wlog_dec(a,pr.status);
  wlog_raw(a," before=");wlog_hex(a,pr.before);wlog_raw(a," after=");wlog_hex(a,pr.after);
  wlog_end(a,a->patch_rc!=0);
 }
}
static void begin_connect(struct app*a){
 u64 now=a->clock();
 a->patch_rc=bta_apply_patch();
 wlog_patch(a,now);
 if(a->patch_rc){retry(a,"Bluetooth setup failed. Check the diagnostic status.");return;}
 if(ensure_bt(a)){retry(a,"Bluetooth is not ready. Retrying shortly.");return;}
 refresh_devices(a);struct bt_device*t=device(a,a->target.address);
 if(!t){if(scan(a)){retry(a,"Bluetooth scan could not start. Retrying shortly.");return;}a->phase=4;a->deadline=a->clock()+16000000;say(a,"Looking for ","... Put it in pairing mode if needed.");return;}
 if(!bt_is_audio(t)){
  if(wlog_begin(a,now,"WARN not_audio")){wlog_raw(a,"mac=");wlog_mac(a,t->address);wlog_raw(a," cod=");wlog_hex(a,t->cod);wlog_raw(a," svc=");wlog_hex(a,t->services);wlog_end(a,1);}
  a->wanted=0;retry(a,"This device does not advertise headphone audio support.");return;
 }
 end_scan(a);u64 address=a->target.address;a->btstate=sc3(54,a->bt,0x80086203,(long)&address);
 a->phase=1;a->deadline=a->clock()+30000000;say(a,"Connecting to ","...");
 if(a->btstate<5||a->btstate>6){sc3(54,a->bt,0x20046205,0);a->connect_rc=sc3(54,a->bt,0x8008620a,(long)&address);
  if(wlog_begin(a,now,a->connect_rc<0?"ERR connect":"connect")){
   wlog_raw(a,"attempt=");wlog_dec(a,(long)a->attempts);wlog_raw(a," mac=");wlog_mac(a,address);
   wlog_raw(a," btstate=");wlog_dec(a,a->btstate);wlog_raw(a," connect_rc=");wlog_dec(a,a->connect_rc);wlog_end(a,a->connect_rc<0);
  }
  /* An immediate driver error (e.g. 0x802F0204) means this device will not
   * connect right now; fail fast instead of waiting out the 30 s window. */
  if(a->connect_rc<0){retry_named(a,"Couldn't connect to ",". Make sure it's on, nearby and not connected to another device.",8000000);return;}
 }else{
  a->connect_rc=0;
  if(wlog_begin(a,now,"connect")){wlog_raw(a,"already_linked mac=");wlog_mac(a,address);wlog_raw(a," btstate=");wlog_dec(a,a->btstate);wlog_end(a,0);}
 }
}
static void start_audio(struct app*a){
 a->shmfd=a->shm("/shm_acap.full",0,0);if(a->shmfd<0){retry(a,"Waiting for the PS4 audio service.");return;}
 void*p=a->map(0,0x10000,1,1,a->shmfd,0);
 if(!p||p==(void*)-1){retry(a,"Unable to open the game audio stream.");return;}a->ring=p;
 a->readpos=*(volatile u32*)(a->ring+12);
 if(*(volatile u32*)a->ring!=0x10000||a->readpos<16||a->readpos>=0x10000||(a->readpos&7)){
  if(wlog_begin(a,a->clock(),"ERR shm_format")){wlog_raw(a,"hdr=");wlog_hex(a,*(volatile u32*)a->ring);wlog_raw(a," readpos=");wlog_hex(a,a->readpos);wlog_end(a,1);}
  retry(a,"The PS4 audio stream has an unexpected format.");return;
 }
 a->streaming=1;a->phase=3;a->refused_since=0;a->errors=0;a->stream_at=a->clock();a->test_until=0;a->test_next=0;a->test_phase=0;
 a->health_at=a->stream_at+10000000;a->health_resyncs=a->resyncs;a->health_ok_skips=0;memset(&a->health,0,sizeof(a->health));a->refused_warned=0;a->err_warned=0;
 int was_dropped=a->dropped,told=a->announced,fresh=!a->session;
 a->attempts=0;a->failed=0;a->dropped=0;a->announced=0;a->session=1;a->ok_at=a->stream_at;a->meter=0;a->meter_peak=0;
 if(wlog_begin(a,a->stream_at,"audio_start")){
  wlog_raw(a,"mac=");wlog_mac(a,a->target.address);wlog_raw(a," name=");wlog_str(a,target_name(a));
  wlog_raw(a," joint=");wlog_dec(a,a->joint);wlog_raw(a," tv=");wlog_dec(a,a->tv);
  wlog_raw(a," vol=");wlog_dec(a,a->volume);wlog_raw(a," readpos=");wlog_dec(a,(long)a->readpos);wlog_end(a,1);
 }
 say(a,"Connected to ",a->tv?". TV sound is on.":". TV sound is muted.");
 if(was_dropped){if(told&&may_announce(a,2))notify("Bluetooth headphones reconnected.");}
 else if(fresh&&may_announce(a,0)){char m[200];compose(m,sizeof(m),"Bluetooth headphones connected: ",target_name(a),a->tv?". TV sound stays on.":". TV sound muted.");notify(m);}
}
static void bluetooth_tick(struct app*a,u64 now){
 /* While audio is streaming, do NOT walk the driver cache: refresh_devices()
  * takes the kernel's Bluetooth link lock, and polling it mid-stream starves
  * the wireless controller's HID link. The proven BTA bridge never did this.
  * Device enumeration resumes as soon as streaming stops. */
 if(!a->streaming&&now>=a->list_at){refresh_devices(a);a->list_at=now+(a->scanning?500000:3000000);}
 if(a->scanning&&now>=a->scan_end)end_scan(a);
 if(a->bt>=0){
  struct{u8 type,subtype;u16 reserved;u32 data;u64 address;}ev[16];long n=sc3(3,a->bt,(long)ev,sizeof(ev));
  if(n>0&&n<=(long)sizeof(ev))for(unsigned i=0;i<(unsigned)n/16;i++){
   int is_target=(ev[i].address==a->target.address);
   int dup=(ev[i].type==a->bt_ev_last_type&&ev[i].subtype==a->bt_ev_last_sub&&ev[i].data==a->bt_ev_last_data&&ev[i].address==a->bt_ev_last_addr&&now-a->bt_ev_last_at<1000000);
   if(!dup){
    a->bt_ev_last_at=now;a->bt_ev_last_type=ev[i].type;a->bt_ev_last_sub=ev[i].subtype;a->bt_ev_last_data=ev[i].data;a->bt_ev_last_addr=ev[i].address;
    if(wlog_begin(a,now,"bt_ev")){
     wlog_raw(a,"type=");wlog_dec(a,ev[i].type);wlog_raw(a," sub=");wlog_dec(a,ev[i].subtype);
     wlog_raw(a," data=");wlog_hex(a,ev[i].data);wlog_raw(a," addr=");wlog_mac(a,ev[i].address);
     wlog_raw(a," target=");wlog_dec(a,is_target);wlog_end(a,is_target);
    }
   }
   if(is_target&&a->wanted){
    if(ev[i].type==4){u64 q[2]={a->target.address,1};sc3(54,a->bt,0x80106208,(long)q);}
    if(ev[i].type==3){a->pin_pending=1;a->deadline=now+60000000;message(a,"These headphones require a pairing PIN.");}
   }
  }
 }
 if(!a->wanted)return;
 if(a->phase==0){if(now>=a->next_try){a->attempts++;a->failed=0;begin_connect(a);}return;}
 if(a->phase==4){
  if(device(a,a->target.address)){end_scan(a);a->phase=0;begin_connect(a);return;}
  if(now>=a->deadline)retry_named(a,"Couldn't find ",". Put it in pairing mode.",8000000);return;
 }
 if(a->phase==1){
  u64 address=a->target.address;a->btstate=sc3(54,a->bt,0x80086203,(long)&address);
  if(a->btstate>=5&&a->btstate<=6){a->pin_pending=0;
   if(exists(NO_WIDEN)){
    a->packet_rc=0;/* diagnostic: leave controller link params untouched */
    if(wlog_begin(a,now,"WARN bt_prepare")){wlog_raw(a,"skipped=NO_WIDEN mac=");wlog_mac(a,address);wlog_raw(a," btstate=");wlog_dec(a,a->btstate);wlog_end(a,1);}
   }else{
    struct bt_prep_diag pd;memset(&pd,0,sizeof(pd));
    a->packet_rc=bt_prepare_diag(address,&pd);
    wlog_koff(a,now,a->packet_rc!=0);
    if(wlog_begin(a,now,a->packet_rc?"ERR bt_prepare":(pd.skipped?"WARN bt_prepare":"bt_prepare"))){
     wlog_raw(a,"rc=");wlog_dec(a,a->packet_rc);wlog_raw(a," skipped=");wlog_dec(a,pd.skipped);
     wlog_raw(a," btstate=");wlog_dec(a,a->btstate);wlog_raw(a," conn_state=");wlog_dec(a,(long)pd.conn_state);
     wlog_raw(a," policy=");wlog_hex(a,pd.conn_policy);wlog_raw(a," hdl=");wlog_hex(a,pd.handle);
     wlog_raw(a," svc=");wlog_hex(a,pd.dev_services);wlog_end(a,a->packet_rc!=0||pd.skipped);
    }
    if(a->packet_rc){retry(a,"No supported Bluetooth audio link. Retrying shortly.");return;}
   }
   a->phase=2;a->deadline=now+400000;}
  else if(now>=a->deadline)retry_named(a,""," didn't respond. Is it on and nearby?",8000000);
 }else if(a->phase==2&&now>=a->deadline)start_audio(a);
}
__attribute__((noinline)) static void wlog_stream_summary(struct app*a,u64 now,const char*tag,const struct second*s,u32 resync_d,int urgent){
 if(wlog_begin(a,now,tag)){
  wlog_raw(a,"pkts=");wlog_dec(a,(long)s->pkts);wlog_raw(a," refused=");wlog_dec(a,(long)s->refused);
  wlog_raw(a," failed=");wlog_dec(a,(long)s->failed);wlog_raw(a," resyncs=+");wlog_dec(a,(long)resync_d);
  wlog_raw(a,"(");wlog_dec(a,(long)a->resyncs);wlog_raw(a,") late8=");wlog_dec(a,(long)s->late8);
  wlog_raw(a," late16=");wlog_dec(a,(long)s->late16);wlog_raw(a," late50=");wlog_dec(a,(long)s->late50);
  wlog_raw(a," maxgap_us=");wlog_dec(a,(long)s->maxgap_us);wlog_raw(a," maxsend_us=");wlog_dec(a,(long)s->maxsend_us);
  wlog_raw(a," maxback_ms=");wlog_dec(a,(long)s->maxback_ms);wlog_raw(a," audio_rc=");wlog_dec(a,a->last_rc);
  if(s->refused)wlog_raw(a," [BT_QUEUE_REFUSED]");
  if(resync_d)wlog_raw(a," [RING_OVERFLOW_DROP]");
  if(s->late50||s->late16)wlog_raw(a," [SENDER_STALLED]");
  wlog_end(a,urgent);
 }
}
static void roll_second(struct app*a,u64 now){
 if(a->sec_start&&now-a->sec_start<1000000)return;
 if(a->sec_start&&(a->cur.pkts||a->cur.maxgap_us)){
  a->cur.seq=++a->sec_seq;a->secs[a->sec_seq%128]=a->cur;
  a->health.pkts+=a->cur.pkts;a->health.refused+=a->cur.refused;a->health.failed+=a->cur.failed;
  a->health.late8+=a->cur.late8;a->health.late16+=a->cur.late16;a->health.late50+=a->cur.late50;
  if(a->cur.maxgap_us>a->health.maxgap_us)a->health.maxgap_us=a->cur.maxgap_us;
  if(a->cur.maxsend_us>a->health.maxsend_us)a->health.maxsend_us=a->cur.maxsend_us;
  if(a->cur.maxback_ms>a->health.maxback_ms)a->health.maxback_ms=a->cur.maxback_ms;
  if(a->cur.peak>a->health.peak)a->health.peak=a->cur.peak;
  if((a->cur.refused||a->cur.failed||a->cur.late50)&&now>=a->health_warn_at){
   a->health_warn_at=now+5000000;
   wlog_stream_summary(a,now,"WARN stream_1s",&a->cur,0,1);
  }
 }
 if(a->health_at&&now>=a->health_at){
  u32 rd=a->resyncs-a->health_resyncs;
  int bad=(a->health.refused||a->health.failed||rd||a->health.late16||a->health.late50);
  if(bad){
   wlog_stream_summary(a,now,"WARN stream_10s",&a->health,rd,1);
   a->health_ok_skips=0;
  }else if(++a->health_ok_skips>=3){
   wlog_stream_summary(a,now,"OK stream_30s",&a->health,0,0);
   a->health_ok_skips=0;
  }
  if(bad||a->health_ok_skips==0){memset(&a->health,0,sizeof(a->health));a->health_resyncs=a->resyncs;}
  a->health_at=now+10000000;
 }
 memset(&a->cur,0,sizeof(a->cur));a->cur.t=(u32)(now/1000000);a->sec_start=now;
}
/* Joint stereo is the default. A headphone can opt out (old dual-channel format) with an
 * empty file /data/bta_audio/dual-off-<address>.
 * Send type 3 makes the driver encode dual-channel SBC at half bitpool per channel, a
 * format headphones did not agree to at connect, so it renegotiates the stream. Type 4
 * is joint stereo at full bitpool, the format headphones normally agree to. */
static void joint_path(u64 address,char out[40]){
 const char*hex="0123456789ABCDEF",*base="/data/bta_audio/dual-off-";unsigned i=0;
 while(base[i]){out[i]=base[i];i++;}
 for(int k=0;k<12;k++)out[i++]=hex[(address>>(44-4*k))&15];
 out[i]=0;
}
static void load_joint(struct app*a){
 if(!a->target.address){a->joint=1;return;}
 char p[40];joint_path(a->target.address,p);a->joint=!exists(p);
}
static void audio_tick(struct app*a,u64 now){
 if(!a->streaming){a->last_send=0;return;}
 roll_second(a,now);
 short pcm[384];unsigned peak=0;unsigned backlog=0;
 if(now<a->test_until){
  /* Self-paced test tone (works even with no game audio): one 192-frame packet
   * every ~4ms. Independent of the capture ring; follows the volume slider
   * (50% = the old fixed level, 100% = about half of full scale). */
  if(now<a->test_next)return;
  a->test_next=now+4000;
  for(unsigned i=0;i<192;i++){short s=(short)(SINE32[(a->test_phase>>8)&31]*2*a->volume/100);a->test_phase+=75;pcm[2*i]=s;pcm[2*i+1]=s;unsigned m=s<0?-s:s;if(m>a->level)a->level=m;if(m>peak)peak=m;}
 }else{
  u32 writepos=(u32)(*(volatile u64*)(a->ring+8)>>32);
  if(writepos<16||writepos>=0x10000||(writepos&7)){
   if(wlog_begin(a,now,"WARN ring_reset")){wlog_raw(a,"writepos=");wlog_hex(a,writepos);wlog_raw(a," readpos=");wlog_hex(a,a->readpos);wlog_end(a,1);}
   restart_audio(a,"Audio stream changed. Reconnecting...");return;
  }
  unsigned available=writepos>=a->readpos?writepos-a->readpos:0xfff0-a->readpos+writepos;
  if(available>0xc000){
   a->readpos=writepos;a->resyncs++;
   if(now>=a->resync_warn_at){a->resync_warn_at=now+5000000;if(wlog_begin(a,now,"WARN resync")){wlog_raw(a,"avail=");wlog_dec(a,(long)available);wlog_raw(a," total=");wlog_dec(a,(long)a->resyncs);wlog_end(a,1);}}
   return;
  }if(available<1536)return;backlog=available;
  u32 cursor=a->readpos;
  for(unsigned i=0;i<384;i++){float f=*(volatile float*)(a->ring+cursor);cursor+=4;if(cursor==0x10000)cursor=16;if(f!=f)f=0;if(f>1)f=1;if(f<-1)f=-1;int v=(int)(f*(32767.0f*(float)a->volume/100.0f));pcm[i]=(short)v;unsigned m=v<0?-v:v;if(m>a->level)a->level=m;if(m>peak)peak=m;}
  a->readpos=cursor;
 }
 struct audio_req req={a->target.address,pcm,sizeof(pcm),a->joint?4:3,0};a->last_rc=sc3(54,a->bt,0x80206216,(long)&req);
 {u64 done=a->clock();struct second*s=&a->cur;s->pkts++;
  if(a->last_rc==0)s->refused++;else if(a->last_rc<0)s->failed++;
  if(a->last_send){u64 gap=now-a->last_send;if(gap>s->maxgap_us)s->maxgap_us=(u32)(gap>0xffffffffUL?0xffffffffUL:gap);if(gap>8000)s->late8++;if(gap>16000)s->late16++;if(gap>50000)s->late50++;}
  if(done-now>s->maxsend_us)s->maxsend_us=(u32)(done-now);if(backlog/384>s->maxback_ms)s->maxback_ms=backlog/384;if(peak>s->peak)s->peak=peak;a->last_send=now;}
 if(a->last_rc==0){if(!a->refused_since)a->refused_since=now;
  else if(now-a->refused_since>100000&&!a->refused_warned){a->refused_warned=1;if(wlog_begin(a,now,"WARN refused_streak")){wlog_raw(a,"ms=");wlog_dec(a,(long)((now-a->refused_since)/1000));wlog_raw(a," back_ms=");wlog_dec(a,(long)(backlog/384));wlog_end(a,1);}}
  else if(now-a->refused_since>(u64)a->recover_ms*1000&&a->auto_recover&&a->relinks<a->recover_limit){relink(a);return;}
  else if(now-a->refused_since>(u64)a->recover_ms*1000&&(!a->auto_recover||a->relinks>=a->recover_limit)&&!a->recovery_warned){
   a->recovery_warned=1;
   if(wlog_begin(a,now,"ERR recovery_paused")){wlog_raw(a,"auto=");wlog_dec(a,a->auto_recover);wlog_raw(a," relinks=");wlog_dec(a,(long)a->relinks);wlog_raw(a," limit=");wlog_dec(a,(long)a->recover_limit);wlog_end(a,1);}
   message(a,a->auto_recover?"Recovery limit reached. Reconnect manually or increase the limit.":"Automatic recovery is off. Use Restart audio or Reconnect.");
  }}
 else if(a->last_rc>0){a->refused_since=0;a->relinks=0;a->refused_warned=0;a->err_warned=0;a->recovery_warned=0;}
 if(a->last_rc<0){
  if(!a->err_warned){a->err_warned=1;if(wlog_begin(a,now,"ERR audio_ioctl")){wlog_raw(a,"rc=");wlog_dec(a,a->last_rc);wlog_raw(a," errors=");wlog_dec(a,(long)(a->errors+1));wlog_end(a,1);}}
  if(++a->errors>20){
   if(a->auto_recover)link_lost(a);
   else{stop_audio(a);a->wanted=0;a->dropped=1;a->failed=1;message(a,"Bluetooth send failed. Automatic recovery is disabled.");if(wlog_begin(a,now,"ERR recovery_disabled")){wlog_raw(a,"reason=audio_ioctl");wlog_raw(a," rc=");wlog_dec(a,a->last_rc);wlog_end(a,1);}}
  }
 }else{a->errors=0;a->packets++;a->ok_at=now;if(peak>a->meter_peak)a->meter_peak=peak;
  /* Keep HDMI capture active for the entire Bluetooth session.
   * Some PS4 titles rebuild their audio mix after entering gameplay; muting
   * HDMI here can collapse the shared capture mix and leave only very quiet
   * samples. For earbud-only listening, mute the monitor physically. */
  if(a->muted)mute(a,0);}
}
/* Bounded JSON writer. Values are numbers and fixed internal strings only. */
struct writer{char*b;unsigned used,cap;};
static void append(struct writer*w,const char*s){while(*s&&w->used+1<w->cap)w->b[w->used++]=*s++;w->b[w->used]=0;}
static void number(struct writer*w,long n){char b[24];unsigned i=0;u64 v;if(n<0){append(w,"-");v=0-(u64)n;}else v=n;do{b[i++]=(char)('0'+v%10);v/=10;}while(v);while(i){char s[2]={b[--i],0};append(w,s);}}
static void field(struct writer*w,const char*key,long value){append(w,",\"");append(w,key);append(w,"\":");number(w,value);}
static void quoted(struct writer*w,const char*s){append(w,"\"");for(unsigned i=0;s[i]&&i<127;i++){char c=s[i];if(c=='"'||c=='\\')append(w,"\\");char b[2]={(c>=32&&c<127)?c:'?',0};append(w,b);}append(w,"\"");}
static void mac_text(u64 address,char out[13]){const char digits[]="0123456789ABCDEF";for(int i=0;i<12;i++)out[i]=digits[(address>>(44-4*i))&15];out[12]=0;}

static u64 parse_mac(const char*p){u64 v=0;for(int i=0;i<12;i++){char c=p[i];unsigned n;if(c>='0'&&c<='9')n=c-'0';else if(c>='A'&&c<='F')n=c-'A'+10;else if(c>='a'&&c<='f')n=c-'a'+10;else return 0;v=(v<<4)|n;}if(!starts(p+12," HTTP/1."))return 0;return v;}
/* The one place the connection's real state is decided. A UI must show this,
 * never infer it from "selected" or "wanted". */
static int link_state(struct app*a){
 if(a->streaming)return BT_LINK_STREAMING;
 if(!a->wanted)return BT_LINK_IDLE;
 if(a->phase==4)return BT_LINK_SEARCHING;
 if(a->dropped)return BT_LINK_LOST;
 if(a->failed)return BT_LINK_FAILED;
 return BT_LINK_CONNECTING;
}
static int retry_in_ms(struct app*a,u64 now){return(a->wanted&&!a->streaming&&!a->phase&&a->next_try>now)?(int)((a->next_try-now)/1000):0;}
static int quiet_for_ms(struct app*a,u64 now){if(!a->streaming||now<a->ok_at)return 0;u64 ms=(now-a->ok_at)/1000;return ms>2000000000UL?2000000000:(int)ms;}
static unsigned status(struct app*a,char*out,unsigned cap){
 u64 now=a->clock();
 struct writer w={out,0,cap};append(&w,"{\"version\":\"" BTA_VERSION "\",\"message\":");quoted(&w,a->message);field(&w,"sender",a->sender_alive);field(&w,"sender_prio",a->sender_prio);field(&w,"thread_rc",a->thread_rc);field(&w,"prio_rc",a->prio_rc);field(&w,"aff_rc",a->aff_rc);field(&w,"pi_rc",a->pi_rc);
 field(&w,"volume",a->volume);field(&w,"auto_recover",a->auto_recover);field(&w,"recover_ms",a->recover_ms);field(&w,"recover_limit",a->recover_limit);field(&w,"sender_sleep_us",a->sender_sleep_us);field(&w,"wanted",a->wanted);field(&w,"streaming",a->streaming);field(&w,"tv_enabled",a->tv);field(&w,"joint",a->joint);field(&w,"send_type",a->joint?4:3);field(&w,"relinks",a->relinks_total);field(&w,"refused_ms",a->refused_since?(long)((now-a->refused_since)/1000):0);field(&w,"hdmi_muted",a->muted==1);field(&w,"scanning",a->scanning);field(&w,"pin_pending",a->pin_pending);field(&w,"test",now<a->test_until);field(&w,"link",link_state(a));field(&w,"attempts",a->attempts);field(&w,"retry_ms",retry_in_ms(a,now));field(&w,"meter",a->meter);field(&w,"quiet_ms",quiet_for_ms(a,now));field(&w,"level",a->level);field(&w,"packets",a->packets);field(&w,"errors",a->errors);field(&w,"resyncs",a->resyncs);field(&w,"patch_rc",a->patch_rc);field(&w,"packet_rc",a->packet_rc);field(&w,"connect_rc",a->connect_rc);field(&w,"audio_rc",a->last_rc);field(&w,"hdmi_rc",a->hdmi_rc);field(&w,"bt_state",a->btstate);field(&w,"no_widen",a->no_widen);field(&w,"uptime_us",a->clock());field(&w,"list_rc",a->list_rc);
 char mac[13];mac_text(a->target.address,mac);append(&w,",\"selected\":");quoted(&w,mac);append(&w,",\"name\":");quoted(&w,a->target.name);
 append(&w,",\"devices\":[");int first=1;
 for(unsigned i=0;i<a->devices.count;i++){struct bt_device*d=&a->devices.items[i];if(!bt_is_audio(d)&&d->address!=a->target.address)continue;
  if(!first)append(&w,",");first=0;mac_text(d->address,mac);append(&w,"{\"address\":");quoted(&w,mac);append(&w,",\"name\":");quoted(&w,d->name);field(&w,"audio",!!(d->services&8));append(&w,"}");}
 append(&w,"]}");return w.used;
}
/* Oldest first: [seq,t,pkts,refused,failed,late8,late16,late50,maxgap_us,maxsend_us,maxback_ms,peak]. */
static unsigned timing_json(struct app*a,char*out,unsigned cap){
 struct writer w={out,0,cap};append(&w,"{\"seq\":");number(&w,a->sec_seq);field(&w,"now_s",(long)(a->clock()/1000000));append(&w,",\"secs\":[");
 u32 n=a->sec_seq<120?a->sec_seq:120;
 for(u32 i=0;i<n;i++){struct second*s=&a->secs[(a->sec_seq-n+1+i)%128];if(i)append(&w,",");u32 v[12]={s->seq,s->t,s->pkts,s->refused,s->failed,s->late8,s->late16,s->late50,s->maxgap_us,s->maxsend_us,s->maxback_ms,s->peak};
  append(&w,"[");for(int k=0;k<12;k++){if(k)append(&w,",");number(&w,v[k]);}append(&w,"]");}
 append(&w,"]}");return w.used;
}
static unsigned native_status(struct app*a,char*out){struct bt_native*n=(void*)out;memset(n,0,sizeof(*n));n->magic=BT_NATIVE_MAGIC;n->version=2;n->volume=a->volume;n->streaming=a->streaming;n->wanted=a->wanted;n->tv=a->tv;n->muted=a->muted==1;n->scanning=a->scanning;n->pin_pending=a->pin_pending;n->selected=a->target.address;memcpy(n->message,a->message,sizeof(n->message));for(unsigned i=0;i<a->devices.count;i++)if(bt_is_audio(&a->devices.items[i])||a->devices.items[i].address==a->target.address)n->devices[n->count++]=a->devices.items[i];
 u64 now=a->clock();n->link=link_state(a);n->attempts=(int)a->attempts;n->retry_ms=retry_in_ms(a,now);n->meter=a->meter;n->quiet_ms=quiet_for_ms(a,now);n->test=now<a->test_until;n->connect_rc=(int)a->connect_rc;n->audio_rc=(int)a->last_rc;
 return sizeof(*n);}
static void response(struct client*c,const char*code,const char*type,const char*body,unsigned n){
 struct writer w={c->output,0,sizeof(c->output)};append(&w,"HTTP/1.1 ");append(&w,code);append(&w,"\r\nContent-Type: ");append(&w,type);append(&w,"\r\nContent-Length: ");number(&w,n);
 append(&w,"\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; connect-src 'self' https://api.github.com; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'\r\n\r\n");
 if(w.used+n>sizeof(c->output)){c->total=0;return;}memcpy(c->output+w.used,body,n);c->total=w.used+n;
}
static char*header(char*request,const char*name){
 char*p=find(request,"\r\n");if(!p)return 0;p+=2;
 while(*p&&*p!='\r'){
  char*q=p;const char*k=name;while(*k){char c=*q;if(c>='A'&&c<='Z')c+=32;if(c!=*k)break;q++;k++;}
  if(!*k&&*q==':'){q++;while(*q==' '||*q=='\t')q++;return q;}
  p=find(p,"\r\n");if(!p)return 0;p+=2;
 }
 return 0;
}
static int host_valid(char*request){
 char*p=header(request,"host");if(!p)return 0;
 u8 ip[4];for(int i=0;i<4;i++){unsigned n=0,digits=0;while(*p>='0'&&*p<='9'){n=n*10+*p++-'0';if(++digits>3||n>255)return 0;}if(!digits)return 0;ip[i]=n;if(i<3&&*p++!='.')return 0;}
 return local_ip(ip)&&starts(p,":8097\r\n");
}
/* Newest 10 KB of app.log, served locally so users do not need FTP. */
static unsigned read_log_tail(char*out,unsigned cap){
 if(!cap)return 0;
 char ring[10000],chunk[512];unsigned used=0,pos=0;
 long fd=sc3(5,(long)WORKER_LOG,0,0);
 if(fd<0){const char*m="No diagnostic log yet. Start the audio service first.\n";unsigned n=length(m);if(n>=cap)n=cap-1;memcpy(out,m,n);out[n]=0;return n;}
 for(;;){long n=sc3(3,fd,(long)chunk,sizeof(chunk));if(n<=0)break;
  for(long i=0;i<n;i++){ring[pos]=chunk[i];pos=(pos+1)%sizeof(ring);if(used<sizeof(ring))used++;}
 }
 closefd(fd);
 unsigned start=used==sizeof(ring)?pos:0,n=used;
 if(n>=cap)n=cap-1;
 if(used>n)start=(start+(used-n))%sizeof(ring);
 for(unsigned i=0;i<n;i++)out[i]=ring[(start+i)%sizeof(ring)];
 out[n]=0;return n;
}
static void diag_item(struct writer*w,int*first,const char*id,const char*state,const char*title,const char*detail,const char*fix){
 if(!*first){append(w,",");}*first=0;append(w,"{\"id\":");quoted(w,id);append(w,",\"state\":");quoted(w,state);
 append(w,",\"title\":");quoted(w,title);append(w,",\"detail\":");quoted(w,detail);append(w,",\"fix\":");quoted(w,fix);append(w,"}");
}
static unsigned diagnostics_json(struct app*a,char*out,unsigned cap){
 struct writer w={out,0,cap};int first=1;append(&w,"{\"version\":\"");append(&w,BTA_VERSION);append(&w,"\",\"tests\":[");
 diag_item(&w,&first,"service","pass","Audio service","The local control service is answering requests.","No action needed.");
 if(a->sender_alive&&a->thread_rc==0)diag_item(&w,&first,"sender",(a->sender_prio==306?"pass":"warn"),"Audio sender thread",a->sender_prio==306?"Dedicated sender thread is alive at priority 306.":"Sender is alive, but its real-time priority is not confirmed.","Review sender scheduling in the report. Start with the default 500 us polling interval.");
 else diag_item(&w,&first,"sender","fail","Audio sender thread","Dedicated sender thread did not start; worker-loop fallback may be active.","Review sender thread and scheduling errors in the report. Restart the payload if the thread remains unavailable.");
 const struct bt_koff*k=bt_koff_info();
 if(k&&k->valid)diag_item(&w,&first,"kernel","pass","Kernel symbol discovery","Kernel functions and Bluetooth offsets were resolved.","No action needed.");
 else diag_item(&w,&first,"kernel",a->wanted?"fail":"wait","Kernel symbol discovery",a->wanted?"Kernel signature discovery failed or is incomplete.":"Not tested yet; connect headphones to trigger the guarded kernel checks.","Do not bypass signature checks. Check firmware compatibility and the koff failure stage in the report.");
 if(!a->patch_logged)diag_item(&w,&first,"patch","wait","Bluetooth acceptance patch","The live patch has not run in this session.","Press Connect. The patch is validated before changing live kernel memory.");
 else if(!a->patch_rc)diag_item(&w,&first,"patch","pass","Bluetooth acceptance patch","Patch routine returned success.","No action needed.");
 else diag_item(&w,&first,"patch","fail","Bluetooth acceptance patch","The patch routine returned an error.","Stop reconnect attempts and inspect patch_rc, status, before and after values in the report.");
 if(!a->target.address)diag_item(&w,&first,"device","wait","Headphone selection","No saved headphone is selected.","Scan nearby, select Haylou W1, then connect.");
 else if(a->streaming||(a->btstate>=5&&a->btstate<=6))diag_item(&w,&first,"device","pass","Bluetooth headphone link","The selected device is connected or streaming.","No action needed.");
 else diag_item(&w,&first,"device",a->wanted?"warn":"wait","Bluetooth headphone link",a->wanted?"Connection is requested but the audio link is not ready.":"A headphone is selected but not connected.","Put the Haylou W1 in its case, disconnect it from the phone, then reconnect.");
 if(a->streaming&&a->ring){
  u32 wp=(u32)(*(volatile u64*)(a->ring+8)>>32),rp=a->readpos;
  if(wp<16||wp>=0x10000||(wp&7)||rp<16||rp>=0x10000||(rp&7))diag_item(&w,&first,"capture","fail","PS4 game-audio capture","The shared audio ring has an invalid read/write position.","Restart the audio path. If this repeats when a game opens, keep the log and game name for diagnosis.");
  else{unsigned avail=wp>=rp?wp-rp:0xfff0-rp+wp;diag_item(&w,&first,"capture",avail>=1536?"pass":"wait","PS4 game-audio capture",avail>=1536?"The shared audio ring contains game-audio samples.":"No complete audio packet is ready in the capture ring.","Enter the game and start playback. HDMI capture is kept active to protect the game mix.");}
 }else diag_item(&w,&first,"capture","wait","PS4 game-audio capture","Capture is checked when headphones are streaming.","Connect the headphones and enter a game; use Test sound to separate Bluetooth from game capture.");
 if(!a->streaming)diag_item(&w,&first,"queue","wait","Bluetooth audio queue","No live audio packets are being sent right now.","Connect headphones and run the 10-second tone test.");
 else if(a->cur.failed)diag_item(&w,&first,"queue","fail","Bluetooth audio queue","Driver send errors were seen in the current sample window.","Reconnect Bluetooth. If errors persist, test Skip HCI widening and keep the full report.");
 else if(a->cur.refused||a->cur.late16||a->cur.late50)diag_item(&w,&first,"queue","warn","Bluetooth audio queue","Some packets were refused or sent late in the latest sample window.","Try 250 us polling, then 500 us. If refusal continues, set recovery window to 6 seconds and reconnect.");
 else diag_item(&w,&first,"queue","pass","Bluetooth audio queue","No send refusal or late packet has been recorded in the current sample window.","Repeat the check while in gameplay for at least one minute.");
 if(a->prio_rc||a->aff_rc||a->pi_rc<0)diag_item(&w,&first,"schedule","warn","Sender scheduling","One or more real-time scheduling attributes were not accepted.","Use 500 us first. Do not force CPU affinity or priority on unsupported firmware.");
 else diag_item(&w,&first,"schedule","pass","Sender scheduling","Thread scheduling setup returned no recorded errors.","No action needed.");
 diag_item(&w,&first,"settings","pass","Saved settings","Recovery and sender settings are stored on the PS4.","Settings survive restarting the payload.");
 if(exists(WORKER_LOG))diag_item(&w,&first,"log","pass","Diagnostic log","The worker log is readable from this dashboard.","Use Copy full report to share a snapshot.");
 else diag_item(&w,&first,"log","warn","Diagnostic log","No worker log file is present yet.","Start a connection attempt, then refresh the report.");
 append(&w,"]}");return w.used;
}
static int route_number(char*request,const char*prefix,unsigned*value){
 if(!starts(request,prefix)){return 0;}const char*p=request+length(prefix);unsigned v=0,n=0;
 while(p[n]>='0'&&p[n]<='9'&&n<6){v=v*10+(unsigned)(p[n]-'0');n++;}
 if(!n||!starts(p+n," HTTP/1.")){return -1;}*value=v;return 1;
}
static void manual_reconnect(struct app*a,int full){
 if(!a->target.address)return;
 end_scan(a);stop_audio(a);
 if(full){drop_link(a);closefd(a->bt);a->bt=-1;}
 a->wanted=1;a->phase=0;a->next_try=a->clock()+(full?1000000:0);
 a->failed=a->dropped=a->announced=0;a->attempts=0;a->relinks=0;a->refused_since=0;a->recovery_warned=0;
 message(a,full?"Full Bluetooth reconnect requested.":"Audio stream restart requested.");
 if(wlog_begin(a,a->clock(),full?"cmd_full_reconnect":"cmd_audio_restart")){wlog_raw(a,"target=");wlog_mac(a,a->target.address);wlog_end(a,1);}
}
static void dispatch(struct app*a,struct client*c){
 char body[12000];
 if(!host_valid(c->input)){response(c,"403 Forbidden","text/plain","Use the PS4 IP address.",23);return;}
 if(starts(c->input,"GET / HTTP/1.")){response(c,"200 OK","text/html; charset=utf-8",web_page,sizeof(web_page)-1);return;}
 if(starts(c->input,"GET /api/native HTTP/1.")){unsigned n=native_status(a,body);response(c,"200 OK","application/octet-stream",body,n);return;}
 if(starts(c->input,"GET /api/status HTTP/1.")){unsigned n=status(a,body,sizeof(body));response(c,"200 OK","application/json",body,n);return;}
 if(starts(c->input,"GET /api/timing HTTP/1.")){unsigned n=timing_json(a,body,sizeof(body));response(c,"200 OK","application/json",body,n);return;}
 if(starts(c->input,"GET /api/log HTTP/1.")){unsigned n=read_log_tail(body,sizeof(body));response(c,"200 OK","text/plain; charset=utf-8",body,n);return;}
  if(starts(c->input,"GET /api/diagnostics HTTP/1.")){unsigned n=diagnostics_json(a,body,sizeof(body));response(c,"200 OK","application/json",body,n);return;}
 char*control=header(c->input,"x-bta-control");
 if(!starts(c->input,"POST ")||!control||!starts(control,"1\r\n")){response(c,"403 Forbidden","text/plain","Control header required.",24);return;}
 if(starts(c->input,"POST /api/scan HTTP/1.")){if(a->streaming){response(c,"409 Conflict","text/plain","Stop audio before scanning.",27);return;}if(scan(a)){response(c,"503 Service Unavailable","text/plain","Scan could not start",20);return;}}
 else if(starts(c->input,"POST /api/select/")){
  u64 mac=parse_mac(c->input+17);refresh_devices(a);struct bt_device*d=device(a,mac);
  if(!mac||!d||!bt_is_audio(d)){response(c,"400 Bad Request","text/plain","Select a listed audio device.",29);return;}
  struct bt_device chosen=*d;u64 old=a->target.address;end_scan(a);stop_audio(a);
  if(save_target(a,&chosen)){response(c,"500 Internal Server Error","text/plain","Cannot save selection.",22);return;}
  load_joint(a);
  if(old!=mac&&a->bt>=0)sc3(54,a->bt,0x8008620b,(long)&old);
  a->wanted=1;a->next_try=0;a->dropped=a->failed=a->announced=a->session=0;a->attempts=0;a->relinks=0;a->refused_since=0;sc3(10,(long)TV_ON,0,0);a->tv=0;say(a,"Connecting to ","...");
  if(wlog_begin(a,a->clock(),"cmd_select")){wlog_raw(a,"mac=");wlog_mac(a,mac);wlog_raw(a," name=");wlog_str(a,chosen.name);wlog_raw(a," joint=");wlog_dec(a,a->joint);wlog_end(a,1);}
 }
 else if(starts(c->input,"POST /api/pin/")){
  char pin[17];unsigned n=0;const char*p=c->input+14;while(p[n]>='0'&&p[n]<='9'&&n<16){pin[n]=p[n];n++;}
  if(!a->pin_pending||!n||!starts(p+n," HTTP/1.")){response(c,"400 Bad Request","text/plain","Invalid pairing PIN.",20);return;}
  pin[n]=0;struct{u64 address;char*pin;unsigned length,pad;}q={a->target.address,pin,n,0};long rc=sc3(54,a->bt,0x80186207,(long)&q);
  if(wlog_begin(a,a->clock(),"cmd_pin")){wlog_raw(a,"rc=");wlog_dec(a,rc);wlog_end(a,1);}
  if(rc){response(c,"500 Internal Server Error","text/plain","PIN submission failed.",22);return;}a->pin_pending=0;message(a,"Pairing PIN submitted.");
 }
 else if(starts(c->input,"POST /api/volume/")){
  const char*p=c->input+17;unsigned value=0,n=0;
  while(p[n]>='0'&&p[n]<='9'&&n<3){value=value*10+(unsigned)(p[n]-'0');n++;}
  if(!n||value>100||!starts(p+n," HTTP/1.")){response(c,"400 Bad Request","text/plain","Volume must be 0-100.",21);return;}
  if(save_volume(a,(int)value)){response(c,"500 Internal Server Error","text/plain","Cannot save volume.",19);return;}
  if(wlog_begin(a,a->clock(),"cmd_volume")){wlog_raw(a,"vol=");wlog_dec(a,a->volume);wlog_end(a,0);}
 }
 else if(starts(c->input,"POST /api/test HTTP/1.")){
  if(!a->streaming||a->bt<0||a->btstate<5||a->btstate>6){response(c,"409 Conflict","text/plain","Connect headphones first.",25);return;}
  a->test_phase=0;a->test_next=0;a->test_until=a->clock()+10000000;message(a,"Playing test tone for 10 seconds.");
  if(wlog_begin(a,a->clock(),"cmd_test")){wlog_raw(a,"10s");wlog_end(a,0);}
 }
 else if(starts(c->input,"POST /api/connect HTTP/1.")){a->wanted=1;a->relinks=0;sc3(10,(long)TV_ON,0,0);a->tv=0;if(!a->streaming){end_scan(a);a->phase=0;a->next_try=0;a->dropped=a->failed=a->announced=a->session=0;a->attempts=0;say(a,"Connecting to ","...");}if(wlog_begin(a,a->clock(),"cmd_connect")){wlog_raw(a,"streaming=");wlog_dec(a,a->streaming);wlog_end(a,1);}}
 else if(starts(c->input,"POST /api/stop HTTP/1.")){if(wlog_begin(a,a->clock(),"cmd_stop")){wlog_raw(a,"ok");wlog_end(a,1);}a->wanted=0;end_scan(a);stop_audio(a);drop_link(a);a->relinks=0;a->refused_since=0;closefd(a->bt);a->bt=-1;a->dropped=a->failed=a->announced=a->session=0;a->attempts=0;message(a,"Earbud audio stopped. TV sound restored.");}
 else if(starts(c->input,"POST /api/joint-on HTTP/1.")){
  if(!a->target.address){response(c,"409 Conflict","text/plain","Select headphones first.",24);return;}
  char p[40];joint_path(a->target.address,p);if(exists(p)&&sc3(10,(long)p,0,0)<0){response(c,"500 Internal Server Error","text/plain","Cannot save preference.",23);return;}
  a->joint=1;message(a,"Joint stereo is on for these headphones.");
  if(wlog_begin(a,a->clock(),"cmd_joint")){wlog_raw(a,"joint=1");wlog_end(a,1);}
 }
 else if(starts(c->input,"POST /api/joint-off HTTP/1.")){
  if(!a->target.address){response(c,"409 Conflict","text/plain","Select headphones first.",24);return;}
  char p[40];joint_path(a->target.address,p);
  if(put(p,"on\n",3)){response(c,"500 Internal Server Error","text/plain","Cannot save preference.",23);return;}
  a->joint=0;message(a,"Joint stereo is off for these headphones.");
  if(wlog_begin(a,a->clock(),"cmd_joint")){wlog_raw(a,"joint=0");wlog_end(a,1);}
 }
  else if(starts(c->input,"POST /api/settings/auto/")){
   unsigned v;if(route_number(c->input,"POST /api/settings/auto/",&v)!=1||v>1){response(c,"400 Bad Request","text/plain","Auto recovery must be 0 or 1.",29);return;}
   a->auto_recover=(int)v;if(save_settings(a)){response(c,"500 Internal Server Error","text/plain","Cannot save settings.",21);return;}a->recovery_warned=0;
   if(wlog_begin(a,a->clock(),"cmd_setting")){wlog_raw(a,"auto_recover=");wlog_dec(a,v);wlog_end(a,0);}
  }
  else if(starts(c->input,"POST /api/settings/recover-ms/")){
   unsigned v;if(route_number(c->input,"POST /api/settings/recover-ms/",&v)!=1||(v!=2000&&v!=4000&&v!=6000&&v!=8000)){response(c,"400 Bad Request","text/plain","Invalid recovery window.",24);return;}
   a->recover_ms=v;if(save_settings(a)){response(c,"500 Internal Server Error","text/plain","Cannot save settings.",21);return;}
   if(wlog_begin(a,a->clock(),"cmd_setting")){wlog_raw(a,"recover_ms=");wlog_dec(a,v);wlog_end(a,0);}
  }
  else if(starts(c->input,"POST /api/settings/recover-limit/")){
   unsigned v;if(route_number(c->input,"POST /api/settings/recover-limit/",&v)!=1||(v!=1&&v!=3&&v!=5)){response(c,"400 Bad Request","text/plain","Invalid recovery limit.",23);return;}
   a->recover_limit=v;if(save_settings(a)){response(c,"500 Internal Server Error","text/plain","Cannot save settings.",21);return;}
   if(wlog_begin(a,a->clock(),"cmd_setting")){wlog_raw(a,"recover_limit=");wlog_dec(a,v);wlog_end(a,0);}
  }
  else if(starts(c->input,"POST /api/settings/sender-us/")){
   unsigned v;if(route_number(c->input,"POST /api/settings/sender-us/",&v)!=1||(v!=250&&v!=500&&v!=1000&&v!=2000)){response(c,"400 Bad Request","text/plain","Invalid sender interval.",24);return;}
   a->sender_sleep_us=v;if(save_settings(a)){response(c,"500 Internal Server Error","text/plain","Cannot save settings.",21);return;}
   if(wlog_begin(a,a->clock(),"cmd_setting")){wlog_raw(a,"sender_sleep_us=");wlog_dec(a,v);wlog_end(a,0);}
  }
  else if(starts(c->input,"POST /api/settings/no-widen/")){
   unsigned v;if(route_number(c->input,"POST /api/settings/no-widen/",&v)!=1||v>1){response(c,"400 Bad Request","text/plain","Invalid HCI setting.",20);return;}
   if(v){if(put(NO_WIDEN,"1",1)){response(c,"500 Internal Server Error","text/plain","Cannot save setting.",20);return;}}
   else if(exists(NO_WIDEN)&&sc3(10,(long)NO_WIDEN,0,0)<0){response(c,"500 Internal Server Error","text/plain","Cannot save setting.",20);return;}
   a->no_widen=(int)v;if(wlog_begin(a,a->clock(),"cmd_setting")){wlog_raw(a,"no_widen=");wlog_dec(a,v);wlog_raw(a," apply=next_connect");wlog_end(a,0);}
  }
  else if(starts(c->input,"POST /api/restart-audio HTTP/1.")){if(!a->target.address){response(c,"409 Conflict","text/plain","Select headphones first.",24);return;}manual_reconnect(a,0);}
  else if(starts(c->input,"POST /api/reconnect HTTP/1.")){if(!a->target.address){response(c,"409 Conflict","text/plain","Select headphones first.",24);return;}manual_reconnect(a,1);}
  else if(starts(c->input,"POST /api/log/clear HTTP/1.")){
   long fd=sc3(5,(long)WORKER_LOG,0x601,0644);if(fd<0){response(c,"500 Internal Server Error","text/plain","Cannot clear log.",17);return;}closefd(fd);
   if(wlog_begin(a,a->clock(),"cmd_log_clear")){wlog_raw(a,"log cleared from dashboard");wlog_end(a,1);}
  }
 else if(starts(c->input,"POST /api/tv-on HTTP/1.")){if(put(TV_ON,"on\n",3)){response(c,"500 Internal Server Error","text/plain","Cannot save preference.",23);return;}a->tv=1;mute(a,0);if(wlog_begin(a,a->clock(),"cmd_tv")){wlog_raw(a,"tv=1");wlog_end(a,0);}}
 else if(starts(c->input,"POST /api/tv-off HTTP/1.")){if(exists(TV_ON)&&sc3(10,(long)TV_ON,0,0)<0){response(c,"500 Internal Server Error","text/plain","Cannot save preference.",23);return;}a->tv=0;/* Keep HDMI capture active; mute the monitor physically. */if(wlog_begin(a,a->clock(),"cmd_tv")){wlog_raw(a,"tv=0");wlog_end(a,0);}}
 else if(starts(c->input,"POST /api/quit HTTP/1.")){if(wlog_begin(a,a->clock(),"cmd_quit")){wlog_raw(a,"ok");wlog_end(a,1);}a->wanted=0;stop_audio(a);a->quit=1;a->dropped=a->failed=a->announced=a->session=0;a->attempts=0;message(a,"Bluetooth Audio stopped. TV sound restored.");}
 else{response(c,"404 Not Found","text/plain","Not found",9);return;}
 unsigned n=status(a,body,sizeof(body));response(c,"200 OK","application/json",body,n);
}
static void network_tick(struct app*a,u64 now){
 for(int i=0;i<4;i++){
  struct client*c=&a->clients[i];
  if(c->fd<0){struct sockaddr4 peer;u32 len=sizeof(peer);long fd=sc3(30,a->listener,(long)&peer,(long)&len);if(fd<0)continue;
   if(len!=16||!local_ip((u8*)&peer.addr)||nonblock(fd)<0){closefd(fd);continue;}int one=1;sc5(105,fd,0xffff,0x800,(long)&one,4);c->fd=fd;c->used=c->sent=c->total=0;c->input[0]=0;c->deadline=now+3000000;
  }
  if(now>c->deadline){closefd(c->fd);c->fd=-1;continue;}
  if(!c->total){long n=sc3(3,c->fd,(long)(c->input+c->used),sizeof(c->input)-c->used-1);
   if(n==0||(n<0&&n!=-35)){closefd(c->fd);c->fd=-1;continue;}
   if(n>0){c->used+=(unsigned)n;c->input[c->used]=0;if(find(c->input,"\r\n\r\n")){lock_state(a);dispatch(a,c);unlock_state(a);}else if(c->used==sizeof(c->input)-1)response(c,"431 Request Header Fields Too Large","text/plain","Request too large",17);}
  }
  if(c->total){long n=sc3(4,c->fd,(long)(c->output+c->sent),c->total-c->sent);if(n>0)c->sent+=(unsigned)n;if(c->sent==c->total||(n<0&&n!=-35)){closefd(c->fd);c->fd=-1;}}
 }
}
/* Feed the driver the way the PS4's own audio daemon does. orbis_audiod creates its
 * Bluetooth senders as dedicated threads with explicit scheduling: SCHED_FIFO, priority
 * 0x132 (306) and CPU mask 0xc0 (cores 6-7), away from game threads. Our sender does
 * nothing but capture ring -> driver; the web page, Bluetooth management and status file
 * stay on the worker thread. One mutex guards the shared state. If the console refuses
 * the scheduling attributes, the sender runs with defaults; if no thread can be created,
 * the worker loop sends audio itself, as before. */
static void init_lock(struct app*a){
 /* Priority inheritance: while the sender waits for the lock, its holder runs at the
  * sender's priority, so a paused housekeeping thread cannot stall the audio. */
 int(*init)(void**,void**,const char*)=0,(*ainit)(void**)=0,(*proto)(void**,int)=0,(*adestroy)(void**)=0;
 void*attr=0;void**ap=0;a->pi_rc=-1;
 if(!resolve("scePthreadMutexattrInit",&ainit)&&ainit&&!ainit(&attr)){ap=&attr;if(!resolve("scePthreadMutexattrSetprotocol",&proto)&&proto)a->pi_rc=proto(ap,1);}
 if(resolve("scePthreadMutexInit",&init)||resolve("scePthreadMutexLock",&a->mlock)||resolve("scePthreadMutexUnlock",&a->munlock)||!init||!a->mlock||!a->munlock)a->mlock=a->munlock=0;
 else if(init(&a->mutex,ap,"Bluetooth Audio state")){a->pi_rc=-2;if(init(&a->mutex,0,"Bluetooth Audio state"))a->mlock=a->munlock=0;}
 if(ap&&!resolve("scePthreadMutexattrDestroy",&adestroy)&&adestroy)adestroy(ap);
}
static void*sender(void*arg){
 struct app*a=arg;
 void*(*self)(void)=0;int(*getprio)(void*,int*)=0;
 if(!resolve("scePthreadSelf",&self)&&!resolve("scePthreadGetprio",&getprio)&&self&&getprio){int p=0;if(!getprio(self(),&p))a->sender_prio=p;}
 while(a->sender_run){u64 now=a->clock();lock_state(a);audio_tick(a,now);unlock_state(a);a->sleep(a->sender_sleep_us?a->sender_sleep_us:500);}
 a->sender_alive=0;return 0;
}
static void start_sender(struct app*a){
 if(!a->mlock)return;/* no lock, no second thread */
 int(*create)(void**,void**,void*(*)(void*),void*,const char*)=0;int(*detach)(void*)=0;
 if(resolve("scePthreadCreate",&create)||resolve("scePthreadDetach",&detach)||!create||!detach){a->thread_rc=-1;return;}
 int(*init)(void**)=0,(*inherit)(void**,int)=0,(*policy)(void**,int)=0,(*param)(void**,const int*)=0,(*affinity)(void**,u64)=0,(*destroy)(void**)=0;
 void*attr=0;void**ap=0;
 if(!resolve("scePthreadAttrInit",&init)&&init&&!init(&attr)){
  ap=&attr;int prio=0x132;
  if(!resolve("scePthreadAttrSetinheritsched",&inherit)&&inherit)inherit(ap,0);
  if(!resolve("scePthreadAttrSetschedpolicy",&policy)&&policy)policy(ap,1);
  a->prio_rc=(!resolve("scePthreadAttrSetschedparam",&param)&&param)?param(ap,&prio):-1;
  a->aff_rc=(!resolve("scePthreadAttrSetaffinity",&affinity)&&affinity)?affinity(ap,0xc0):-1;
 }
 void*thread=0;a->sender_run=1;a->sender_alive=1;
 a->thread_rc=create(&thread,ap,sender,a,"Bluetooth Audio sender");
 if(a->thread_rc&&ap){a->prio_rc=a->aff_rc=-2;a->thread_rc=create(&thread,0,sender,a,"Bluetooth Audio sender");}
 if(a->thread_rc){a->sender_run=0;a->sender_alive=0;}else detach(thread);
 if(ap&&!resolve("scePthreadAttrDestroy",&destroy)&&destroy)destroy(ap);
}
/* The sender uses this stack frame's state: wait for it to leave before returning. */
static void stop_sender(struct app*a){a->sender_run=0;while(a->sender_alive)a->sleep(1000);}
void*bta_worker(void*arg){
 struct app a;memset(&a,0,sizeof(a));a.listener=(long)arg;a.bt=a.hdmi=a.shmfd=-1;a.muted=-1;a.wanted=0;for(int i=0;i<4;i++)a.clients[i].fd=-1;
 if(resolve("sceKernelUsleep",&a.sleep)||resolve("sceKernelGetProcessTime",&a.clock)||resolve("shm_open",&a.shm)||resolve("mmap",&a.map)||resolve("munmap",&a.unmap)||!a.sleep||!a.clock||!a.shm||!a.map||!a.unmap){closefd(a.listener);return 0;}
 sc3(136,(long)"/data/bta_audio",0777,0);sc3(15,(long)"/data/bta_audio",0777,0);sc3(10,(long)BTA_STOP,0,0);
 load_target(&a);load_joint(&a);load_volume(&a);load_settings(&a);a.tv=exists(TV_ON);a.no_widen=exists(NO_WIDEN);a.hdmi=sc3(5,(long)"/dev/hdmi",2,0);
 message(&a,"Ready. Select your headphones and press Connect.");notify("Bluetooth Audio is ready and idle. Open the home-screen app, then press Connect. Web controls: http://127.0.0.1:8097/ (or your PS4 IP on port 8097).");
 init_lock(&a);start_sender(&a);
 {u64 t0=a.clock();
  struct{u64 size;char text[28];unsigned version;}sw={.size=40};int(*getver)(void*)=0;unsigned swv=0;
  if(!resolve("sceKernelGetSystemSwVersion",&getver)&&getver&&!getver(&sw)){sw.text[27]=0;swv=sw.version;}
  if(wlog_begin(&a,t0,"worker_start")){
   wlog_raw(&a,"v=" BTA_VERSION " fw=");wlog_str(&a,swv?sw.text:"unknown");wlog_raw(&a," fw_hex=");wlog_hex(&a,swv);
   wlog_raw(&a," hdmi_fd=");wlog_dec(&a,a.hdmi);wlog_raw(&a," tv=");wlog_dec(&a,a.tv);
   wlog_raw(&a," vol=");wlog_dec(&a,a.volume);wlog_raw(&a," joint=");wlog_dec(&a,a.joint);
   wlog_raw(&a," no_widen=");wlog_dec(&a,a.no_widen);
   if(a.target.address){wlog_raw(&a," saved=");wlog_mac(&a,a.target.address);wlog_raw(&a," name=");wlog_str(&a,a.target.name);}
   wlog_end(&a,1);
  }
  int sched_bad=(!a.sender_alive||a.thread_rc||a.prio_rc||a.aff_rc||a.pi_rc<0);
  if(wlog_begin(&a,t0,sched_bad?"WARN sender_sched":"sender_sched")){
   wlog_raw(&a,"alive=");wlog_dec(&a,a.sender_alive);wlog_raw(&a," pi_rc=");wlog_dec(&a,a.pi_rc);
   wlog_raw(&a," thread_rc=");wlog_dec(&a,a.thread_rc);wlog_raw(&a," prio_rc=");wlog_dec(&a,a.prio_rc);
   wlog_raw(&a," aff_rc=");wlog_dec(&a,a.aff_rc);
   if(sched_bad)wlog_raw(&a," [DEGRADED_SCHEDULING]");
   wlog_end(&a,1);
  }
  wlog_flush(&a,t0,1);
 }
 while(!a.quit){u64 now=a.clock();
  if(now>=a.net_at){network_tick(&a,now);a.net_at=now+4000;}/* locks around each request */
  if(a.quit)break;
  char json[12000];unsigned n=0;int beat=now>=a.heartbeat;
  /* Take the lock only when there is shared-state work (~35 times a second). */
  if(now<a.poll_at&&a.sender_alive&&now<a.meter_at&&!beat){a.sleep(500);continue;}
  lock_state(&a);
  if(now>=a.poll_at){bluetooth_tick(&a,now);a.poll_at=now+50000;}
  if(!a.sender_alive)audio_tick(&a,now);/* fallback: no sender thread */
  /* Output meter: peak of the last ~80 ms, with a quick decay so a client that
   * polls a few times a second still sees movement instead of a random window. */
  if(now>=a.meter_at){unsigned p=a.meter_peak*100/32767;if(p>100)p=100;unsigned hold=(unsigned)a.meter*7/10;a.meter=(int)(p>hold?p:hold);a.meter_peak=0;a.meter_at=now+80000;}
  if(beat){n=status(&a,json,sizeof(json));a.level=0;a.heartbeat=now+1000000;}
  unlock_state(&a);
  if(beat){
   /* File I/O stays outside the lock: a slow write must never hold up the sender. */
   put("/data/bta_audio/status.json",json,n);
   if(exists(BTA_STOP)){lock_state(&a);if(wlog_begin(&a,now,"stop_sentinel")){wlog_raw(&a,"detected");wlog_end(&a,1);}unlock_state(&a);break;}
   int tv=exists(TV_ON),nw=exists(NO_WIDEN);
   lock_state(&a);
   a.tv=tv;
   if(nw!=a.no_widen){a.no_widen=nw;if(wlog_begin(&a,now,"no_widen_changed")){wlog_raw(&a,"no_widen=");wlog_dec(&a,nw);wlog_end(&a,1);}}
   unlock_state(&a);
   wlog_flush(&a,now,0);
   /* Disconnect is detected by the audio send failing (see audio_tick), as the
    * proven bridge did. No per-second driver state poll while streaming. */
  }
  a.sleep(500);
 }
 stop_sender(&a);
 end_scan(&a);stop_audio(&a);a.wanted=0;message(&a,"Bluetooth Audio is offline. TV sound restored.");char json[12000];unsigned n=status(&a,json,sizeof(json));put("/data/bta_audio/status.json",json,n);
 if(wlog_begin(&a,a.clock(),"worker_stop")){
  wlog_raw(&a,"quit=");wlog_dec(&a,a.quit);wlog_raw(&a," packets=");wlog_dec(&a,(long)a.packets);
  wlog_raw(&a," errors=");wlog_dec(&a,(long)a.errors);wlog_raw(&a," resyncs=");wlog_dec(&a,(long)a.resyncs);
  wlog_raw(&a," relinks=");wlog_dec(&a,(long)a.relinks_total);wlog_end(&a,1);
 }
 wlog_flush(&a,a.clock(),1);
 for(int i=0;i<4;i++)closefd(a.clients[i].fd);closefd(a.listener);closefd(a.bt);closefd(a.hdmi);
 return 0;
}
