#include <cstdio>
#include "ui/ColorWheel.h"

using namespace strata::ui::colorwheel;
int main() {
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const char* message) { ++checks; if (!ok) { ++failed; std::printf("FAIL: %s\n", message); } };
    auto near = [&](double a, double b, const char* message) { check(std::fabs(a-b)<1e-9,message); };
    State black; black.sync({0,0,0});
    black.begin({30,0},30,Drag::Hue,true); black.update({0,-30},30,false);
    for(double value:black.rgb)near(value,0,"hue at black must remain black");
    near(black.hsv.h,.25,"relative ring rotates hue"); black.end();
    black.begin({0,0},30,Drag::HueSaturation,false); black.update({15,0},30,false);
    near(black.hsv.s,.5,"saturation moves at zero value");
    for(double value:black.rgb)near(value,0,"saturation must not promote zero to one"); black.end();
    State relative; relative.sync({.5,.25,.25}); const RGB original=relative.rgb;
    relative.begin({-20,8},30,Drag::HueSaturation,false); relative.update({-20,8},30,false);
    check(relative.rgb==original,"off-center relative click does not jump");
    relative.update({-14,8},30,false); near(relative.hsv.s,.7,"relative drag uses the existing puck plus delta"); relative.end();
    relative.begin({-20,8},30,Drag::Saturation,false); relative.update({-8,8},30,false);
    near(relative.hsv.h,0,"Shift constrains hue");relative.end();
    relative.begin({30,0},30,Drag::Hue,true);double saturation=relative.hsv.s;
    relative.update({0,-30},30,false);near(relative.hsv.s,saturation,"Ctrl/ring constrains saturation");relative.end();
    State absolute;absolute.sync({1,1,1});absolute.begin({0,0},30,Drag::HueSaturation,false);absolute.update({0,-30},30,true);
    near(absolute.hsv.h,.25,"Alt uses absolute hue");near(absolute.hsv.s,1,"Alt uses absolute saturation");absolute.end();
    State brightness;brightness.sync({0,0,0});brightness.begin({0,0},30,Drag::Value,false);brightness.update({20,0},30,false);
    for(double v:brightness.rgb)near(v,.1,"Ctrl+Shift adjusts value from black");brightness.end();
    const RGB magenta=fromTMI({0,.85,-.005});
    near(magenta[0],.2783333333333333,"recorded magenta R");near(magenta[1],-.5716666666666667,"recorded magenta G");near(magenta[2],magenta[0],"magenta preserves R/B symmetry");
    const RGB warm=fromTMI({.25,.45,-.004995});
    near(warm[0],.270005,"recorded temperature R");near(warm[1],-.304995,"temperature preserves G");near(warm[2],.020005,"recorded temperature B");
    const RGB cool=fromTMI({-.7,.45,-.004995});
    near(cool[0],-.204995,"recorded cool R");near(cool[1],-.304995,"recorded cool G");near(cool[2],.495005,"recorded cool B");
    near(fromRGB(magenta).h,1./3,"negative-dominant colors use the recorded complementary hue");
    for(RGB sample:{RGB{0,0,0},RGB{1,1,1},RGB{-.06,-.06,-.06},magenta,warm,cool,RGB{2,-1,.25}}) {
        auto rebuilt=toRGB(fromRGB(sample));auto tmi=fromTMI(toTMI(sample));
        for(int i=0;i<3;++i){near(rebuilt[i],sample[i],"signed HDR HSV round-trip");near(tmi[i],sample[i],"TMI round-trip");}
    }
    std::printf("%d color-wheel checks, %d failed\n",checks,failed);return failed?1:0;
}
