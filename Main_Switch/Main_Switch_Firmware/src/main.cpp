#include "ch32fun.h"
#include <stdio.h>
#include <stdbool.h>

// TODOs
// - DMA ADC reading
// - state machine 
// - output voltage slope verification (too slow or too fast is bad)
// - fast shutdown if input voltage drops

// calib:
// VIN (displayed, actual). With clean 5V and resistor values assumed
// 3812 -> 3986
// 22937 -> 24000
// 34819 -> 36003
// 40728 -> 42004
// 48670 -> 50005

static const int PIN_CONTACTOR = PC3;
static const int PIN_PRECHG = PC5;              // TIM2 CH1 (with remap)
static const int PIN_LED_STATUS = PC4;
static const int ADC_CHANNEL_VIN = ANALOG_0;    // PA2
static const int ADC_CHANNEL_VOUT = ANALOG_1;   // PA1

static const int PWM_MAX_VAL = 4095;


static const int FIXED_POINT_ACCURACY = 16;     // bits
static const int FIXED_POINT_MASK = ((1 << FIXED_POINT_ACCURACY) - 1);
#define FIXPT_FROM_INT(a) ((a) << FIXED_POINT_ACCURACY)
#define FIXPT_TO_INT(a) ((a) >> FIXED_POINT_ACCURACY)
#define FIXPT_TO_FRAC_MP(a, multiplier) ((long long)((a) & FIXED_POINT_MASK) * multiplier / (1 << FIXED_POINT_ACCURACY))
#define FIXPT_TO_FRAC(a) (FIXPT_TO_FRAC_MP(a, 100000))
#define FIXPT_PRINTF(a, intDigits, fracMultiplier) printf("%*d.%0d", intDigits, FIXPT_TO_INT(a), FIXPT_TO_FRAC_MP(a, fracMultiplier))
typedef int fip_16_t;


static const int R1 = 120000, R2 = 10000, VCC = 5063;
static const int ADC_FULL_SCALE_MV = VCC * (R1+R2) / R2;
#define ADC_TO_MV(adcVal) ( adcVal * ADC_FULL_SCALE_MV / 1023 ) // do it like this to circumvent integer imprecision

static const int TARGET_SLOPE_MV_S = 50000;     		// mV / s
static const int TARGET_SLOPE = (TARGET_SLOPE_MV_S * 1023 / ADC_FULL_SCALE_MV); // ADC bits / s
static const int PRECHG_LOOP_INTERVAL = 100;    		// µs
static const int PRECHG_ADC_AVG_DIVISOR = 8;  		 	// moving average divisor for adc read values (the higher the slower the filter)
static const int PRECHG_VIN_HISTORY_BUF_SIZE = 16;
static const int PRECHG_VIN_HISTORY_AVG_COUNT = 4;      // how many history samples to average
static const int PRECHG_UPDATE_HISTORY_INTERVAL = 8;    // update history every X iterations
static const int LAST_HISTORY_ENTRY_ELAPSED = PRECHG_LOOP_INTERVAL * ((PRECHG_UPDATE_HISTORY_INTERVAL-1) * PRECHG_VIN_HISTORY_BUF_SIZE);  // µs, how long the last history entry was ago
bool doPrecharge = false;


#define MODE_OUTPUT GPIO_Speed_10MHz | GPIO_CNF_OUT_PP
#define MODE_ALTFUNC GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF

// IWDG runs from LSI (around 128KHz), reload value can be up to 12 bits
static void iwdg_setup(uint16_t reload_val, uint8_t prescaler) {
	IWDG->CTLR = 0x5555;
	IWDG->PSCR = prescaler;

	IWDG->CTLR = 0x5555;
	IWDG->RLDR = reload_val & 0xfff;

	IWDG->CTLR = 0xCCCC;
}

static void iwdg_feed() {
	IWDG->CTLR = 0xAAAA;
}


