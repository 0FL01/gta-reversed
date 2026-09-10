// Probe-only control-flow lift, independently transcribed from owned retail
// CWorldScan::ScanWorld 75F180..75FD7F (compact 72CAE0). No binary data or
// runtime executable access. Labels retain the original branch boundaries;
// scalar locals name stack slots, unlike the production edge abstraction.
// The separately extracted CGeneral::GetATanOfXY supplies hull angles.
#pragma once
#include <xmmintrin.h>

namespace water_scan_proof {
using Point = std::array<float,2>;
using Cell = std::array<int,2>;
inline RealtimeWaterScanPoints Points(const RealtimeWaterFrustum& f) {
    // Literal SSE dataflow, 841902..841917 (same in 841CD2..841CE7), plus
    // 7213A1..7213F8 local corners and PC24 721419..721491 block conversion.
    const auto load=[](Point p) { return _mm_set_ps(0,0,p[1],p[0]); };
    const auto xmm3=load(f.right),xmm4=load(f.up),xmm5=load(f.at),xmm6=load(f.position);
    RealtimeWaterScanPoints out{};
    const float x=float(f.viewWindow[0]*f.farClip),y=float(f.viewWindow[1]*f.farClip);
    const std::array<std::array<float,3>,5> local{{{-x,y,f.farClip},{x,y,f.farClip},
        {x,-y,f.farClip},{-x,-y,f.farClip},{0,0,0}}};
    for (int i=0;i<5;++i) {
        volatile auto xmm0=_mm_mul_ps(_mm_set1_ps(local[i][0]),xmm3);
        volatile auto xmm1=_mm_mul_ps(_mm_set1_ps(local[i][1]),xmm4);
        volatile auto xmm2=_mm_mul_ps(_mm_set1_ps(local[i][2]),xmm5);
        xmm0=_mm_add_ps(xmm0,xmm6); xmm2=_mm_add_ps(xmm2,xmm1); xmm2=_mm_add_ps(xmm2,xmm0);
        xmm2=_mm_div_ps(xmm2,_mm_set1_ps(500)); xmm2=_mm_add_ps(xmm2,_mm_set1_ps(6));
        std::array<float,4> store{}; _mm_storeu_ps(store.data(),xmm2); out[i]={store[0],store[1]};
    }
    return out;
}
inline std::vector<Cell> Scan(std::array<Point,5> p) {
    int n=5;
    for (int i=0;i<n-1;++i) for (int j=i+1;j<n;++j) {
        if (p[i]==p[j]) { for (int k=j;k<n-1;++k) p[k]=p[k+1]; --n; --j; }
    }
    std::vector<Cell> result;
    if (n<3) return result; // source-valid frusta have at least three distinct points
    std::array<Point,5> h{};
    std::array<bool,5> visited{};
    int edi=0, ebx=0, hc=1;
    for (int i=1;i<n;++i) if (p[i][1]<p[edi][1]) edi=i;
    h[0]=p[edi]; visited[edi]=true;
    float direction=0;
L75F376:
    {
        float smallest=99999.8984375f;
        for (int esi=0;esi<n;++esi) if (esi!=edi) {
            const float dx=float(p[esi][0]-p[edi][0]),dy=float(p[esi][1]-p[edi][1]);
            float turn=float(water_oracle::CGeneral::GetATanOfXY(dx,dy)-direction);
            while (turn<=0) turn=float(double(turn)+6.283185482025146484375);
            while (turn>=6.283185482025146484375) turn=float(double(turn)-6.283185482025146484375);
            if (turn<smallest) { smallest=turn; ebx=esi; }
        }
        if (visited[ebx]) goto L75F479;
        h[hc++]=p[ebx]; visited[ebx]=true; edi=ebx;
        direction=float(smallest+direction);
    }
    goto L75F376;
L75F479:
    if (hc<3) return result;
    const auto floor=[](float f) { return int(std::floor(double(f))); };
    const auto prev=[&](int i) { return i==0 ? hc-1 : i-1; };
    const auto next=[&](int i) { return i+1==hc ? 0 : i+1; };
    // RW 89F404/89F432 installs PC24. These explicit extended calculations
    // round at EVERY lifted instruction, independently of native SSE codegen.
    const auto slope=[&](int a,int b) {
        const float dx=float(static_cast<long double>(h[b][0])-h[a][0]);
        const float dy=float(static_cast<long double>(h[b][1])-h[a][1]);
        return float(static_cast<long double>(dx)/dy);
    };
    const auto intercept=[&](int a,float s) {
        const float fraction=float(std::ceil(double(h[a][1]))-h[a][1]);
        const float product=float(static_cast<long double>(fraction)*s);
        return float(static_cast<long double>(product)+h[a][0]);
    };
    int start=0;
    float maximum=h[0][1];
    for (int i=1;i<hc;++i) {
        if (h[i][1]<h[start][1]) start=i;
        else if (h[i][1]>=maximum) maximum=h[i][1];
    }
    int s10=floor(h[start][1]),s24=floor(maximum),arg8=9999,argC=-9999;
    int s18=start,s20=start,a=start,b=start;
    for (int i=0;i<hc;++i) {
        a=s18; s18=prev(a); arg8=std::min(arg8,floor(h[a][0]));
        if (floor(h[a][1])!=floor(h[s18][1])) break;
    }
    for (int i=0;i<hc;++i) {
        b=s20; s20=next(b); argC=std::max(argC,floor(h[b][0]));
        if (floor(h[b][1])!=floor(h[s20][1])) break;
    }
    // In the single-row case the source emits these bounds before any
    // observable use of its slopes (possibly 0/0 for a horizontal edge).
    if (s10==s24) { for (int x=arg8;x<=argC;++x) result.push_back({x,s10}); return result; }
    float s1c=slope(a,s18),s14=slope(b,s20),s4=intercept(a,s1c),s8=intercept(b,s14);
    if (s1c<0) arg8=std::min(arg8,floor(s4));
    if (s14>=0) argC=std::max(argC,floor(s8));
L75F821:
    for (int x=arg8;x<=argC;++x) result.push_back({x,s10});
    if (s10==s24) return result; // remaining source arithmetic has no callbacks
    s8=float(s14+s8); s4=float(s1c+s4); ++s10;
    if (s10!=floor(h[s18][1])) goto L75FAC0;
    if (s10==s24) goto L75FA4C;
    edi=s1c<0 ? floor(h[s18][0]) : floor(float(s4-s1c));
L75F96F:
    a=s18; s18=prev(a); s4=h[a][0]; edi=std::min(edi,floor(s4));
    if (s10==floor(h[s18][1])) goto L75F96F;
    arg8=edi; s1c=slope(a,s18); s4=intercept(a,s1c);
    if (s1c<0) arg8=std::min(arg8,floor(s4));
    goto L75FAFF;
L75FA4C:
    if (s1c>=0) { arg8=floor(float(s4-s1c)); goto L75FAFF; }
L75FA56:
    arg8=floor(h[s18][0]); s18=prev(s18);
    if (arg8>floor(h[s18][0])) goto L75FA56;
    goto L75FAFF;
L75FAC0:
    arg8=floor(s1c<0 ? s4 : float(s4-s1c));
L75FAFF:
    if (s10!=floor(h[s20][1])) goto L75FCEB;
    if (s10==s24) goto L75FC63;
    ebx=s14>=0 ? floor(h[s20][0]) : floor(float(s8-s14));
L75FB83:
    b=s20; s20=next(b); s8=h[b][0]; ebx=std::max(ebx,floor(s8));
    if (s10==floor(h[s20][1])) goto L75FB83;
    argC=ebx; s14=slope(b,s20); s8=intercept(b,s14);
    if (s14>=0) argC=std::max(argC,floor(s8));
    goto L75F821;
L75FC63:
    if (s14<0) { argC=floor(float(s8-s14)); goto L75F821; }
L75FC70:
    argC=floor(h[s20][0]); s20=next(s20);
    if (argC<floor(h[s20][0])) goto L75FC70;
    goto L75F821;
L75FCEB:
    argC=floor(s14>=0 ? s8 : float(s8-s14));
    goto L75F821;
}
} // namespace water_scan_proof
