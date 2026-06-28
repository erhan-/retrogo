/* Prime Go MIDI-to-keyboard bridge v3
 * 2-player layout with cboygo jog wheel handling
 *
 * D-Pad:  HC1=Left, HC2=Down, HC3=Right, Loop=Up  (WASD-style)
 * Buttons: PLAY=A, CUE=B, Pitch-=X, Pitch+=Y, HC4=L1, Roll=R1
 * Select/Start: FX Activate=Select, FX Assign=Start  
 * Jog: Left/Right (cboygo 14-bit decoder + 2-step threshold)
 * 
 * Player 1 = Left deck (ch2), Player 2 = Right deck (ch3)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/time.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

/* === MIDI mappings === */
#define CH_LEFT   2
#define CH_RIGHT  3
#define CH_GLOBAL 15
#define CH_FX     4
#define CH_MIXER  0

/* Deck notes (left: ch2, right: ch3) */
#define N_PLAY     10
#define N_CUE       9
#define N_SYNC      8
#define N_SYNC      8
#define N_HC1      15
#define N_HC2      16
#define N_HC3      17
#define N_HC4      18
#define N_LOOP     12  /* Pad Mode: Loops / Loop */
#define N_ROLL     13  /* Pad Mode: Roll */
#define N_PITCHM   29  /* Pitch Bend - */
#define N_PITCHP   30  /* Pitch Bend + */

/* Jog wheel: 14-bit CC on MSB 55, LSB 77 */
#define CC_JOG_MSB 55
#define CC_JOG_LSB 77

/* Global */
#define N_BACK      3
#define N_FWD       4
#define N_VIEW      7

/* FX section (ch4) */
#define N_FX_ACTIVATE 6
#define N_FX_ASSIGN  11

/* === Keyboard mapping matching RetroArch defaults === */
/* Player 1 */
#define P1_A  KEY_X
#define P1_B  KEY_Z
#define P1_X  KEY_S
#define P1_Y  KEY_A
#define P1_L  KEY_Q     /* HC4 */
#define P1_R  KEY_W     /* Roll */
#define P1_L2 KEY_E     /* Sync */
#define P1_R2 KEY_R     /* Vinyl */
#define P1_SELECT  KEY_RIGHTSHIFT
#define P1_START   KEY_ENTER

/* Player 2 - letters not used by RetroArch defaults */
#define P2_A  KEY_C
#define P2_B  KEY_D
#define P2_X  KEY_V
#define P2_Y  KEY_H
#define P2_L  KEY_T
#define P2_R  KEY_B
#define P2_L2 KEY_U
#define P2_R2 KEY_N
#define P2_SELECT  KEY_O
#define P2_START   KEY_M

/* P1 D-Pad */
#define P1_UP    KEY_UP
#define P1_DOWN  KEY_DOWN
#define P1_LEFT  KEY_LEFT
#define P1_RIGHT KEY_RIGHT

/* P2 D-Pad (right deck: IJKL layout) */
#define P2_UP    KEY_I
#define P2_DOWN  KEY_K
#define P2_LEFT  KEY_J
#define P2_RIGHT KEY_L

/* Menu / save / load */
#define K_MENU  KEY_BACKSPACE
#define K_SAVE  KEY_F5
#define K_LOAD  KEY_F7

/* === Internal state === */
typedef struct {
   int uifd, midi_fd, running;
   int running_status, parse_pos, parse_needed, parse_data[2];
   /* Jog state per channel */
   int jog_msb[2], jog_lsb[2], jog_prev[2];
} midi_state_t;

static void send_key(midi_state_t *s, int key, int val) {
   struct input_event ev;
   memset(&ev, 0, sizeof(ev));
   ev.type = EV_KEY; ev.code = key; ev.value = val;
   write(s->uifd, &ev, sizeof(ev));
}
static void send_sync(midi_state_t *s) {
   struct input_event ev;
   memset(&ev, 0, sizeof(ev));
   ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
   write(s->uifd, &ev, sizeof(ev));
}
static void send_tap(midi_state_t *s, int key) {
   send_key(s, key, 1);
   send_sync(s);
   usleep(16000);  /* ~1 frame hold */
   send_key(s, key, 0);
   send_sync(s);
}

