//RP2350-Zeroに合わせてCLK_PINを変更_2026.03.26_
//r1基板に合わせてST_PINとCLK_PINを交換_2026.04.15

#include <Arduino.h>
#include "hardware/pio.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "pico/time.h"

#define ST_PIN       14 //for MS-Zero_r1
#define CLK_PIN      15 //for MS-Zero_r1
#define ADC_PIN      26

#define DUMMY_SKIP   87   
#define PIXELS       288  
#define TOTAL_READ   380  
#define ACCUMULATION 10

// --- 整数倍オーバーサンプリングの定義 ---
// 288点の間(287箇所)に3点ずつ入れる -> 288 + (287 * 3) = 1149点
#define OUTPUT_POINTS 1149 

uint32_t accumulatedData[PIXELS];
uint16_t finalOutput[OUTPUT_POINTS];
int exposureMs = 500; 

// 3次補間 (Catmull-Rom)
float cubicInterpolate(float p0, float p1, float p2, float p3, float t) {
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
}

void setup_clk_generator() {
    uint16_t inst[] = { pio_encode_set(pio_pins, 1), pio_encode_set(pio_pins, 0) };
    struct pio_program prog = { inst, 2, -1 };
    uint offset = pio_add_program(pio0, &prog);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + 1);
    sm_config_set_set_pins(&c, CLK_PIN, 1);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (200000 * 2));
    pio_gpio_init(pio0, CLK_PIN);
    pio_sm_set_consecutive_pindirs(pio0, 0, CLK_PIN, 1, true);
    pio_sm_init(pio0, 0, offset, &c);
    pio_sm_set_enabled(pio0, 0, true); 
}

void setup() {
    Serial.begin(230400);
    adc_init(); adc_gpio_init(ADC_PIN); adc_select_input(0);
    pinMode(ST_PIN, OUTPUT); digitalWrite(ST_PIN, LOW);
    setup_clk_generator(); 
}

#define IS_CLK_HIGH (gpio_get(CLK_PIN))

void performMeasurement() {
    for(int i=0; i<PIXELS; i++) accumulatedData[i] = 0;

    uint32_t targetSubExpUs = (exposureMs * 1000) / ACCUMULATION;
    int32_t stHighUs = (int32_t)targetSubExpUs - 240;
    if (stHighUs < 10) stHighUs = 10; 

    for(int n=0; n<ACCUMULATION; n++) {
        uint64_t start_st = time_us_64();
        gpio_put(ST_PIN, 1);
        while ((time_us_64() - start_st) < (uint64_t)stHighUs);
        gpio_put(ST_PIN, 0);

        for (int i = 0; i < TOTAL_READ; i++) {
            while(!IS_CLK_HIGH); 
            uint16_t val = adc_read();
            int pixelIdx = i - DUMMY_SKIP;
            if (pixelIdx >= 0 && pixelIdx < PIXELS) {
                accumulatedData[pixelIdx] += val;
            }
            while(IS_CLK_HIGH); 
        }
    }

    // --- 4倍オーバーサンプリング補間ロジック ---
    for(int j=0; j<OUTPUT_POINTS; j++) {
        // インデックス j を 4.0 で割ることで、元のピクセル位置（0.0, 0.25, 0.5, 0.75, 1.0...）を算出
        float target_i = (float)j / 4.0f;
        int i1 = (int)target_i;
        float t = target_i - (float)i1;

        if (t == 0.0f) {
            // ピタリと一致する点は補間せず生データ（累積値）をそのまま使用
            finalOutput[j] = (uint16_t)accumulatedData[i1];
        } else {
            // その間の点は3次補間
            int i0 = max(i1 - 1, 0);
            int i2 = min(i1 + 1, PIXELS - 1);
            int i3 = min(i1 + 2, PIXELS - 1);
            float val = cubicInterpolate((float)accumulatedData[i0], (float)accumulatedData[i1], (float)accumulatedData[i2], (float)accumulatedData[i3], t);
            finalOutput[j] = (uint16_t)(val < 0 ? 0 : val);
        }
    }

    const uint8_t header[] = {0x00, 0x00, 0xFF, 0xFF};
    const uint8_t footer[] = {0x0F, 0x0F, 0x0F, 0x0F};
    Serial.write(header, 4);
    Serial.write((uint8_t*)finalOutput, OUTPUT_POINTS * 2);
    Serial.write(footer, 4);
    Serial.flush();
}

void loop() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n'); cmd.trim();
        if (cmd == "send") performMeasurement();
        else if (cmd.startsWith("exp:")) exposureMs = max(10, cmd.substring(4).toInt());
    }
}
