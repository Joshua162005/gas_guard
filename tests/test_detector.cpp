// Run on a PC:  g++ -std=c++11 -O1 -o /tmp/t tests/test_detector.cpp && /tmp/t
#include "../esp32_gas_sensor/gas_detector.h"
#include <cstdio>
#include <cstdlib>
#include <random>
static std::mt19937 rng(42);
static float noise(float s){ std::normal_distribution<float> d(0,s); return d(rng); }
static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){printf("  FAIL: %s\n",msg);fails++;} else printf("  ok:   %s\n",msg);}while(0)

struct Sim { GasDetector d; uint32_t t=0; int alerts=0; bool prev=false; int firstAlertAt=-1;
  Sim():d(DetectorConfig()){}
  void step(float v){ d.update(v,t*1000u); if(d.alert()&&!prev){alerts++; if(firstAlertAt<0)firstAlertAt=t;} prev=d.alert(); t++; }
};

int main(){
  printf("T1 clean air, noise +-15, 8 h, slow upward drift 100 counts/h (temperature/humidity)\n");
  { Sim s; for(int i=0;i<8*3600;i++){ float base=1200+100.0f*i/3600.0f; s.step(base+noise(15)); }
    CHECK(s.alerts==0,"no false alarm in 8 h");
    CHECK(s.d.state()==NORMAL,"state NORMAL at end"); printf("  baseline learned: %.0f (true %.0f)\n",s.d.baseline(),1200+800.0f); }

  printf("T2 fast but realistic drift 300 counts/h for 3 h\n");
  { Sim s; for(int i=0;i<3*3600;i++){ float base=1200+300.0f*i/3600.0f; s.step(base+noise(15)); }
    printf("  alerts=%d baseline=%.0f true=%.0f\n",s.alerts,s.d.baseline(),1200+900.0f);
    CHECK(s.alerts==0,"no false alarm at 300/h drift"); }

  printf("T3 leak: +700 step after 10 min, lasts 5 min, then clears\n");
  { Sim s; for(int i=0;i<600;i++) s.step(1200+noise(15));
    int t0=s.t; for(int i=0;i<300;i++) s.step(1900+noise(15));
    printf("  alert started %d s after gas arrived\n", s.firstAlertAt-t0);
    CHECK(s.firstAlertAt>=0 && s.firstAlertAt-t0<=4,"alarm within 4 s");
    CHECK(s.d.alert(),"still alarming while gas present");
    for(int i=0;i<60;i++) s.step(1200+noise(15));
    CHECK(!s.d.alert(),"alarm cleared after gas gone");
    CHECK(s.alerts==1,"exactly one alarm (no flapping)"); }

  printf("T4 gas level hovering at the limit with noise must not flap\n");
  { Sim s; for(int i=0;i<600;i++) s.step(1200+noise(10)); int a0=s.alerts;
    for(int i=0;i<600;i++) s.step(1200+300+noise(40));
    printf("  alarm starts during hover: %d\n", s.alerts-a0);
    CHECK(s.alerts-a0<=2,"at most 2 alarm starts while hovering at the limit"); }

  printf("T5 leak ramping +600 over 10 min\n");
  { Sim s; for(int i=0;i<600;i++) s.step(1200+noise(15));
    for(int i=0;i<600;i++) s.step(1200+600.0f*i/600+noise(15));
    for(int i=0;i<120;i++) s.step(1800+100+noise(15));
    CHECK(s.alerts>=1,"10-minute ramp detected"); }

  printf("T6 very slow leak, +300 over 3 hours (documented limitation: drift tracking may absorb it)\n");
  { Sim s; for(int i=0;i<600;i++) s.step(1200+noise(15));
    for(int i=0;i<3*3600;i++) s.step(1200+300.0f*i/(3*3600)+noise(15));
    printf("  alerts=%d (informational)\n",s.alerts); }

  printf("T7 sensor unplugged / powered off\n");
  { Sim s; for(int i=0;i<600;i++) s.step(1200+noise(15));
    for(int i=0;i<20;i++) s.step(0);
    CHECK(s.d.state()==SENSOR_ERROR,"SENSOR_ERROR reported");
    CHECK(!s.d.alert(),"no alarm from a dead sensor");
    for(int i=0;i<30;i++) s.step(1200+noise(15));
    CHECK(s.d.state()==WARMING_UP,"treated as cold start when it returns");
    for(int i=0;i<400;i++) s.step(1200+noise(15));
    CHECK(s.d.state()==NORMAL,"back to NORMAL after warm-up+calibration"); }

  printf("T8 one bad sample must not cause a fault\n");
  { Sim s; for(int i=0;i<600;i++) s.step(1200+noise(15)); s.step(0); s.step(1200);
    CHECK(s.d.state()==NORMAL,"single glitch ignored"); }

  printf("T9 gas already present when powered on (backstop)\n");
  { Sim s; for(int i=0;i<125;i++) s.step(3000+noise(20));   // through warm-up, hovering high
    for(int i=0;i<60;i++) s.step(3800+noise(20));
    CHECK(s.d.alert(),"hard limit alarms even with no baseline"); }

  printf("T10 cold-start transient (high, decaying during warm-up) must not alarm\n");
  { Sim s; for(int i=0;i<119;i++) s.step(3900-2500.0f*i/119+noise(20));
    for(int i=0;i<200;i++) s.step(1400+noise(15));
    CHECK(s.alerts==0,"no alarm during warm-up transient");
    CHECK(s.d.state()==NORMAL,"NORMAL afterwards"); printf("  baseline=%.0f\n",s.d.baseline()); }

  printf("T11 your sensor as-is: clean air ~3100 (pot too high)\n");
  { Sim s; for(int i=0;i<700;i++) s.step(3100+noise(15));
    CHECK(s.alerts==0,"no false alarm"); printf("  warn='%s' alarm level=%.0f\n",s.d.warn(),s.d.alarmLevel());
    CHECK(std::string(s.d.warn())=="baseline_high","warns that baseline is too high"); }

  printf("T12 sensor still settling: creeps up 300 counts over 10 min after warm-up, then stays\n");
  { Sim s; for(int i=0;i<200;i++) s.step(1200+noise(15));
    for(int i=0;i<600;i++) s.step(1200+300.0f*i/600+noise(15));
    for(int i=0;i<1800;i++) s.step(1500+noise(15));
    CHECK(s.alerts==0,"no false alarm while the sensor settles"); }

  printf("T13 unstable clean air during learning must not set a low baseline\n");
  { Sim s; for(int i=0;i<181;i++) s.step(1200+noise(15));
    for(int i=0;i<40;i++) s.step(1200+200.0f*(i%2)+noise(15));   // swinging +-200
    CHECK(s.d.state()==CALIBRATING,"still calibrating while readings swing"); }

  printf("\n%s (%d failures)\n", fails?"FAILED":"ALL PASSED", fails); return fails?1:0;
}
