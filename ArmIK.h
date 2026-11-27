#pragma once

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
constexpr float rad=0.0174532925199433f;

struct Input {
    Vec nativeShoulder, nativeElbow, nativeHand;
    Vec shoulder, elbow, hand;
    Vec forward, up;
    Vec target;
    Vec nativeAim{}; // real camera/hand aim ray, never a muzzle-position offset
    bool locked=false;
    float blend=1.f;
    float reachScale=.98f;
    float elbowFlareDeg=0.f;
};
struct Output {
    Vec elbow, hand, outside, forward, up;
    Vec requestedAim, forearmAim;
    float upperLength, lowerLength, clearance, frontDepth;
};

inline float pitch(Vec v, Vec up) { return std::asin(clamp(dot(v,up),-1.f,1.f)); }
inline Vec direction(Vec outside, Vec forward, Vec up, float yaw, float elevation) {
    const float h=std::cos(elevation);
    return unit(outside*(std::sin(yaw)*h)+forward*(std::cos(yaw)*h)+up*std::sin(elevation),forward);
}
// Stable front-hemisphere yaw. Rearward targets saturate continuously rather than
// flipping from one side to the other as the look direction crosses +/-180.
inline float frontYaw(Vec v, Vec outside, Vec forward, float minYaw, float maxYaw) {
    const float x=dot(v,outside), y=dot(v,forward);
    const float h=std::sqrt(x*x+y*y);
    if(h<1e-5f) return 0.f;
    return clamp(std::asin(clamp(x/h,-1.f,1.f)),minYaw,maxYaw);
}

// Local front-of-chest exclusion region. A shoulder-side point in front of the
// chest is safe even if it crosses the shoulder plane. Check a whole segment up
// to the front boundary, not merely its endpoints. The torso has finite depth.
inline bool segmentClear(Vec a, Vec b, Vec shoulder, Vec outside, Vec forward,
                         float margin, float depth, float tolerance=0.f) {
    a=a-shoulder; b=b-shoulder;
    float ay=dot(a,forward), by=dot(b,forward);
    float ax=dot(a,outside), bx=dot(b,outside);
    if(ay>depth&&by>depth) return true;
    // Restrict segment to the portion behind the front clear plane.
    if(ay>depth) { ax=bx+(ax-bx)*(depth-by)/(ay-by); ay=depth; }
    if(by>depth) { bx=ax+(bx-ax)*(depth-ay)/(by-ay); by=depth; }
    // Sloped shoulder/chest outline; slight inboard travel allowed as the
    // limb advances forward, but not back through the torso.
    if((ay<0.f&&by>0.f)||(by<0.f&&ay>0.f)) {
        const float xAtZero=ax+(bx-ax)*(-ay)/(by-ay);
        if(xAtZero < -margin-tolerance) return false;
    }
    return ax+.35f*std::max(0.f,ay)>=-margin-tolerance &&
           bx+.35f*std::max(0.f,by)>=-margin-tolerance;
}