// from https://github.com/cnlohr/ch32fun/blob/master/examples/tim2_pwm_remap/tim2_pwm_remap.c
void t2pwm_init( void )
{
	// Enable GPIOC, GPIOD, TIM2, and AFIO *very important!*
	RCC->APB2PCENR |= RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOD | RCC_APB2Periph_GPIOC;
	RCC->APB1PCENR |= RCC_APB1Periph_TIM2;
	
	// Reset TIM2 to init all regs
	RCC->APB1PRSTR |= RCC_APB1Periph_TIM2;
	RCC->APB1PRSTR &= ~RCC_APB1Periph_TIM2;
	
	// SMCFGR: default clk input is CK_INT
	// set TIM2 clock prescaler divider 
	TIM2->PSC = 0x0000;
	// set PWM total cycle width
	TIM2->ATRLR = PWM_MAX_VAL;
	
	// for channel 1 and 2, let CCxS stay 00 (output), set OCxM to 110 (PWM I)
	// enabling preload causes the new pulse width in compare capture register only to come into effect when UG bit in SWEVGR is set (= initiate update) (auto-clears)
	TIM2->CHCTLR1 |= TIM_OC1M_2 | TIM_OC1M_1 | TIM_OC1PE;

	// CTLR1: default is up, events generated, edge align
	// enable auto-reload of preload
	TIM2->CTLR1 |= TIM_ARPE;

	// Enable CH1 output, positive pol
	TIM2->CCER |= TIM_CC1E | TIM_CC1NP;

	// initialize counter
	TIM2->SWEVGR |= TIM_UG;

	// Enable TIM2
	TIM2->CTLR1 |= TIM_CEN;
}

void t2pwm_setpw(uint8_t chl, uint16_t width)
{
	switch(chl&3)
	{
		case 0: TIM2->CH1CVR = width; break;
		case 1: TIM2->CH2CVR = width; break;
		case 2: TIM2->CH3CVR = width; break;
		case 3: TIM2->CH4CVR = width; break;
	}
}


uint32_t lastPrechargeLoop = 0;
bool prechargeLoopFirstRun = true;
uint8_t prechargeLoopIterations = 0;    // overflows at UPDATE_HISTORY_INTERVAL

fip_16_t adcVinAvg = 0, adcVoutAvg = 0;  // fixed point
fip_16_t adcVoutAvgBuf[PRECHG_VIN_HISTORY_BUF_SIZE];
int adcVoutAvgBufIdx = 0;   // points to next location that will be written to

void prechargeLoop() {
    if (funSysTick32() - lastPrechargeLoop > Ticks_from_Us(PRECHG_LOOP_INTERVAL)) {
        lastPrechargeLoop = funSysTick32();
        prechargeLoopIterations++;
        
        // TODO: replace with DMA ADC?
        int adcVin = funAnalogRead(ADC_CHANNEL_VIN);
        int adcVout = funAnalogRead(ADC_CHANNEL_VOUT);
        
        // initialize averages faster
        if (prechargeLoopFirstRun) {
            prechargeLoopFirstRun = false;
            
            adcVinAvg = FIXPT_FROM_INT(adcVin);
            adcVoutAvg = FIXPT_FROM_INT(adcVout);
            
            for (int i = 0; i < PRECHG_VIN_HISTORY_BUF_SIZE; i++) {
                adcVoutAvgBuf[i] = adcVoutAvg;
            }
        }
        
        // calculate simple moving average
        adcVinAvg -= adcVinAvg / PRECHG_ADC_AVG_DIVISOR;
        adcVinAvg += FIXPT_FROM_INT(adcVin) / PRECHG_ADC_AVG_DIVISOR;
        adcVoutAvg -= adcVoutAvg / PRECHG_ADC_AVG_DIVISOR;
        adcVoutAvg += FIXPT_FROM_INT(adcVout) / PRECHG_ADC_AVG_DIVISOR;
        
        
        // fip_16_t historyVal = adcVoutAvgBuf[adcVoutAvgBufIdx];
        fip_16_t historyVal = 0;
        for (int i = 0; i < PRECHG_VIN_HISTORY_AVG_COUNT; i++) {
            int idx = i + adcVoutAvgBufIdx;
            if (idx >= PRECHG_VIN_HISTORY_BUF_SIZE) {
                idx -= PRECHG_VIN_HISTORY_BUF_SIZE;
            }
            historyVal += adcVoutAvgBuf[idx];
        }
        historyVal /= PRECHG_VIN_HISTORY_AVG_COUNT;

        int deltaAvg = adcVout - FIXPT_TO_INT(historyVal);
        int usSinceHistoryVal = LAST_HISTORY_ENTRY_ELAPSED + PRECHG_LOOP_INTERVAL * prechargeLoopIterations;    // also calc sub-history-entry-interval time
        int slopeBitsPerS = deltaAvg * 1'000'000 / usSinceHistoryVal;
        if (slopeBitsPerS > INT16_MAX) {
            slopeBitsPerS = INT16_MAX;
        }

        fip_16_t pwmPercent = FIXPT_FROM_INT(0);
        if (slopeBitsPerS < 0) {
            pwmPercent = FIXPT_FROM_INT(1);
        }
        else if (slopeBitsPerS > (TARGET_SLOPE*3/2)) {
            pwmPercent = FIXPT_FROM_INT(0);
        }
        else {
            fip_16_t slope = FIXPT_FROM_INT(slopeBitsPerS);
            const fip_16_t valMin = FIXPT_FROM_INT(0);
            const fip_16_t valMax = FIXPT_FROM_INT(TARGET_SLOPE*3/2);

            pwmPercent = FIXPT_FROM_INT(1);            // invert 0-1 range
            pwmPercent -= ((slope - valMin) / FIXPT_TO_INT(valMax - valMin));   // calculate simple ratio. (For fixed point division, you need to divide by the integer part only)
        }

        int pwmVal = FIXPT_TO_INT(pwmPercent * PWM_MAX_VAL);
		if (!doPrecharge) {
			pwmVal = 0;
		}
        t2pwm_setpw(0, pwmVal);

        
        // update history every X iterations
        if (prechargeLoopIterations >= PRECHG_UPDATE_HISTORY_INTERVAL) {
            prechargeLoopIterations = 0;

            adcVoutAvgBuf[adcVoutAvgBufIdx] = adcVoutAvg;
            adcVoutAvgBufIdx++;
            if (adcVoutAvgBufIdx >= PRECHG_VIN_HISTORY_BUF_SIZE) {
                adcVoutAvgBufIdx = 0;
            }

            printf("%9ld,%4d,%4d,%4d,%4d,%5d,%4d\n", 
                funSysTick32()/DELAY_US_TIME,
                FIXPT_TO_INT(adcVinAvg),
                FIXPT_TO_INT(adcVoutAvg),
                FIXPT_TO_INT(historyVal),
                deltaAvg,
                slopeBitsPerS,
                pwmVal
            );
        }
    }
}


