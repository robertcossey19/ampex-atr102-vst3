#include "TapeDSP.h"
#include <cstdio>
using namespace ampex;
// Jiles-Atherton core as used in the browser worklet (Chowdhury DAFx-19 form), trapezoidal step in H.
struct JilesAtherton
{
    double Ms = 3.5e5, a = 22e3, alpha = 1.6e-3, k = 27e3, c = 1.7e-1, T = 1.0;
    double M = 0, Hp = 0;
    void reset() { M = 0; Hp = 0; }
    static double L (double x) { return std::abs (x) < 1e-4 ? x / 3.0 : 1.0 / std::tanh (x) - 1.0 / x; }
    static double Ld (double x) { return std::abs (x) < 1e-4 ? 1.0 / 3.0 : 1.0 - 1.0 / (std::tanh (x) * std::tanh (x)) + 1.0 / (x * x); }
    double dMdH (double H, double Mcur, double dH) const
    {
        const double Q = (H + alpha * Mcur) / a;
        const double Man = Ms * L (Q);
        const double delta = dH >= 0 ? 1.0 : -1.0;
        const double dM = Man - Mcur;
        const double deltaM = (dM * delta >= 0) ? 1.0 : 0.0;    // no irreversible change when moving toward Man
        const double num = deltaM * dM / (delta * k - alpha * dM) + c * Ms / a * Ld (Q);
        const double den = 1.0 + alpha * c * Ms / a * Ld (Q) - alpha * deltaM * dM / (delta * k - alpha * dM) * 0.0;
        return num / den;
    }
    double step (double H)
    {
        const double dH = H - Hp;
        const double k1 = dMdH (Hp, M, dH);
        const double k2 = dMdH (H, M + k1 * dH, dH);
        M += 0.5 * (k1 + k2) * dH;
        Hp = H;
        return M;
    }
};
// anhysteretic remanence via decaying (bias+signal) field through a JA core
double Mar(const JilesAtherton& proto, double Hs, double Hb, int cycles=24, int steps=24){
  JilesAtherton ja=proto; ja.reset(); ja.T=1.0; // T irrelevant: dMdt*T -> uses Hd=(Hn-H)/T, consistent
  const int n=cycles*steps; const double dph=2*kPi/steps;
  for(int i=0;i<n;i++){ double e = 1.0 - (double)(i+1)/n; e = e*e; /* field decay profile (quadratic-ish) */ double H = e*(Hs + Hb*std::sin(dph*i)); ja.step(H);} 
  return ja.M;
}
int main(int argc,char**argv){
  double aMul=argc>1?atof(argv[1]):0.815, alpha=argc>2?atof(argv[2]):1.6e-3, c=argc>3?atof(argv[3]):0.17, biasMul=argc>4?atof(argv[4]):4.0;
  const TapeSpec& ts=tapeTable()[1];
  JilesAtherton ja; ja.k=ts.Hc_Am; ja.a=aMul*ts.Hc_Am; ja.Ms=ts.Br_T/(kMu0*0.85); ja.c=c; ja.alpha=alpha;
  double Hb=biasMul*ts.Hc_Am;
  printf("Mar curve (Hs/Hc : M/Ms) Hb=%.0f\n",Hb);
  for(double r : {0.02,0.05,0.1,0.2,0.3,0.5,0.7,1.0,1.5,2.0,3.0,5.0}) printf("  %.2f : %.4f  (slope %.3f)\n", r, Mar(ja,r*ts.Hc_Am,Hb)/ja.Ms, Mar(ja,r*ts.Hc_Am,Hb)/ja.Ms/r);
  // THD vs level using table as static nonlinearity
  printf("THD3 vs Hs amplitude:\n");
  for(double r : {0.05,0.1,0.15,0.2,0.3,0.4,0.6,0.8,1.0}){
    const int N=256; double re1=0,im1=0,re3=0,im3=0; for(int i=0;i<N;i++){ double w=2*kPi*i/N; double y=Mar(ja,r*ts.Hc_Am*sin(w),Hb); re1+=y*cos(w); im1+=y*sin(w); re3+=y*cos(3*w); im3+=y*sin(3*w);} 
    printf("  Hs=%.2f Hc : THD3=%.3f%%  gain=%.4f\n", r, 100*hypot(re3,im3)/hypot(re1,im1), 2*hypot(re1,im1)/N/(r*ts.Hc_Am)*ja.a/ja.Ms*3);
  }
  printf("LF output vs bias (Hs=0.1Hc):\n");
  for(double b : {0.5,1.0,1.5,2.0,2.5,3.0,4.0,6.0,8.0}) printf("  Hb=%.1f Hc : M=%.4f\n", b, Mar(ja,0.1*ts.Hc_Am,b*ts.Hc_Am)/ja.Ms);
}
