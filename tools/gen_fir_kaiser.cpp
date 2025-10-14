// Simple Kaiser-windowed lowpass FIR generator for Q31 output
// Designs a linear-phase even-length lowpass with unity DC gain.
// Parameters are fixed for this project: N=96, fs=192000 Hz, fc=15000 Hz, beta≈8.0.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static double bessel_i0(double x) {
    // Approximate I0 using a series expansion (sufficient for beta<=10 here)
    double sum = 1.0;
    double y = x * x / 4.0;
    double t = 1.0;
    for (int k = 1; k < 50; ++k) {
        t *= y / (k * k);
        sum += t;
        if (t < 1e-12) break;
    }
    return sum;
}

int main() {
    const int N = 96;               // taps (even)
    const double fs = 192000.0;     // Hz
    const double fc = 15000.0;      // Hz cutoff
    const double beta = 8.0;        // Kaiser beta (~80 dB attenuation)

    const int M = N - 1;            // order
    const double norm_cut = fc / fs; // normalized (cycles/sample)

    std::vector<double> h(N);

    // Ideal lowpass (sinc) centered at M/2
    for (int n = 0; n < N; ++n) {
        double m = n - M / 2.0;
        double x = 2.0 * M_PI * norm_cut * m;
        double sinc = (fabs(x) < 1e-12) ? 1.0 : sin(x) / x;
        h[n] = 2.0 * norm_cut * sinc; // 2*fc/fs * sinc(2*pi*fc/fs * m)
    }

    // Kaiser window
    double denom = bessel_i0(beta);
    for (int n = 0; n < N; ++n) {
        double r = (2.0 * n) / M - 1.0; // in [-1,1]
        double w = bessel_i0(beta * sqrt(1.0 - r * r)) / denom;
        h[n] *= w;
    }

    // Normalize to unity DC gain at the polyphase outputs.
    // First normalize prototype to sum=1, then scale by L so each phase sum ≈ 1.
    double sum = 0.0;
    for (double v : h) sum += v;
    for (double &v : h) v /= sum; // sum=1

    const int L = 4; // upsample factor
    for (double &v : h) v *= L;   // total sum = L

    // Convert to Q31 (int32), scaling by 2^31
    const double Q31 = 2147483648.0; // 2^31
    // Print as C-style initializer list, 8 per line
    std::puts("// Generated 96-tap Kaiser-windowed LPF (fc=15 kHz @ fs=192 kHz), Q31 format");
    std::puts("// Passband: 0-15 kHz, Transition: ~15-19 kHz, Stopband: >19 kHz");
    std::puts("{");
    for (int i = 0; i < N; ++i) {
        long long q = llround(h[i] * Q31);
        if (q >  2147483647LL) q =  2147483647LL;
        if (q < -2147483648LL) q = -2147483648LL;
        std::printf(" %lldL,%s", q, ((i+1)%8==0 || i==N-1) ? "\n" : "");
    }
    std::puts("}");
    return 0;
}
