#pragma once

// Engine-independent two-link geometry. All coordinates are world space; the
// adapter supplies the live shoulder axis and actual lengths for each skeleton.
#include <algorithm>
#include <cmath>

namespace DualWieldIK {
struct Vec {
    float x, y, z;
    Vec operator+(Vec b) const { return {x+b.x,y+b.y,z+b.z}; }
    Vec operator-(Vec b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vec operator*(float k) const { return {x*k,y*k,z*k}; }
};
inline float dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec cross(Vec a, Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline float length(Vec a) { return std::sqrt(dot(a,a)); }
inline bool finite(Vec a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
inline Vec unit(Vec a, Vec fallback) {
    const float l=length(a);
    return std::isfinite(l)&&l>1e-6f ? a*(1.f/l) : fallback;
}
inline float clamp(float x,float lo,float hi) { return std::max(lo,std::min(hi,x)); }
inline Vec project(Vec a, Vec n) { return a-n*dot(a,n); }
inline Vec reflect(Vec a, Vec n) { return a-n*(2.f*dot(a,n)); }
constexpr float rad=0.0174532925199433f;

struct Input {
    Vec nativeShoulder, nativeElbow, nativeHand;
    Vec shoulder, elbow, hand;
    Vec forward, up;
    Vec target;                    // shared world target when lock-on is active
    Vec nativeAim{};               // native weapon world direction, not reflected
    bool locked=false;
    float blend=1.f;
    float reachScale=.98f;
    float elbowFlareDeg=0.f;
};
struct Output {
    Vec elbow, hand, outside, forward;
    float upperLength, lowerLength, clearance;
};

// A compact shooting envelope in the live torso plane. Limit lateral spread
// relative to total arm length, not just world yaw. Using the same pitch plane
// as the native gun keeps the forearm forward instead of mirroring a bent arm.
inline Vec safeDirection(Vec v, Vec outside, Vec forward, Vec up) {
    v=unit(v,forward);
    const float x=dot(v,outside), y=dot(v,forward), z=clamp(dot(v,up),-1.f,1.f);
    const float horizontal=std::sqrt(x*x+y*y);
    // Never swing around to the rear/side to chase an unreachable lock-on.
    // An inboard target saturates at zero yaw and retains elevation.
    if(horizontal<1e-5f) return up*(z>=0.f?1.f:-1.f);
    const float yaw=std::asin(clamp(x/horizontal,0.f,std::sin(16.f*rad)));
    return unit(outside*(horizontal*std::sin(yaw))+
                forward*(horizontal*std::cos(yaw))+up*z,forward);
}

inline bool solve(const Input& in, Output& out) {
    if(!finite(in.nativeShoulder)||!finite(in.nativeElbow)||!finite(in.nativeHand)||
       !finite(in.shoulder)||!finite(in.elbow)||!finite(in.hand)||
       !finite(in.forward)||!finite(in.up)||(in.locked&&!finite(in.target))||
       !std::isfinite(in.blend)||!std::isfinite(in.reachScale)||!std::isfinite(in.elbowFlareDeg)) return false;
    const Vec span=in.shoulder-in.nativeShoulder;
    const float s=length(span), a=length(in.elbow-in.shoulder), b=length(in.hand-in.elbow);
    const float nativeLength=length(in.nativeElbow-in.nativeShoulder)+length(in.nativeHand-in.nativeElbow);
    if(!std::isfinite(s)||!std::isfinite(a)||!std::isfinite(b)||!std::isfinite(nativeLength)||
       s<1e-4f||a<1e-4f||b<1e-4f||nativeLength<1e-4f||s>4.f*(a+b)) return false;
    const Vec outside=span*(1.f/s);
    // Model basis is used only to disambiguate front/up on the actual shoulder plane.
    const Vec upHint=unit(project(in.up,outside),{0,0,0});
    Vec forward=unit(project(in.forward,outside),{0,0,0});
    if(length(forward)<.5f || std::fabs(dot(forward,upHint))>.995f) {
        forward=unit(cross(outside,upHint),{0,0,0});
        if(dot(forward,in.forward)<0.f) forward=forward*(-1.f);
    }
    if(length(forward)<.5f) return false;
    Vec up=unit(project(upHint,forward),{0,0,0});
    if(length(up)<.5f) return false;

    const Vec nativeReach=in.nativeHand-in.nativeShoulder;
    const Vec nativeForearm=unit(in.nativeHand-in.nativeElbow,forward);
    // The right shoulder->wrist vector can be sideways even though its gun is
    // pointing forward. Reflecting that reach produced a wide left-arm punch.
    // Take actual aim from the native gun, with a forearm fallback for bad data.
    Vec desired=finite(in.nativeAim)&&length(in.nativeAim)>.5f
        ? unit(in.nativeAim,nativeForearm) : nativeForearm;
    if(in.locked) {
        const Vec delta=in.target-in.shoulder;
        const float dist=length(delta);
        if(std::isfinite(dist)&&dist>1e-4f) {
            // A target within the arm's own reach cannot dictate its extension;
            // blend target intent smoothly instead of folding the wrist inward.
            const float weight=clamp(dist/(a+b),0.f,1.f);
            desired=unit(desired*(1.f-weight)+unit(delta,desired)*weight,desired);
        }
    }
    const float blend=clamp(in.blend,0.f,1.f);
    const Vec desiredDir=safeDirection(desired,outside,forward,up);
    const Vec current=unit(in.hand-in.shoulder,desiredDir);
    const Vec dir=safeDirection(current*(1.f-blend)+desiredDir*blend,outside,forward,up);

    // A gun is held in an extended firing stance even when its target is very
    // close. Treat native extension as a preference, not an excuse to collapse
    // the arm into a defensive punch. Deliberate INI reach caps still apply.
    const float minReach=std::max(std::sqrt(a*a+b*b+2.f*a*b*std::cos(130.f*rad)),
                                  std::sqrt(std::max(0.f,b*b-a*a)));
    const float maxReach=std::sqrt(a*a+b*b+2.f*a*b*std::cos(8.f*rad));
    const float cap=std::max(minReach,std::min(maxReach,(a+b)*clamp(in.reachScale,.70f,1.f)));
    const float minAimReach=std::min(cap,(a+b)*.95f);
    const float nativeRatio=clamp(length(nativeReach)/nativeLength,0.f,1.f);
    const float aimReach=clamp((a+b)*nativeRatio,minAimReach,cap);
    const float reach=clamp(length(in.hand-in.shoulder)*(1.f-blend)+aimReach*blend,
                            std::max(minReach,std::min(cap,(a+b)*(.82f+.13f*blend))),cap);
    const float along=(a*a-b*b+reach*reach)/(2.f*reach);
    const float radius=std::sqrt(std::max(0.f,a*a-along*along));

    // Select the elbow on its exact solution circle. The default bend plane
    // is vertical (elbow below the aim ray), NOT outward. The lateral basis
    // and pitch basis are orthogonal; their coefficients let us enforce a
    // small shoulder-relative envelope without changing either bone length.
    const Vec outerPole=unit(project(outside,dir),outside);
    Vec pitchPole=unit(cross(outside,dir),up*(-1.f));
    if(dot(cross(outside,forward),up)>0.f) pitchPole=pitchPole*(-1.f);
    const float clearance=-.025f*s;
    const float elbowCenter=along*dot(dir,outside);
    const float lateralRadius=radius*dot(outerPole,outside);
    // Allow a little natural lateral elbow position, and optionally widen the
    // envelope by user-requested flare. Even full flare is bounded.
    const float maxElbowOut=std::max(.19f*a,.10f*s)+a*.16f*clamp(in.elbowFlareDeg/15.f,0.f,1.f);
    const float preferredElbowOut=std::min(maxElbowOut,.075f*a+a*.13f*clamp(in.elbowFlareDeg/15.f,0.f,1.f));
    float lateral=0.f;
    if(lateralRadius>1e-6f) {
        const float lo=clamp((clearance-elbowCenter)/lateralRadius,-1.f,1.f);
        const float hi=clamp((maxElbowOut-elbowCenter)/lateralRadius,-1.f,1.f);
        lateral=clamp((preferredElbowOut-elbowCenter)/lateralRadius,lo,hi);
    }
    const Vec pole=outerPole*lateral+pitchPole*std::sqrt(std::max(0.f,1.f-lateral*lateral));
    const Vec elbow=in.shoulder+dir*along+pole*radius;
    const Vec hand=in.shoulder+dir*reach;
    if(!finite(elbow)||!finite(hand)||dot(elbow-in.shoulder,outside)<clearance-1e-5f||
       dot(hand-in.shoulder,outside)<clearance-1e-5f) return false;
    out={elbow,hand,outside,forward,a,b,clearance};
    return true;
}

// Check the actual rebuilt bone matrices, rather than trusting a rotation API's
// successful return. Linear segments remain outside the protected shoulder plane
// when both endpoints do. Tolerance is relative to the live limb lengths.
inline bool validate(const Input& in, const Output& out, Vec elbow, Vec hand) {
    const float tol=.02f*(out.upperLength+out.lowerLength);
    return finite(elbow)&&finite(hand)&&
        length(elbow-out.elbow)<=tol && length(hand-out.hand)<=tol &&
        dot(elbow-in.shoulder,out.outside)>=out.clearance-tol &&
        dot(hand-in.shoulder,out.outside)>=out.clearance-tol;
}
}