static int init_uinput(void) {
   int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
   if (fd < 0) { perror("open uinput"); return -1; }
   ioctl(fd, UI_SET_EVBIT, EV_KEY);
   ioctl(fd, UI_SET_EVBIT, EV_SYN);

   int keys[] = {
      KEY_X,KEY_Z,KEY_S,KEY_A,KEY_Q,KEY_W,KEY_E,KEY_R,KEY_RIGHTSHIFT,KEY_ENTER,  /* P1 */
      KEY_C,KEY_D,KEY_V,KEY_H,KEY_T,KEY_B,KEY_U,KEY_N,KEY_O,KEY_M,  /* P2 */
      KEY_UP,KEY_DOWN,KEY_LEFT,KEY_RIGHT,  /* P1 D-Pad */
      KEY_I,KEY_K,KEY_J,KEY_L,  /* P2 D-Pad (IJKL) */
      KEY_BACKSPACE,KEY_F5,KEY_F7
   };
   for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++) ioctl(fd,UI_SET_KEYBIT,keys[i]);

   struct uinput_setup us;
   memset(&us,0,sizeof(us));
   strcpy(us.name,"Prime Go Controller");
   us.id.bustype=BUS_USB; us.id.vendor=0x1234; us.id.product=0x5679; us.id.version=1;
   if(ioctl(fd,UI_DEV_SETUP,&us)<0){perror("UI_DEV_SETUP");close(fd);return -1;}
   if(ioctl(fd,UI_DEV_CREATE)<0){perror("UI_DEV_CREATE");close(fd);return -1;}
   sleep(1);
   return fd;
}

static void note_on(midi_state_t *s, int ch, int note) {
   if(ch==CH_LEFT){
      if(note==N_LOOP)   send_key(s,P1_UP,1);
      if(note==N_PLAY)   send_key(s,P1_A,1);
      if(note==N_CUE)    send_key(s,P1_B,1);
      if(note==N_PITCHM) send_key(s,P1_X,1);
      if(note==N_PITCHP) send_key(s,P1_Y,1);
      if(note==N_HC4)    send_key(s,P1_L,1);
      if(note==N_ROLL)   send_key(s,P1_R,1);
      if(note==N_HC1)    send_key(s,P1_LEFT,1);
      if(note==N_HC2)    send_key(s,P1_DOWN,1);
      if(note==N_HC3)    send_key(s,P1_RIGHT,1);
   } else if(ch==CH_RIGHT){
      if(note==N_PLAY)   send_key(s,P2_A,1);
      if(note==N_CUE)    send_key(s,P2_B,1);
      if(note==N_PITCHM) send_key(s,P2_X,1);
      if(note==N_PITCHP) send_key(s,P2_Y,1);
      if(note==N_HC4)    send_key(s,P2_L,1);
      if(note==N_ROLL)   send_key(s,P2_R,1);
      if(note==N_LOOP)   send_key(s,P2_UP,1);
      if(note==N_HC1)    send_key(s,P2_LEFT,1);
      if(note==N_HC2)    send_key(s,P2_DOWN,1);
      if(note==N_HC3)    send_key(s,P2_RIGHT,1);
   } else if(ch==CH_MIXER){
      if(note==14) send_key(s,P1_SELECT,1);
      if(note==15) send_key(s,P1_START,1);
   } else if(ch==CH_FX){
      if(note==11) send_key(s,P1_L2,1);
      if(note==12) send_key(s,P1_R2,1);
   } else if(ch==CH_GLOBAL){
      if(note==N_VIEW) send_key(s,K_MENU,1);
      if(note==36)     send_key(s,P2_L2,1);
      if(note==37)     send_key(s,P2_R2,1);
   }
}
static void note_off(midi_state_t *s, int ch, int note) {
   if(ch==CH_LEFT){
      if(note==N_PLAY)   send_key(s,P1_A,0);
      if(note==N_CUE)    send_key(s,P1_B,0);
      if(note==N_PITCHM) send_key(s,P1_X,0);
      if(note==N_PITCHP) send_key(s,P1_Y,0);
      if(note==N_HC4)    send_key(s,P1_L,0);
      if(note==N_ROLL)   send_key(s,P1_R,0);
      if(note==N_LOOP)   send_key(s,P1_UP,0);
      if(note==N_HC1)    send_key(s,P1_LEFT,0);
      if(note==N_HC2)    send_key(s,P1_DOWN,0);
      if(note==N_HC3)    send_key(s,P1_RIGHT,0);
   } else if(ch==CH_RIGHT){
      if(note==N_PLAY)   send_key(s,P2_A,0);
      if(note==N_CUE)    send_key(s,P2_B,0);
      if(note==N_PITCHM) send_key(s,P2_X,0);
      if(note==N_PITCHP) send_key(s,P2_Y,0);
      if(note==N_HC4)    send_key(s,P2_L,0);
      if(note==N_ROLL)   send_key(s,P2_R,0);
      if(note==N_LOOP)   send_key(s,P2_UP,0);
      if(note==N_HC1)    send_key(s,P2_LEFT,0);
      if(note==N_HC2)    send_key(s,P2_DOWN,0);
      if(note==N_HC3)    send_key(s,P2_RIGHT,0);
   } else if(ch==CH_MIXER){
      if(note==14) send_key(s,P1_SELECT,0);
      if(note==15) send_key(s,P1_START,0);
   } else if(ch==CH_FX){
      if(note==11) send_key(s,P1_L2,0);
      if(note==12) send_key(s,P1_R2,0);
   } else if(ch==CH_GLOBAL){
      if(note==N_VIEW) send_key(s,K_MENU,0);
      if(note==36)     send_key(s,P2_L2,0);
      if(note==37)     send_key(s,P2_R2,0);
   }
}

