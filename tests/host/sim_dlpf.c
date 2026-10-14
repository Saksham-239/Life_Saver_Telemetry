#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 2nd-order digital biquad low-pass filter */
typedef struct {
    double b0, b1, b2;
    double a1, a2;
    double x1, x2;
    double y1, y2;
} biquad_lpf_t;

static void biquad_init(biquad_lpf_t *f, double fc_hz, double fs_hz) {
    double omega = 2.0 * M_PI * fc_hz / fs_hz;
    double sn = sin(omega);
    double cs = cos(omega);
    double alpha = sn / (2.0 * 0.70710678); /* Q = 1/sqrt(2) Butterworth */

    double a0 = 1.0 + alpha;
    f->b0 = ((1.0 - cs) / 2.0) / a0;
    f->b1 = (1.0 - cs) / a0;
    f->b2 = ((1.0 - cs) / 2.0) / a0;
    f->a1 = (-2.0 * cs) / a0;
    f->a2 = (1.0 - alpha) / a0;

    f->x1 = f->x2 = 0.0;
    f->y1 = f->y2 = 0.0;
}

static double biquad_process(biquad_lpf_t *f, double in) {
    double out = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = in;
    f->y2 = f->y1;
    f->y1 = out;
    return out;
}

typedef struct {
    int cfg;
    const char *name;
    double fc_hz;
} dlpf_config_t;

static const dlpf_config_t s_dlpfs[] = {
    { 2, "CFG 2 (98 Hz)", 98.0 },
    { 3, "CFG 3 (44 Hz)", 44.0 },
    { 4, "CFG 4 (21 Hz)", 21.0 }
};