inline bool solve(const Input& in, Output& out) {
    if(!finite(in.nativeShoulder)||!finite(in.nativeElbow)||!finite(in.nativeHand)||
       !finite(in.shoulder)||!finite(in.elbow)||!finite(in.hand)||
       !finite(in.forward)||!finite(in.up)||(in.locked&&!finite(in.target))||
       !std::isfinite(in.blend)||!std::isfinite(in.reachScale)||!std::isfinite(in.elbowFlareDeg)) return false;
    const Vec span=in.shoulder-in.nativeShoulder;
    const float s=length(span), a=length(in.elbow-in.shoulder), b=length(in.hand-in.elbow);
    const float na=length(in.nativeElbow-in.nativeShoulder), nb=length(in.nativeHand-in.nativeElbow);
    if(!std::isfinite(s)||!std::isfinite(a)||!std::isfinite(b)||!std::isfinite(na)||!std::isfinite(nb)||
       s<1e-4f||a<1e-4f||b<1e-4f||na<1e-4f||nb<1e-4f||s>4.f*(a+b)) return false;
    const Vec outside=span*(1.f/s);
    const Vec up=unit(project(in.up,outside),{0,0,0});
    if(length(up)<.5f) return false;
    Vec forward=unit(cross(outside,up),{0,0,0});
    if(dot(forward,in.forward)<0.f) forward=forward*(-1.f);
    if(length(forward)<.5f) return false;
    const Vec nativeUpper=unit(in.nativeElbow-in.nativeShoulder,forward);
    const Vec nativeLower=unit(in.nativeHand-in.nativeElbow,forward);
    Vec requested=unit(in.nativeAim,nativeLower);
    if(in.locked) {
        const Vec delta=in.target-in.shoulder;
        if(length(delta)>1e-4f) {
            const float weight=clamp(length(delta)/(a+b),0.f,1.f);
            requested=unit(requested*(1.f-weight)+unit(delta,requested)*weight,requested);
        }
    }

    // Forearm, not shoulder-to-wrist chord, tracks the actual aim. This avoids
    // the v22/v23 camera/torso divergence. The upper arm supplies the elbow
    // support and has a much narrower yaw range than the pointing forearm.
    const float blend=clamp(in.blend,0.f,1.f);
    const Vec lowerIntent=unit(unit(in.hand-in.elbow,requested)*(1.f-blend)+requested*blend,requested);
    const float aimPitch=pitch(lowerIntent,up);
    float lowerYaw=frontYaw(lowerIntent,outside,forward,-83.f*rad,60.f*rad);
    const float upperYaw=clamp(lowerYaw*.58f,-17.f*rad,25.f*rad)+
                         clamp(in.elbowFlareDeg,0.f,15.f)*.30f*rad;
    // Native animation provides the bend amount. Limit the vertical difference,
    // since native sideways elbow flare is not an appropriate left-arm template.
    const float nativeBend=clamp(pitch(nativeLower,up)-pitch(nativeUpper,up),8.f*rad,28.f*rad);
    const float reachCap=(a+b)*clamp(in.reachScale,.70f,1.f);
    const float capBend=std::acos(clamp((reachCap*reachCap-a*a-b*b)/(2.f*a*b),-1.f,1.f));
    const float bend=clamp(std::max(nativeBend,capBend),8.f*rad,48.f*rad);
    // Keep the elbow lower than the gun except at very steep downward pitch.
    const float upperPitch=clamp(aimPitch-bend,-78.f*rad,82.f*rad);
    const Vec upperDir=direction(outside,forward,up,upperYaw,upperPitch);
    const Vec elbow=in.shoulder+upperDir*a;
    const float margin=.035f*s, depth=.30f*s;
    // Upper segment is within the shoulder/chest slope by construction:
    // tan(17 degrees)<0.35, and the ray always has a forward component.
    // A forearm already outside the front plane can turn inward freely. If
    // not, use the exact tangent slope to the sloped exclusion boundary.
    const Vec localElbow=elbow-in.shoulder;
    const float ex=dot(localElbow,outside), ey=dot(localElbow,forward);
    if(ey<depth) {
        const float tangentYaw=std::atan2(-margin-.35f*depth-ex,depth-ey);
        lowerYaw=std::max(lowerYaw,tangentYaw);
    }
    const Vec forearm=direction(outside,forward,up,lowerYaw,aimPitch);
    const Vec hand=elbow+forearm*b;
    if(!finite(elbow)||!finite(hand)||
       !segmentClear(in.shoulder,elbow,in.shoulder,outside,forward,margin,depth,1e-5f)||
       !segmentClear(elbow,hand,in.shoulder,outside,forward,margin,depth,1e-5f)) return false;
    out={elbow,hand,outside,forward,up,requested,forearm,a,b,margin,depth};
    return true;
}

// Validate measured rebuilt joint positions and both complete segments.
inline bool validate(const Input& in, const Output& out, Vec elbow, Vec hand) {
    const float tol=.02f*(out.upperLength+out.lowerLength);
    return finite(elbow)&&finite(hand)&&
        length(elbow-out.elbow)<=tol && length(hand-out.hand)<=tol &&
        segmentClear(in.shoulder,elbow,in.shoulder,out.outside,out.forward,out.clearance,out.frontDepth,tol)&&
        segmentClear(elbow,hand,in.shoulder,out.outside,out.forward,out.clearance,out.frontDepth,tol);
}
}