/* cboygo-style jog: 14-bit CC, 2-step threshold, 16ms hold */
static void jog_tick(midi_state_t *s, int ch_idx) {
   if(s->jog_msb[ch_idx] < 0 || s->jog_lsb[ch_idx] < 0) return;
   int cur = (s->jog_msb[ch_idx] << 7) | s->jog_lsb[ch_idx];
   if(s->jog_prev[ch_idx] >= 0 && cur != s->jog_prev[ch_idx]) {
      int diff = cur - s->jog_prev[ch_idx];
      if(diff > 8192) diff -= 16384;
      else if(diff < -8192) diff += 16384;
      if(diff <= -2) send_tap(s, ch_idx ? P2_LEFT : P1_LEFT);
      else if(diff >= 2) send_tap(s, ch_idx ? P2_RIGHT : P1_RIGHT);
   }
   s->jog_prev[ch_idx] = cur;
   s->jog_msb[ch_idx] = s->jog_lsb[ch_idx] = -1;
}

static void handle_cc(midi_state_t *s, int ch, int cc, int val) {
   if(ch != CH_LEFT && ch != CH_RIGHT) return;
   int ci = (ch == CH_RIGHT);
   if(cc == CC_JOG_MSB) { s->jog_msb[ci] = val; jog_tick(s, ci); }
   else if(cc == CC_JOG_LSB) { s->jog_lsb[ci] = val; jog_tick(s, ci); }
}

static void parse_midi_byte(midi_state_t *s, unsigned char b) {
   if(b >= 0xF8) return;
   if(b >= 0x80){
      s->running_status = b; s->parse_pos = 0;
      s->parse_needed = ((b&0xF0)==0xC0 || (b&0xF0)==0xD0) ? 1 : 2;
      return;
   }
   if(!s->running_status) return;
   s->parse_data[s->parse_pos++]=b;
   if(s->parse_pos >= s->parse_needed){
      int st = s->running_status, ch = st&0x0F;
      switch(st&0xF0){
      case 0x90: if(s->parse_data[1]>0) note_on(s,ch,s->parse_data[0]);
                 else note_off(s,ch,s->parse_data[0]); break;
      case 0x80: note_off(s,ch,s->parse_data[0]); break;
      case 0xB0: handle_cc(s,ch,s->parse_data[0],s->parse_data[1]); break;
      }
      s->parse_pos=0;
   }
}

static void *midi_thread(void *arg){
   midi_state_t *s=(midi_state_t*)arg;
   struct pollfd pfd={.fd=s->midi_fd,.events=POLLIN};
   unsigned char buf[256];
   while(s->running){
      if(poll(&pfd,1,50)<=0) continue;
      int n=read(s->midi_fd,buf,sizeof(buf));
      if(n<0) break;
      for(int i=0;i<n;i++) parse_midi_byte(s,buf[i]);
      send_sync(s);
   }
   return NULL;
}

int main(void){
   midi_state_t s; pthread_t tid;
   memset(&s,0,sizeof(s));
   s.jog_msb[0]=s.jog_lsb[0]=s.jog_prev[0]=-1;
   s.jog_msb[1]=s.jog_lsb[1]=s.jog_prev[1]=-1;

   s.uifd = init_uinput();
   if(s.uifd<0) return 1;
   s.midi_fd = open("/dev/snd/midiC0D0",O_RDWR);
   if(s.midi_fd<0){perror("open midiC0D0");close(s.uifd);return 1;}
   s.running=1;
   printf("Prime Go Controller v3 started (2-player layout, cboygo jog)\n");
   pthread_create(&tid,NULL,midi_thread,&s);
   pthread_join(tid,NULL);
   close(s.midi_fd);
   ioctl(s.uifd,UI_DEV_DESTROY);
   close(s.uifd);
   return 0;
}
