#include "../ArmIK.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace DualWieldIK;
static unsigned checks=0;
static void require(bool ok,const char* what) {
    ++checks;
    if(!ok) { std::cerr<<"FAIL: "<<what<<" check "<<checks<<"\n"; std::exit(1); }
}
static Vec aim(float yaw,float pitch) {
    const float h=std::cos(pitch*rad);
    return {h*std::sin(yaw*rad),h*std::cos(yaw*rad),std::sin(pitch*rad)};
}
static Input sample(float yaw,float pitch,float ratio=1.f) {
    Input in{};
    in.shoulder={-.22f,0,1.45f};
    in.nativeShoulder={.22f,0,1.45f};
    in.elbow=in.shoulder+Vec{-.04f,.06f,-.302f}*ratio;
    in.hand=in.elbow+Vec{.02f,.09f,-.265f}*ratio;
    const Vec dir=aim(yaw,pitch);
    in.nativeElbow=in.nativeShoulder+dir*.30f+Vec{.04f,0,0};
    in.nativeHand=in.nativeElbow+dir*.28f;
    in.forward={0,1,0}; in.up={0,0,1};
    return in;
}
static Vec rotate(Vec p) { // orthogonal proper rotation + translation
    const float c=.70710678118f;
    return {p.x*c-p.y*c,(p.x*c+p.y*c)*.8660254f-p.z*.5f,
            (p.x*c+p.y*c)*.5f+p.z*.8660254f};
}
static Vec move(Vec p) { return rotate(p)+Vec{3.f,-7.f,2.f}; }
static Input transformed(Input in) {
    in.shoulder=move(in.shoulder); in.elbow=move(in.elbow); in.hand=move(in.hand);
    in.nativeShoulder=move(in.nativeShoulder); in.nativeElbow=move(in.nativeElbow);
    in.nativeHand=move(in.nativeHand); in.target=move(in.target);
    in.forward=rotate(in.forward); in.up=rotate(in.up);
    return in;
}
int main(int argc,char**) {
    unsigned poses=0;
    const bool csv=argc>1;
    if(csv) std::cout<<"yaw,pitch,distance,shoulderX,shoulderY,elbowX,elbowY,handX,handY,targetX,targetY\n";
    for(int yaw=-180;yaw<=180;yaw+=5) for(int pitch=-90;pitch<=90;pitch+=5)
    for(float ratio:{.75f,1.f,1.25f}) for(float blend:{.25f,.5f,1.f})
    for(float distance:{.08f,.3f,1.f,20.f}) for(bool lock:{false,true}) {
        auto in=sample(float(yaw),float(pitch),ratio);
        in.locked=lock; in.blend=blend;
        in.target=in.nativeShoulder+aim(float(yaw),float(pitch))*distance;
        Output out{};
        require(solve(in,out),"finite reachable solve");
        require(validate(in,out,out.elbow,out.hand),"output validation");
        require(std::fabs(length(out.elbow-in.shoulder)-out.upperLength)<1e-4f,"upper length");
        require(std::fabs(length(out.hand-out.elbow)-out.lowerLength)<1e-4f,"lower length");
        require(dot(out.elbow-in.shoulder,out.outside)>=out.clearance-1e-5f,"elbow torso clearance");
        require(dot(out.hand-in.shoulder,out.outside)>=out.clearance-1e-5f,"wrist torso clearance");
        const Vec upper=unit(out.elbow-in.shoulder,{0,1,0});
        const Vec lower=unit(out.hand-out.elbow,{0,1,0});
        const float flex=std::acos(clamp(dot(upper,lower),-1.f,1.f))/rad;
        require(flex>=7.95f&&flex<=130.05f,"elbow flexion");
        require(!validate(in,out,out.elbow+out.outside*(-1.f),out.hand),"reject corrupted matrix");
        if(!csv && (poses%311==0)) {
            Output moved{};
            require(solve(transformed(in),moved),"rotated rig");
            require(length(moved.elbow-move(out.elbow))<.02f,"rigid equivariance elbow");
            require(length(moved.hand-move(out.hand))<.02f,"rigid equivariance hand");
        }
        if(csv&&ratio==1.f&&blend==1.f&&lock&&pitch==0&&yaw%45==0&&distance==1.f)
            std::cout<<yaw<<','<<pitch<<','<<distance<<','<<in.shoulder.x<<','<<in.shoulder.y<<','
                <<out.elbow.x<<','<<out.elbow.y<<','<<out.hand.x<<','<<out.hand.y<<','
                <<in.target.x<<','<<in.target.y<<'\n';
        ++poses;
    }
    Output o{};
    auto in=sample(0,0);
    require(solve(in,o),"straight ahead");
    in=sample(0,85); require(solve(in,o)&&o.hand.z>in.shoulder.z,"upward aim");
    in=sample(0,-85); require(solve(in,o)&&o.hand.z<in.shoulder.z,"downward aim");
    in=sample(0,0); in.nativeShoulder=in.shoulder; require(!solve(in,o),"collapsed shoulders");
    in=sample(0,0); in.hand=in.elbow; require(!solve(in,o),"collapsed forearm");
    in=sample(0,0); in.blend=std::numeric_limits<float>::quiet_NaN(); require(!solve(in,o),"invalid config");
    in=sample(0,0); in.locked=true; in.target.x=std::numeric_limits<float>::infinity(); require(!solve(in,o),"invalid target");
    std::cerr<<"PASS: "<<poses<<" pose combinations, "<<checks<<" assertions\n";
}
