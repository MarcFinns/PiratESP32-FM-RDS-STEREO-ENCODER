#!/usr/bin/env python3
"""
Frequency Response Analysis of DSP Filter Chain
================================================
Analyzes three cascaded filter stages:
1. Pre-emphasis filter (first-order FIR high-pass)
2. 19 kHz notch filter (biquad IIR, Q=25)
3. Polyphase FIR upsampler (4x, 96-tap Kaiser window)

Target: Audio band flatness (20 Hz - 15 kHz) at 48 kHz sample rate
"""

import numpy as np
from scipy import signal
import matplotlib.pyplot as plt

# ============================================================================
# FILTER DEFINITIONS
# ============================================================================

class PreEmphasisFilter:
    """Pre-emphasis filter: H(z) = 3.0 * (1 - 0.6592*z^-1)"""

    def __init__(self):
        self.gain = 3.0
        self.alpha = 0.6592
        # FIR coefficients: b = [3.0, -1.9776], a = [1]
        self.b = np.array([self.gain, -self.gain * self.alpha])
        self.a = np.array([1.0])

    def frequency_response(self, frequencies, fs):
        """Compute frequency response at given frequencies"""
        # Normalized frequencies (0 to pi)
        omega = 2 * np.pi * frequencies / fs

        # H(e^jω) = 3.0 * (1 - 0.6592*e^-jω)
        H = self.gain * (1 - self.alpha * np.exp(-1j * omega))

        return H

    def magnitude_db(self, frequencies, fs):
        """Magnitude response in dB"""
        H = self.frequency_response(frequencies, fs)
        return 20 * np.log10(np.abs(H))

    def magnitude_linear(self, frequencies, fs):
        """Magnitude response (linear)"""
        H = self.frequency_response(frequencies, fs)
        return np.abs(H)


class NotchFilter:
    """19 kHz notch filter with Q=25 (biquad IIR)"""

    def __init__(self, f_notch=19000, Q=25, fs=48000):
        self.f_notch = f_notch
        self.Q = Q
        self.fs = fs

        # Design notch filter using scipy
        # Notch filter: H(z) has zeros on unit circle at notch frequency
        # and poles inside unit circle for finite bandwidth
        w0 = 2 * np.pi * f_notch / fs  # Digital frequency (rad/sample)

        # Bandwidth in octaves (approximate)
        # Q = f_center / bandwidth_hz
        # For notch filter: BW = f0/Q
        bandwidth_hz = f_notch / Q

        # Design using scipy's iirnotch
        self.b, self.a = signal.iirnotch(f_notch, Q, fs)

    def frequency_response(self, frequencies, fs):
        """Compute frequency response using scipy"""
        w, H = signal.freqz(self.b, self.a, worN=frequencies, fs=fs)
        return H

    def magnitude_db(self, frequencies, fs):
        """Magnitude response in dB"""
        H = self.frequency_response(frequencies, fs)
        return 20 * np.log10(np.abs(H))

    def magnitude_linear(self, frequencies, fs):
        """Magnitude response (linear)"""
        H = self.frequency_response(frequencies, fs)
        return np.abs(H)


