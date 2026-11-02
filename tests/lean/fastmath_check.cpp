// Accuracy of the lean voice's polynomial stand-ins for libm (firmware/src/lean/LeanDsp.h) over the ranges the voice uses. Exit code 1 on a miss.
#include <math.h>
#include <stdio.h>

#include "lean/LeanDsp.h"

int main() {
    int bad = 0;
    auto check = [&](const char* name, double worst, double limit) {
        printf("%-12s worst %.3g (limit %.3g)%s\n", name, worst, limit, worst <= limit ? "" : "  <-- FAIL");
        if (worst > limit) ++bad;
    };
    double w = 0;
    for (double x = -60.0; x <= 60.0; x += 0.00037) { const double r = exp2((double)(float)x); const double e = fabs(lean::fastExp2((float)x) / r - 1.0); if (e > w) w = e; }
    check("exp2 rel", w, 3.0e-7);
    w = 0;
    for (double x = 1.0e-6; x < 1.0e6; x *= 1.0007) { const double r = log2((double)(float)x); const double e = fabs(lean::fastLog2((float)x) - r) - 1.2e-7 * fabs(r); if (e > w) w = e; }
    check("log2 abs", w, 2.0e-7);   // beyond the float rounding of the result itself
    w = 0;
    for (double p = -3.0; p <= 3.0; p += 0.00013) { const double e = fabs(lean::fastSinCycles((float)p) - sin(2.0 * M_PI * (double)(float)p)); if (e > w) w = e; }
    check("sin abs", w, 2.0e-6);
    w = 0;
    for (double u = 0.0005; u <= 0.45; u += 0.00001) { const double e = fabs(lean::fastTanPi((float)u) / tan(M_PI * (double)(float)u) - 1.0); if (e > w) w = e; }
    check("tan rel .45", w, 1.0e-6);
    w = 0;
    for (double u = 0.0005; u <= 0.49; u += 0.00001) { const double e = fabs(lean::fastTanPi((float)u) / tan(M_PI * (double)(float)u) - 1.0); if (e > w) w = e; }
    check("tan rel .49", w, 4.0e-6);
    return bad ? 1 : 0;
}
