// Accuracy of nemesis/dsp/FastMath.h against libm. Built with NEMESIS_DSP_FLOAT + NEMESIS_DSP_FAST_MATH.
// The reference is evaluated at the SAME float-rounded input the fast function receives, so input quantisation (which
// no float implementation can avoid) is not counted as error. A point passes when |fast - ref| <= tolAbs + tolRel*|ref|.
#include <nemesis/dsp/FastMath.h>

#include <cmath>
#include <cstdio>

using namespace nemesis::dsp;

template <class F, class G>
static bool sweep(const char* name, double lo, double hi, int n, F fast, G ref, double tolRel, double tolAbs) {
    double worstUse = 0, worstRel = 0, worstAbs = 0, atUse = 0;
    for (int i = 0; i <= n; ++i) {
        const float xf = (float)(lo + (hi - lo) * i / n);
        const double r = ref((double)xf);
        const double f = fast(xf);
        const double err = std::fabs(f - r);
        const double use = err / (tolAbs + tolRel * std::fabs(r));
        if (use > worstUse) { worstUse = use; atUse = xf; }
        worstAbs = std::fmax(worstAbs, err);
        if (std::fabs(r) > 1e-3) worstRel = std::fmax(worstRel, err / std::fabs(r));
    }
    const bool ok = worstUse <= 1.0;
    std::printf("%-6s [%g, %g]  max abs %.2e  max rel (|ref|>1e-3) %.2e  tolerance used %.0f%% at x=%g  %s\n", name, lo,
                hi, worstAbs, worstRel, worstUse * 100, atUse, ok ? "ok" : "FAIL");
    return ok;
}

int main() {
    bool ok = true;
    ok &= sweep("exp2", -20, 20, 400000, [](float x) { return fm::exp2(x); }, [](double x) { return std::exp2(x); }, 2e-7, 0);
    ok &= sweep("exp2", -126, 126, 400000, [](float x) { return fm::exp2(x); }, [](double x) { return std::exp2(x); }, 2e-7, 0);
    ok &= sweep("log2", 1e-6, 4, 400000, [](float x) { return fm::log2(x); }, [](double x) { return std::log2(x); }, 1.5e-7, 3e-7);
    ok &= sweep("log2", 1, 1e6, 400000, [](float x) { return fm::log2(x); }, [](double x) { return std::log2(x); }, 2e-7, 0);
    ok &= sweep("tan", 0.0, 1.5398, 400000, [](float x) { return fm::tan(x); }, [](double x) { return std::tan(x); }, 4e-7, 0);   // up to 0.49*pi
    ok &= sweep("log1p", 0, 255, 400000, [](float x) { return fm::log1p(x); }, [](double x) { return std::log1p(x); }, 2e-7, 5e-7);
    ok &= sweep("expm1", 0, 5.55, 400000, [](float x) { return fm::expm1(x); }, [](double x) { return std::expm1(x); }, 1e-6, 2e-7);
    return ok ? 0 : 1;
}