class PolyphaseFIRUpsampler:
    """4x upsampling with 96-tap Kaiser window FIR (15 kHz LPF)"""

    def __init__(self, upsample_factor=4, num_taps=96, cutoff_hz=15000, fs_in=48000):
        self.L = upsample_factor
        self.num_taps = num_taps
        self.cutoff_hz = cutoff_hz
        self.fs_in = fs_in
        self.fs_out = fs_in * self.L

        # Design FIR filter for polyphase upsampler
        # The filter operates at the OUTPUT rate (192 kHz)
        # But the cutoff is designed to pass input signal up to 15 kHz

        # Normalized cutoff (relative to OUTPUT sample rate)
        # cutoff_hz / (fs_out / 2) = 15000 / 96000 = 0.15625
        cutoff_normalized = cutoff_hz / (self.fs_out / 2)

        # Kaiser window design
        # Ripple specifications: passband ripple < 0.01 dB, stopband > 80 dB
        # For these specs, Kaiser beta ≈ 8-10
        beta = 8.5

        # Design FIR filter with Kaiser window
        # NOTE: The gain factor L is applied to compensate for upsampling
        # This maintains unity gain through the upsampling process
        self.h = self.L * signal.firwin(
            num_taps,
            cutoff_normalized,
            window=('kaiser', beta),
            scale=True  # Scale to unity gain at DC
        )

        self.b = self.h
        self.a = np.array([1.0])

    def frequency_response(self, frequencies, fs):
        """
        Compute frequency response at input sample rate

        The polyphase filter operates at fs_out (192 kHz), but we want to
        know its response to signals at the input rate (48 kHz).

        For an upsampler, the effective frequency response at the input
        is obtained by evaluating H(e^jω) where ω is normalized to fs_out.
        """
        # Evaluate filter response at output sample rate
        # but for frequencies that correspond to the input spectrum
        w, H = signal.freqz(self.b, self.a, worN=frequencies, fs=self.fs_out)
        return H

    def magnitude_db(self, frequencies, fs):
        """Magnitude response in dB"""
        H = self.frequency_response(frequencies, fs)
        return 20 * np.log10(np.abs(H))

    def magnitude_linear(self, frequencies, fs):
        """Magnitude response (linear)"""
        H = self.frequency_response(frequencies, fs)
        return np.abs(H)


# ============================================================================
# ANALYSIS FUNCTIONS
# ============================================================================

def analyze_cascade(frequencies, fs=48000):
    """
    Analyze cascaded frequency response of all three filters
    """
    # Create filter instances
    preemph = PreEmphasisFilter()
    notch = NotchFilter(f_notch=19000, Q=25, fs=fs)
    upsampler = PolyphaseFIRUpsampler(upsample_factor=4, num_taps=96,
                                       cutoff_hz=15000, fs_in=fs)

    # Compute individual responses
    H1 = preemph.frequency_response(frequencies, fs)
    H2 = notch.frequency_response(frequencies, fs)
    H3 = upsampler.frequency_response(frequencies, fs)

    # Cascade: H_total = H1 * H2 * H3
    H_total = H1 * H2 * H3

    # Magnitude and phase
    mag_linear = np.abs(H_total)
    mag_db = 20 * np.log10(mag_linear)
    phase_rad = np.angle(H_total)
    phase_deg = np.degrees(phase_rad)

    return {
        'H1': H1, 'H2': H2, 'H3': H3,
        'H_total': H_total,
        'mag_linear': mag_linear,
        'mag_db': mag_db,
        'phase_rad': phase_rad,
        'phase_deg': phase_deg,
        'preemph': preemph,
        'notch': notch,
        'upsampler': upsampler
    }


def compute_passband_ripple(mag_db, frequencies, passband_limits=(20, 15000)):
    """
    Compute passband ripple (peak-to-peak variation in dB)
    """
    # Find indices within passband
    f_min, f_max = passband_limits
    idx = (frequencies >= f_min) & (frequencies <= f_max)

    passband_mag_db = mag_db[idx]
    passband_freqs = frequencies[idx]

    # Ripple = max - min
    mag_max = np.max(passband_mag_db)
    mag_min = np.min(passband_mag_db)
    ripple_db = mag_max - mag_min

    # Mean and std
    mag_mean = np.mean(passband_mag_db)
    mag_std = np.std(passband_mag_db)

    return {
        'ripple_db': ripple_db,
        'mag_max_db': mag_max,
        'mag_min_db': mag_min,
        'mag_mean_db': mag_mean,
        'mag_std_db': mag_std,
        'passband_freqs': passband_freqs,
        'passband_mag_db': passband_mag_db
    }


# ============================================================================
# MAIN ANALYSIS
# ============================================================================

