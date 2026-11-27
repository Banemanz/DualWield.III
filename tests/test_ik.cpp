#include "../ArmIK.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace DualWieldIK;
static unsigned checks=0, poses=0;
static void require(bool ok,const char* what) {
    ++checks;
    if(!ok) { std::cerr<<"FAIL: "<<what<<" check "<<checks<<" pose "<<poses<<"\n"; std::exit(1); }
}
static Vec aim(float yaw,float pitchDeg) {
    const float h=std::cos(pitchDeg*rad);
    return {h*std::sin(yaw*rad),h*std::cos(yaw*rad),std::sin(pitchDeg*rad)};
}
static Input sample(float yaw,float pitchDeg,float ratio=1.f) {
    Input in{};
    in.shoulder={-.22f,0,1.45f};
    in.nativeShoulder={.22f,0,1.45f};
    in.elbow=in.shoulder+Vec{-.04f,.06f,-.302f}*ratio;
    in.hand=in.elbow+Vec{.02f,.09f,-.265f}*ratio;
    const Vec dir=aim(yaw,pitchDeg);
    in.nativeElbow=in.nativeShoulder+dir*.30f+Vec{.04f,0,0};
    in.nativeHand=in.nativeElbow+dir*.28f;
    in.nativeAim=dir;
    in.forward={0,1,0}; in.up={0,0,1};
    return in;
}
static Vec rotate(Vec p) {
    const float c=.70710678118f;
    return {p.x*c-p.y*c,(p.x*c+p.y*c)*.8660254f-p.z*.5f,
            (p.x*c+p.y*c)*.5f+p.z*.8660254f};
}
static Vec move(Vec p) { return rotate(p)+Vec{3.f,-7.f,2.f}; }
static Input transformed(Input in) {
    in.shoulder=move(in.shoulder); in.elbow=move(in.elbow); in.hand=move(in.hand);
    in.nativeShoulder=move(in.nativeShoulder); in.nativeElbow=move(in.nativeElbow);
    in.nativeHand=move(in.nativeHand); in.target=move(in.target);
    in.forward=rotate(in.forward); in.up=rotate(in.up); in.nativeAim=rotate(in.nativeAim);
    return in;
}
static void checkPose(const Input& in,const Output& o) {
    require(validate(in,o,o.elbow,o.hand),"output validation");
    require(std::fabs(length(o.elbow-in.shoulder)-o.upperLength)<1e-4f,"upper length");
    require(std::fabs(length(o.hand-o.elbow)-o.lowerLength)<1e-4f,"lower length");
    require(segmentClear(in.shoulder,o.elbow,in.shoulder,o.outside,o.forward,o.clearance,o.frontDepth,1e-5f),"upper chest clearance");
    require(segmentClear(o.elbow,o.hand,in.shoulder,o.outside,o.forward,o.clearance,o.frontDepth,1e-5f),"forearm chest clearance");
    require(dot(o.elbow-in.shoulder,o.outside)<=std::sin(30.f*rad)*o.upperLength+1e-5f,"upper arm doesn't flare");
    require(dot(unit(o.hand-o.elbow,o.forward),o.forearmAim)>.99999f,"forearm exactly follows achieved aim");
    require(!validate(in,o,o.elbow+o.outside*(-1.f),o.hand),"reject corrupted matrix");
}
int main(int argc,char**) {
    bool csv=argc>1;
    if(csv) std::cout<<"yaw,pitch,distance,shoulderX,shoulderY,elbowX,elbowY,handX,handY,targetX,targetY\n";
    for(int yaw=-180;yaw<=180;yaw+=5) for(int p=-90;p<=90;p+=5)
    for(float ratio:{.75f,1.f,1.25f}) for(float blend:{.25f,.5f,1.f})
    for(float distance:{.08f,.3f,1.f,20.f}) for(bool lock:{false,true}) {
        auto in=sample(float(yaw),float(p),ratio);
        in.locked=lock; in.blend=blend;
        in.target=in.nativeShoulder+aim(float(yaw),float(p))*distance;
        Output out{};
        require(solve(in,out),"finite reachable solve");
        checkPose(in,out);
        if(blend==1.f&&!lock) {
            require(std::fabs(dot(out.forearmAim,out.up)-dot(in.nativeAim,out.up))<1e-4f,
                    "preserve native elevation");
            // In the forward cone, the forearm must follow the real ray, including
            // inboard directions. It must never be silently projected outward.
            if(std::abs(yaw)<=45&&std::abs(p)<=45&&dot(out.elbow-in.shoulder,out.forward)>=out.frontDepth)
                require(dot(out.forearmAim,in.nativeAim)>.999f,"forward aim follows real ray");
        }
        if(!csv && (poses%311==0)) {
            Output moved{};
            require(solve(transformed(in),moved),"rotated rig");
            require(length(moved.elbow-move(out.elbow))<.01f,"rigid equivariance elbow");
            require(length(moved.hand-move(out.hand))<.01f,"rigid equivariance hand");
        }
        if(csv&&ratio==1.f&&blend==1.f&&lock&&p==0&&yaw%45==0&&distance==1.f)
            std::cout<<yaw<<','<<p<<','<<distance<<','<<in.shoulder.x<<','<<in.shoulder.y<<','
                <<out.elbow.x<<','<<out.elbow.y<<','<<out.hand.x<<','<<out.hand.y<<','
                <<in.target.x<<','<<in.target.y<<'\n';
        ++poses;
    }
    for(float a:{.18f,.23f,.30f,.40f,.50f})
    for(float b:{.18f,.23f,.30f,.40f,.50f})
    for(float reachScale:{.70f,.85f,.98f,1.f})
    for(float blend:{.25f,1.f})
    for(int yaw=-180;yaw<=180;yaw+=10)
    for(int p=-90;p<=90;p+=15) {
        auto custom=sample(0,0);
        custom.elbow=custom.shoulder+Vec{0,0,-a};
        custom.hand=custom.elbow+Vec{0,0,-b};
        custom.nativeAim=aim(float(yaw),float(p));
        custom.target=custom.shoulder+custom.nativeAim*.6f;
        custom.locked=true; custom.blend=blend; custom.reachScale=reachScale;
        Output out{};
        require(solve(custom,out),"asymmetric rig");
        checkPose(custom,out);
        ++poses;
    }
    // Screenshot-inspired yaw case: torso and camera do NOT share heading.
    // The target is to the right from the left shoulder. v23's x>=0 wrist
    // constraint forced the left forearm away from this aim direction.
    auto in=sample(0,0);
    in.nativeAim=aim(40,15);
    in.nativeElbow=in.nativeShoulder+Vec{.12f,.25f,-.05f};
    in.nativeHand=in.nativeElbow+in.nativeAim*.28f;
    Output o{};
    require(solve(in,o),"camera right of torso");
    checkPose(in,o);
    require(dot(o.forearmAim,in.nativeAim)>.999f,"actual camera ray reached");
    require(dot(o.hand-in.shoulder,o.outside)<0.f,"wrist can safely converge in front");
    require(dot(o.elbow-in.shoulder,o.forward)>o.frontDepth,"elbow ahead of chest");
    require(dot(o.elbow-in.shoulder,o.outside)>-.095f,"upper arm not folded through chest");
    in.locked=true; in.target=in.shoulder+in.nativeAim*20.f;
    require(solve(in,o),"controller right of torso");
    require(dot(o.forearmAim,in.nativeAim)>.999f,"actual lock ray reached");
    in.target=in.shoulder+Vec{.09f,.045f,-.15f};
    require(solve(in,o),"close cross body");
    checkPose(in,o);
    require(length(o.hand-in.shoulder)>.70f*(o.upperLength+o.lowerLength),"point blank avoids excessive folding");
    // A midpoint inside the exclusion volume must be rejected even when the
    // wrist endpoint is far forward.
    require(!segmentClear(in.shoulder,in.shoulder+Vec{.2f,.05f,0},in.shoulder,{-1,0,0},{0,1,0},.015f,.13f),"actual torso intersection rejected");
    in=sample(0,0); in.nativeShoulder=in.shoulder; require(!solve(in,o),"collapsed shoulders");
    in=sample(0,0); in.hand=in.elbow; require(!solve(in,o),"collapsed forearm");
    in=sample(0,0); in.blend=std::numeric_limits<float>::quiet_NaN(); require(!solve(in,o),"invalid config");
    in=sample(0,0); in.locked=true; in.target.x=std::numeric_limits<float>::infinity(); require(!solve(in,o),"invalid target");
    std::cerr<<"PASS: "<<poses<<" pose combinations, "<<checks<<" assertions\n";
}