int main(void) {
    printf("========================================================================================\n");
    printf("  MPU-6050 DLPF & SAMPLE RATE IMPACT SIMULATION (Host Simulation)\n");
    printf("========================================================================================\n\n");

    const double durations_ms[] = { 10.0, 20.0, 30.0, 50.0 };
    const int decimations[]     = { 10, 5 }; /* 10 -> 100 Hz, 5 -> 200 Hz */
    const char *dec_names[]     = { "100 Hz (div 10, SMPLRT_DIV=9)", "200 Hz (div 5, SMPLRT_DIV=4)" };

    for (size_t d_idx = 0; d_idx < 2; d_idx++) {
        int dec = decimations[d_idx];
        printf("--- Target Output Sample Rate: %s ---\n", dec_names[d_idx]);
        printf("%-18s | %-9s | %-9s | %-9s | %-16s | %-16s | %-10s\n",
               "DLPF Setting", "Pulse Dur", "True Peak", "Filt Peak", "Worst Peak (%%)", "Mean Peak (%%)", "Best (%%)");
        printf("-------------------------------------------------------------------------------------------------------\n");

        for (size_t cfg_i = 0; cfg_i < sizeof(s_dlpfs)/sizeof(s_dlpfs[0]); cfg_i++) {
            dlpf_config_t dlpf = s_dlpfs[cfg_i];

            for (size_t t_i = 0; t_i < sizeof(durations_ms)/sizeof(durations_ms[0]); t_i++) {
                double T_ms = durations_ms[t_i];
                double A_g = 6.0; /* 6g impact */

                double max_true = 0.0;
                double max_filt = 0.0;

                double worst_dec = 1e9;
                double best_dec  = 0.0;
                double sum_dec   = 0.0;

                /* Sweep all decimation phases: phase = 0 .. dec - 1 */
                for (int phase = 0; phase < dec; phase++) {
                    int n_samples = 500;
                    biquad_lpf_t filter;
                    biquad_init(&filter, dlpf.fc_hz, 1000.0);

                    int pulse_start_idx = 100 + phase;
                    int pulse_len_idx = (int)T_ms;

                    double phase_max_dec = 0.0;

                    for (int n = 0; n < n_samples; n++) {
                        double true_val = 1.0; /* 1g baseline */
                        if (n >= pulse_start_idx && n < pulse_start_idx + pulse_len_idx) {
                            double t_rel = (double)(n - pulse_start_idx);
                            true_val += A_g * sin(M_PI * t_rel / (double)pulse_len_idx);
                        }
                        if (true_val > max_true) max_true = true_val;

                        double filt_val = biquad_process(&filter, true_val);
                        if (n > 50 && filt_val > max_filt) max_filt = filt_val;

                        /* Decimation sampling instant (aligned to n % dec == 0) */
                        if (n % dec == 0) {
                            if (n > 50 && filt_val > phase_max_dec) phase_max_dec = filt_val;
                        }
                    }

                    if (phase_max_dec < worst_dec) worst_dec = phase_max_dec;
                    if (phase_max_dec > best_dec)  best_dec  = phase_max_dec;
                    sum_dec += phase_max_dec;
                }

                double mean_dec = sum_dec / (double)dec;

                /* Relative peak preservation above 1g baseline */
                double true_impact  = max_true - 1.0;
                double worst_impact = worst_dec - 1.0;
                double mean_impact  = mean_dec - 1.0;
                double best_impact  = best_dec - 1.0;

                double worst_pct = (worst_impact / true_impact) * 100.0;
                double mean_pct  = (mean_impact / true_impact) * 100.0;
                double best_pct  = (best_impact / true_impact) * 100.0;

                printf("%-18s | %5.1f ms   | %5.2f g   | %5.2f g   | %5.2f g (%5.1f%%) | %5.2f g (%5.1f%%) | %5.1f%%\n",
                       dlpf.name, T_ms, max_true, max_filt,
                       worst_dec, worst_pct, mean_dec, mean_pct, best_pct);
            }
            printf("-------------------------------------------------------------------------------------------------------\n");
        }
        printf("\n");
    }

    /* Simulation of 60 Hz chassis vibration rejection */
    printf("--- High-Frequency Vibration Rejection Simulation (60 Hz Chassis Noise at 1 kHz) ---\n");
    printf("%-18s | %-16s | %-16s | %-14s\n", "DLPF Setting", "Noise In (60 Hz)", "Noise Out", "Attenuation %%");
    printf("----------------------------------------------------------------------------------------\n");
    for (size_t cfg_i = 0; cfg_i < sizeof(s_dlpfs)/sizeof(s_dlpfs[0]); cfg_i++) {
        dlpf_config_t dlpf = s_dlpfs[cfg_i];
        biquad_lpf_t filter;
        biquad_init(&filter, dlpf.fc_hz, 1000.0);

        double max_noise_out = 0.0;
        for (int n = 0; n < 300; n++) {
            double noise_in = 1.0 + 1.0 * sin(2.0 * M_PI * 60.0 * (double)n / 1000.0);
            double noise_out = biquad_process(&filter, noise_in);
            if (n > 50) {
                double amp = fabs(noise_out - 1.0);
                if (amp > max_noise_out) max_noise_out = amp;
            }
        }
        double atten_pct = (1.0 - max_noise_out) * 100.0;
        printf("%-18s | %10.2f g      | %10.2f g      | %6.2f %%\n",
               dlpf.name, 1.00, max_noise_out, atten_pct);
    }
    printf("========================================================================================\n\n");

    printf("--- Engineering Decision & Model Limitations Document ---\n");
    printf("1. ODR / DLPF SELECTION:\n");
    printf("   - At 100 Hz (SMPLRT_DIV=9), a 20 ms / 6g impact under CFG 3 keeps only 71.2%% (worst-case)\n");
    printf("     because decimation aliasing / phase offset misses the narrow filtered crest.\n");
    printf("   - At 200 Hz (SMPLRT_DIV=4), CFG 3 keeps 91.3%% (worst-case) and 91.7%% (mean).\n");
    printf("   - Vibration Rejection: CFG 3 attenuates 60 Hz mechanical noise by 53.1%% (CFG 2 only 5.9%%).\n");
    printf("   -> FINAL SELECTION: DLPF CFG 3 (44 Hz) @ 200 Hz ODR (SMPLRT_DIV=4, 5 ms RTOS period).\n\n");

    printf("2. MODEL LIMITATIONS & HARDWARE BENCH GAPS:\n");
    printf("   - Bilinear Transform / 2nd-order Butterworth assumption: InvenSense MPU-6050 internal\n");
    printf("     switched-capacitor low-pass filter has proprietary phase and group-delay curves\n");
    printf("     that may deviate slightly from ideal 2nd-order Butterworth responses.\n");
    printf("   - Half-Sine Pulse Assumption: Real human falls and impacts on concrete/ground exhibit\n");
    printf("     non-linear deceleration waveforms with high-frequency micro-spikes, bounce ringing,\n");
    printf("     and clothing/tissue damping.\n");
    printf("   - Physical impact bench verification with an oscilloscope and calibrated shaker/drop-rig\n");
    printf("     remains mandatory for target hardware validation.\n");
    printf("========================================================================================\n");
    return 0;
}