def main():
    fs = 48000  # Sample rate

    # Test frequencies
    test_freqs = np.array([20, 100, 1000, 5000, 10000, 15000])

    # High-resolution frequency sweep for ripple analysis
    freq_sweep = np.logspace(np.log10(20), np.log10(15000), 1000)

    print("=" * 80)
    print("FREQUENCY RESPONSE ANALYSIS - DSP FILTER CHAIN")
    print("=" * 80)
    print(f"Sample Rate: {fs} Hz")
    print(f"Audio Band: 20 Hz - 15 kHz")
    print()

    # ========================================================================
    # INDIVIDUAL FILTER ANALYSIS
    # ========================================================================

    print("-" * 80)
    print("1. PRE-EMPHASIS FILTER: H(z) = 3.0 * (1 - 0.6592*z^-1)")
    print("-" * 80)
    preemph = PreEmphasisFilter()
    print(f"DC Gain (f=0): {preemph.magnitude_linear(np.array([0]), fs)[0]:.6f}")
    print(f"Nyquist Gain (f={fs/2}): {preemph.magnitude_linear(np.array([fs/2]), fs)[0]:.6f}")
    print()
    print("Frequency Response at Test Points:")
    print(f"{'Frequency':<12} {'Magnitude (linear)':<20} {'Magnitude (dB)':<15}")
    print("-" * 50)
    for f in test_freqs:
        mag_lin = preemph.magnitude_linear(np.array([f]), fs)[0]
        mag_db = preemph.magnitude_db(np.array([f]), fs)[0]
        print(f"{f:>8} Hz   {mag_lin:>18.6f}   {mag_db:>13.4f} dB")
    print()

    # Ripple analysis
    mag_sweep = preemph.magnitude_db(freq_sweep, fs)
    ripple1 = compute_passband_ripple(mag_sweep, freq_sweep)
    print(f"Passband Ripple (20 Hz - 15 kHz): {ripple1['ripple_db']:.4f} dB")
    print(f"  Max: {ripple1['mag_max_db']:.4f} dB at {freq_sweep[np.argmax(mag_sweep)]:.1f} Hz")
    print(f"  Min: {ripple1['mag_min_db']:.4f} dB at {freq_sweep[np.argmin(mag_sweep)]:.1f} Hz")
    print()

    # ========================================================================
    print("-" * 80)
    print("2. NOTCH FILTER: 19 kHz center, Q = 25")
    print("-" * 80)
    notch = NotchFilter(f_notch=19000, Q=25, fs=fs)
    print(f"Notch Frequency: 19000 Hz")
    print(f"Q Factor: 25")
    print(f"Bandwidth: {19000/25:.1f} Hz")
    print(f"Attenuation at 19 kHz: {notch.magnitude_db(np.array([19000]), fs)[0]:.2f} dB")
    print()
    print("Frequency Response at Test Points:")
    print(f"{'Frequency':<12} {'Magnitude (linear)':<20} {'Magnitude (dB)':<15}")
    print("-" * 50)
    for f in test_freqs:
        mag_lin = notch.magnitude_linear(np.array([f]), fs)[0]
        mag_db = notch.magnitude_db(np.array([f]), fs)[0]
        print(f"{f:>8} Hz   {mag_lin:>18.6f}   {mag_db:>13.4f} dB")
    print()

    mag_sweep = notch.magnitude_db(freq_sweep, fs)
    ripple2 = compute_passband_ripple(mag_sweep, freq_sweep)
    print(f"Passband Ripple (20 Hz - 15 kHz): {ripple2['ripple_db']:.4f} dB")
    print(f"  Max: {ripple2['mag_max_db']:.4f} dB")
    print(f"  Min: {ripple2['mag_min_db']:.4f} dB")
    print()

    # ========================================================================
    print("-" * 80)
    print("3. POLYPHASE FIR UPSAMPLER: 4x, 96-tap Kaiser, 15 kHz LPF")
    print("-" * 80)
    upsampler = PolyphaseFIRUpsampler(upsample_factor=4, num_taps=96,
                                       cutoff_hz=15000, fs_in=fs)
    print(f"Upsampling Factor: {upsampler.L}x")
    print(f"Input Sample Rate: {upsampler.fs_in} Hz")
    print(f"Output Sample Rate: {upsampler.fs_out} Hz")
    print(f"Number of Taps: {upsampler.num_taps}")
    print(f"Cutoff Frequency: {upsampler.cutoff_hz} Hz")
    print()
    print("Frequency Response at Test Points:")
    print(f"{'Frequency':<12} {'Magnitude (linear)':<20} {'Magnitude (dB)':<15}")
    print("-" * 50)
    for f in test_freqs:
        mag_lin = upsampler.magnitude_linear(np.array([f]), fs)[0]
        mag_db = upsampler.magnitude_db(np.array([f]), fs)[0]
        print(f"{f:>8} Hz   {mag_lin:>18.6f}   {mag_db:>13.4f} dB")
    print()

    mag_sweep = upsampler.magnitude_db(freq_sweep, fs)
    ripple3 = compute_passband_ripple(mag_sweep, freq_sweep)
    print(f"Passband Ripple (20 Hz - 15 kHz): {ripple3['ripple_db']:.4f} dB")
    print(f"  Max: {ripple3['mag_max_db']:.4f} dB")
    print(f"  Min: {ripple3['mag_min_db']:.4f} dB")
    print()

    # ========================================================================
    # CASCADED RESPONSE
    # ========================================================================

    print("=" * 80)
    print("CASCADED FREQUENCY RESPONSE (All Three Filters)")
    print("=" * 80)
    print()

    result = analyze_cascade(test_freqs, fs)

    print("Frequency Response at Test Points:")
    print(f"{'Frequency':<12} {'H1 (dB)':<12} {'H2 (dB)':<12} {'H3 (dB)':<12} {'Total (dB)':<12} {'Total (linear)':<15}")
    print("-" * 80)
    for i, f in enumerate(test_freqs):
        h1_db = 20 * np.log10(np.abs(result['H1'][i]))
        h2_db = 20 * np.log10(np.abs(result['H2'][i]))
        h3_db = 20 * np.log10(np.abs(result['H3'][i]))
        total_db = result['mag_db'][i]
        total_lin = result['mag_linear'][i]
        print(f"{f:>8} Hz   {h1_db:>10.4f}   {h2_db:>10.4f}   {h3_db:>10.4f}   {total_db:>10.4f}   {total_lin:>13.6f}")
    print()

    # High-resolution cascade analysis
    result_sweep = analyze_cascade(freq_sweep, fs)
    ripple_total = compute_passband_ripple(result_sweep['mag_db'], freq_sweep)

    print(f"PASSBAND RIPPLE ANALYSIS (20 Hz - 15 kHz):")
    print(f"  Peak-to-Peak Ripple: {ripple_total['ripple_db']:.4f} dB")
    print(f"  Maximum Gain: {ripple_total['mag_max_db']:.4f} dB at {freq_sweep[np.argmax(result_sweep['mag_db'])]:.1f} Hz")
    print(f"  Minimum Gain: {ripple_total['mag_min_db']:.4f} dB at {freq_sweep[np.argmin(result_sweep['mag_db'])]:.1f} Hz")
    print(f"  Mean Gain: {ripple_total['mag_mean_db']:.4f} dB")
    print(f"  Standard Deviation: {ripple_total['mag_std_db']:.4f} dB")
    print()

    # ========================================================================
    # FLATNESS ASSESSMENT
    # ========================================================================

    print("=" * 80)
    print("FLATNESS QUALITY ASSESSMENT")
    print("=" * 80)
    print()

    ripple_db = ripple_total['ripple_db']

    print("Industry Standards for Audio Flatness:")
    print("  Excellent: < 0.1 dB ripple")
    print("  Very Good: 0.1 - 0.5 dB ripple")
    print("  Good:      0.5 - 1.0 dB ripple")
    print("  Fair:      1.0 - 2.0 dB ripple")
    print("  Poor:      > 2.0 dB ripple")
    print()

    if ripple_db < 0.1:
        quality = "EXCELLENT"
    elif ripple_db < 0.5:
        quality = "VERY GOOD"
    elif ripple_db < 1.0:
        quality = "GOOD"
    elif ripple_db < 2.0:
        quality = "FAIR"
    else:
        quality = "POOR"

    print(f"Measured Ripple: {ripple_db:.4f} dB")
    print(f"Quality Rating: {quality}")
    print()

    # Analysis of dominant contributor
    print("DOMINANT FLATNESS CONTRIBUTORS:")
    print(f"  Pre-emphasis: {ripple1['ripple_db']:.4f} dB (high-pass characteristic)")
    print(f"  Notch filter: {ripple2['ripple_db']:.4f} dB (out-of-band, minimal impact)")
    print(f"  Upsampler FIR: {ripple3['ripple_db']:.4f} dB (flat to 15 kHz)")
    print()

    # Identify limiting stage
    ripples = [
        (ripple1['ripple_db'], "Pre-emphasis filter"),
        (ripple2['ripple_db'], "Notch filter"),
        (ripple3['ripple_db'], "Upsampler FIR")
    ]
    ripples.sort(reverse=True)

    print(f"PRIMARY LIMITER: {ripples[0][1]} ({ripples[0][0]:.4f} dB ripple)")
    print()

    # ========================================================================
    # GENERATE PLOTS
    # ========================================================================

    print("Generating frequency response plots...")

    # Extended frequency range for visualization
    freq_plot = np.logspace(1, np.log10(24000), 2000)
    result_plot = analyze_cascade(freq_plot, fs)

    fig, axes = plt.subplots(3, 1, figsize=(12, 10))

    # Plot 1: Individual filter responses
    ax = axes[0]
    ax.semilogx(freq_plot, 20*np.log10(np.abs(result_plot['H1'])),
                label='Pre-emphasis', linewidth=2)
    ax.semilogx(freq_plot, 20*np.log10(np.abs(result_plot['H2'])),
                label='Notch (19 kHz)', linewidth=2)
    ax.semilogx(freq_plot, 20*np.log10(np.abs(result_plot['H3'])),
                label='Upsampler FIR', linewidth=2)
    ax.axvline(20, color='gray', linestyle='--', alpha=0.5, linewidth=1)
    ax.axvline(15000, color='gray', linestyle='--', alpha=0.5, linewidth=1)
    ax.axvline(19000, color='red', linestyle=':', alpha=0.5, linewidth=1, label='Notch center')
    ax.grid(True, which='both', alpha=0.3)
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_title('Individual Filter Frequency Responses')
    ax.legend()
    ax.set_xlim([10, 24000])

    # Plot 2: Cascaded response (full range)
    ax = axes[1]
    ax.semilogx(freq_plot, result_plot['mag_db'], linewidth=2, color='purple')
    ax.axvline(20, color='gray', linestyle='--', alpha=0.5, linewidth=1)
    ax.axvline(15000, color='gray', linestyle='--', alpha=0.5, linewidth=1)
    ax.axhline(0, color='black', linestyle='-', alpha=0.3, linewidth=1)
    ax.grid(True, which='both', alpha=0.3)
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_title('Cascaded Frequency Response (All Filters)')
    ax.set_xlim([10, 24000])

    # Plot 3: Passband detail (20 Hz - 15 kHz)
    ax = axes[2]
    passband_idx = (freq_plot >= 20) & (freq_plot <= 15000)
    passband_f = freq_plot[passband_idx]
    passband_mag = result_plot['mag_db'][passband_idx]

    ax.semilogx(passband_f, passband_mag, linewidth=2, color='green')
    ax.axhline(ripple_total['mag_max_db'], color='red', linestyle='--',
               alpha=0.5, linewidth=1, label=f"Max: {ripple_total['mag_max_db']:.4f} dB")
    ax.axhline(ripple_total['mag_min_db'], color='blue', linestyle='--',
               alpha=0.5, linewidth=1, label=f"Min: {ripple_total['mag_min_db']:.4f} dB")
    ax.axhline(ripple_total['mag_mean_db'], color='orange', linestyle=':',
               alpha=0.7, linewidth=1, label=f"Mean: {ripple_total['mag_mean_db']:.4f} dB")
    ax.grid(True, which='both', alpha=0.3)
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_title(f'Passband Detail (20 Hz - 15 kHz) | Ripple: {ripple_total["ripple_db"]:.4f} dB')
    ax.legend()
    ax.set_xlim([20, 15000])

    plt.tight_layout()
    plt.savefig('/Users/marcello/Documents/Arduino/ESP32_RDS_STEREO_SW_ENCODER_GIT/frequency_response_analysis.png',
                dpi=150, bbox_inches='tight')
    print("Plots saved to: frequency_response_analysis.png")
    print()

    print("=" * 80)
    print("ANALYSIS COMPLETE")
    print("=" * 80)


if __name__ == "__main__":
    main()