int main()
{
    SysTick->CNT = 0;
	SystemInit();

	// Enable GPIOs
	funGpioInitAll();

	funPinMode( PIN_LED_STATUS, MODE_OUTPUT );
	funPinMode( PIN_PRECHG, MODE_OUTPUT );
	funPinMode( PIN_CONTACTOR, MODE_OUTPUT );
    funDigitalWrite( PIN_PRECHG, FUN_LOW );
    funDigitalWrite( PIN_CONTACTOR, FUN_LOW );
    // Analog pins don't need to be set, because that's the default state
    
    funAnalogInit();
    
    // Enable remap, so PC5 is T2CH1
    funPinMode( PIN_PRECHG, MODE_ALTFUNC );
    AFIO->PCFR1 |= AFIO_PCFR1_TIM2_REMAP_PARTIALREMAP1;
    t2pwm_init();
    t2pwm_setpw(0, 0);

    iwdg_setup(100, IWDG_Prescaler_128);  // setup watchdog to 100 ms (runs at 1kHz -> timeout after 100 cycles)


    uint32_t lastBlink = 0, lastADCRead = 0;

    while(1) {

        if (funSysTick32() - lastBlink > Ticks_from_Ms(100)) {
            lastBlink = funSysTick32();

            uint8_t state = funSysTick32() / Ticks_from_Ms(100) % 2;

            funDigitalWrite( PIN_LED_STATUS, state );
		    // funDigitalWrite( PIN_PRECHG, !state );
        }

        // if (funSysTick32() - lastADCRead > Ticks_from_Ms(1)) {
        //     lastADCRead = funSysTick32();

        //     int adcVin = funAnalogRead(ADC_CHANNEL_VIN);
        //     int adcVout = funAnalogRead(ADC_CHANNEL_VOUT);

        //     printf("%6ld,%4d,%4d,%5d,%5d\n", funSysTick32()/DELAY_MS_TIME, adcVin, adcVout, ADC_TO_MV(adcVin), ADC_TO_MV(adcVout));
        // }

        // if (funSysTick32() > Ticks_from_Ms(11000)) {
        //     funDigitalWrite( PIN_PRECHG, FUN_LOW);
        //     t2pwm_setpw(0, 0); // turn off precharge FET RC filter

        // }
        // else if (funSysTick32() > Ticks_from_Ms(2000)) {
        //     t2pwm_setpw(0, (4095/3)); // slow down precharge FET RC filter
        // }
        else if (funSysTick32() > Ticks_from_Ms(1000)) {
        //     // funDigitalWrite( PIN_PRECHG, FUN_HIGH);
        //     t2pwm_setpw(0, 4095); // turn on precharge FET RC filter fully for 1s
        //     funDigitalWrite( PIN_CONTACTOR, FUN_HIGH);
            doPrecharge = true;
        }

        prechargeLoop();

        iwdg_feed();
    }
}