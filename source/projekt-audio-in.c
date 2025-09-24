#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "MCXA153.h"
#include "fsl_debug_console.h"
#include "oled.h"
#include "arm_math.h"

#define NFFT 256
#define N2FFT (2 * NFFT)
#define SAMPLE_FREQ (143.0f * 256.0f)   // ADC sampling frequency in Hz (adjust as needed)
#define TOLERANCE_PERCENT 10    // ±10% tolerance for string detection

// Standard violin string frequencies (Hz)
const float stringFreqs[4] = {196.00f, 293.66f, 440.00f, 659.25f};
const char *stringNames[4] = {"G", "D", "A", "E"};

// ADC result buffer
lpadc_conv_result_t g_LpadcResultConfigStruct;

// Data and FFT buffers
q15_t dataBuffer[NFFT] = {0};         // Raw ADC samples converted to q15
q15_t x[N2FFT]       = {0};           // Complex input for FFT (interleaved real, imag)
q15_t win[NFFT]      = {0};           // Window coefficients

q15_t *wrBuff = dataBuffer;
volatile bool dataReady = false;

// FFT instance
arm_cfft_instance_q15 cfft_inst;

void drawBars(q15_t *data, uint8_t w, uint8_t h) {
    uint16_t h2 = h - 1;
    uint8_t binsToDraw = NFFT / 2 - 2; // Skip x[0] and x[1]
    float scale = (float)w / binsToDraw;

    OLED_Clear_Screen(0);  // Optional: clear display before drawing bars

    for (int i = 2; i < NFFT / 2; i++) {
        uint8_t xPos = (uint8_t)((i - 2) * scale);
        if (xPos >= w) continue;

        uint16_t barHeight = (data[i] * h2) >> 15;
        if (barHeight > h2) barHeight = h2;

        OLED_Draw_Line(xPos, h2, xPos, h2 - barHeight);
    }
}

void cmplx_mag_q15(q15_t *src, q15_t *dst, uint32_t len) {
    q15_t re, im;
    for (uint32_t i = 0; i < len; i++) {
        re = *src++;
        im = *src++;
        q31_t sumsq = (q31_t)re * re + (q31_t)im * im;
        arm_sqrt_q15(sumsq >> 15, dst);
        *dst++ <<= 2;
    }
}

void ADC0_IRQHANDLER(void) {
    LPADC_GetConvResult(ADC0_PERIPHERAL, &g_LpadcResultConfigStruct);
    if (!dataReady) {
        *wrBuff++ = g_LpadcResultConfigStruct.convValue - Q15_MIN;
    }
    if (wrBuff >= &dataBuffer[NFFT]) {
        wrBuff = dataBuffer;
        dataReady = true;
    }
}

int main(void) {
    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
    BOARD_InitDebugConsole();
#endif
    OLED_Init(LPI2C0_PERIPHERAL);

    arm_cfft_init_q15(&cfft_inst, NFFT);

    for (int i = 0; i < NFFT; i++) {
        win[i] = ((q31_t)Q15_MAX - arm_cos_q15((Q15_MAX * i) / (NFFT - 1))) / 2;
    }

    OLED_Clear_Screen(0);
    OLED_Refresh_Gram();

    while (1) {
        if (dataReady) {
            // Build complex buffer
            q15_t *px = x;
            for (int i = 0; i < NFFT; i++) {
                *px++ = dataBuffer[i];
                *px++ = 0;
            }
            dataReady = false;

            // Apply window
            for (int i = 0; i < NFFT; i++) {
                x[2*i] = (((q31_t)x[2*i] * win[i]) >> 15);
            }

            // Run FFT
            arm_cfft_q15(&cfft_inst, x, 0, 1);
            cmplx_mag_q15(x, x, NFFT/2);

            // Find peak
            uint32_t peakIndex = 1;
            q15_t peakVal = x[1];
            for (uint32_t i = 2; i < NFFT/2; i++) {
                if (x[i] > peakVal) {
                    peakVal = x[i];
                    peakIndex = i;
                }
            }
            int i = peakIndex;
            float delta = 0.0f;

            if (i > 0 && i < (NFFT/2 - 1)) {
                float xm1 = (float)x[i - 1];
                float x0  = (float)x[i];
                float xp1 = (float)x[i + 1];
                float denom = xm1 - 2 * x0 + xp1;
                if (denom != 0.0f) {
                    delta = 0.5f * (xm1 - xp1) / denom;
                }
            }

            float interpolatedIndex = (float)peakIndex + delta;
            float peakFreq = interpolatedIndex * (SAMPLE_FREQ / NFFT);
            int32_t dispFreq = (int32_t)(peakFreq + 0.5f);

            // Determine string match
            const char *matched = "--";
            for (int s = 0; s < 4; s++) {
                float f0 = stringFreqs[s];
                float tol = f0 * TOLERANCE_PERCENT / 100.0f;
                if (peakFreq >= f0 - tol && peakFreq <= f0 + tol) {
                    matched = (char *)stringNames[s];
                    break;
                }
            }

            // Display
            OLED_Clear_Screen(0);
            drawBars(x, 128, 48);
            OLED_7seg(0, 0, dispFreq, 4, 1);
            OLED_Puts(100, 25, (char *)matched);
            OLED_Refresh_Gram();
        }
    }
    return 0;
}
