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
    bool locked=false;
    float blend=1.f;
    float reachScale=.98f;
    float elbowFlareDeg=0.f;
};
struct Output {
    Vec elbow, hand, outside, forward;
    float upperLength, lowerLength, clearance;
};

// Keep the wrist in an outward/forward yaw sector without cutting off pitch.
// A small outward yaw at straight ahead avoids the chest even at steep pitch.
// Backward lock-on is saturated at the side rather than folding across the body.
inline Vec safeDirection(Vec v, Vec outside, Vec forward, Vec up) {
    v=unit(v,forward);
    const float x=dot(v,outside), y=dot(v,forward), z=clamp(dot(v,up),-1.f,1.f);
    const float horizontal=std::sqrt(x*x+y*y);
    if(horizontal<1e-5f) return up*(z>=0.f?1.f:-1.f);
    // Rearward/inboard targets use the forward limit continuously across
    // the cross-body hemisphere; rearward/outboard targets use the side limit.
    float yaw=std::atan2(x,y);
    if(y<0.f) yaw=3.f*rad+82.f*rad*clamp(x/horizontal*1.5f,0.f,1.f);
    yaw=clamp(yaw, 3.f*rad, 85.f*rad);
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
    Vec desired=reflect(nativeReach,outside); // natural symmetrical free aim
    if(in.locked && length(in.target-in.shoulder)>1e-4f)
        desired=in.target-in.shoulder; // both arms aim at the same target, not opposite yaw
    float blend=clamp(in.blend,0.f,1.f);
    const Vec desiredDir=safeDirection(desired,outside,forward,up);
    const Vec current=unit(in.hand-in.shoulder,desiredDir);
    const Vec dir=safeDirection(current*(1.f-blend)+desiredDir*blend,outside,forward,up);

    // Limit elbow flexion by reach (8..130 degrees), independent of interpolation
    // pose or frame rate. The arm length ratio is preserved for asymmetric rigs.
    const float minReach=std::max(std::sqrt(a*a+b*b+2.f*a*b*std::cos(130.f*rad)),
                                  std::sqrt(std::max(0.f,b*b-a*a)));
    const float maxReach=std::sqrt(a*a+b*b+2.f*a*b*std::cos(8.f*rad));
    const float cap=std::max(minReach,std::min(maxReach,(a+b)*clamp(in.reachScale,.70f,1.f)));
    const float nativeRatio=clamp(length(nativeReach)/nativeLength,0.f,1.f);
    float reach=clamp(length(in.hand-in.shoulder)*(1.f-blend)+(a+b)*nativeRatio*blend,minReach,cap);
    if(in.locked) reach=clamp(std::min(reach,length(desired)),minReach,cap);
    const float along=(a*a-b*b+reach*reach)/(2.f*reach);
    const float radius=std::sqrt(std::max(0.f,a*a-along*along));

    // Prefer the mirrored native elbow plane when it is anatomically safe.
    // Bias toward the outside and down, not an arbitrary per-animation rotation cap.
    const Vec outerPole=unit(project(outside,dir),unit(project(up*(-1.f),dir),forward));
    const Vec natural=unit(project(reflect(in.nativeElbow-in.nativeShoulder,outside),dir),outerPole);
    const Vec down=unit(project(up*(-1.f),dir),outerPole);
    Vec pole=unit(project(natural*.60f + outerPole*(.75f+clamp(in.elbowFlareDeg,0.f,15.f)*.045f)
                          + down*.18f,dir),outerPole);
    const float clearance=-.035f*s; // a tiny tolerance at the shoulder surface
    Vec elbow=in.shoulder+dir*along+pole*radius;
    const Vec hand=in.shoulder+dir*reach;
    if(dot(elbow-in.shoulder,outside)<clearance)
        elbow=in.shoulder+dir*along+outerPole*radius;
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
