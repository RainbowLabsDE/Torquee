#include "ch32fun.h"
#include <stdio.h>

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
static const int ADC_CHANNEL_VIN = ANALOG_0;   // PA2
static const int ADC_CHANNEL_VOUT = ANALOG_1;  // PA1


static const int R1 = 120000, R2 = 10000, VCC = 5063;
static const int ADC_FULL_SCALE_MV = VCC * (R1+R2) / R2;
#define ADC_TO_MV(adcVal) ( adcVal * ADC_FULL_SCALE_MV / 1023 ) // do it like this to circumvent integer imprecision

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
	TIM2->ATRLR = 4095;
	
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


int main()
{
    SysTick->CNT = 0;
	SystemInit();

	// Enable GPIOs
	funGpioInitAll();

	funPinMode( PIN_LED_STATUS, MODE_OUTPUT );
	funPinMode( PIN_PRECHG, MODE_OUTPUT );
	funPinMode( PIN_CONTACTOR, MODE_OUTPUT );
    funDigitalWrite( PIN_PRECHG, FUN_LOW);
    funDigitalWrite( PIN_CONTACTOR, FUN_LOW);
    // Analog pins don't need to be set, because that's the default state
    
    funAnalogInit();
    
    // Enable remap, so PC5 is T2CH1
    funPinMode( PIN_PRECHG, MODE_ALTFUNC );
    AFIO->PCFR1 |= AFIO_PCFR1_TIM2_REMAP_PARTIALREMAP1;
    t2pwm_init();
    t2pwm_setpw(0, 0);

    iwdg_setup(100, IWDG_Prescaler_128);  // setup watchdog to 100 ms (runs at 1kHz -> timeout after 100 cycles)



	// while(1)
	// {
	// 	funDigitalWrite( PIN_LED_STATUS, FUN_LOW );
	// 	funDigitalWrite( PIN_PRECHG, FUN_HIGH );
	// 	Delay_Ms( 250 );
	// 	funDigitalWrite( PIN_LED_STATUS, FUN_HIGH );
	// 	funDigitalWrite( PIN_PRECHG, FUN_LOW );
	// 	Delay_Ms( 250 );

    //     int adcVin = funAnalogRead(ADC_CHANNEL_VIN);
    //     int adcVout = funAnalogRead(ADC_CHANNEL_VOUT);

    //     printf("ADC:    %4d, %4dn", adcVin, adcVout);
    //     printf("ADC mV: %5d, %5d\n", ADC_TO_MV(adcVin), ADC_TO_MV(adcVout));
	// }

    uint32_t lastBlink = 0, lastADCRead = 0;

    while(1) {

        if (funSysTick32() - lastBlink > Ticks_from_Ms(100)) {
            lastBlink = funSysTick32();

            uint8_t state = funSysTick32() / Ticks_from_Ms(100) % 2;

            funDigitalWrite( PIN_LED_STATUS, state );
		    // funDigitalWrite( PIN_PRECHG, !state );
        }

        if (funSysTick32() - lastADCRead > Ticks_from_Ms(1)) {
            lastADCRead = funSysTick32();

            int adcVin = funAnalogRead(ADC_CHANNEL_VIN);
            int adcVout = funAnalogRead(ADC_CHANNEL_VOUT);

            printf("%6ld,%4d,%4d,%5d,%5d\n", funSysTick32()/DELAY_MS_TIME, adcVin, adcVout, ADC_TO_MV(adcVin), ADC_TO_MV(adcVout));
        }

        if (funSysTick32() > Ticks_from_Ms(11000)) {
            funDigitalWrite( PIN_PRECHG, FUN_LOW);
            t2pwm_setpw(0, 0); // turn off precharge FET RC filter

        }
        else if (funSysTick32() > Ticks_from_Ms(2000)) {
            t2pwm_setpw(0, (4095/3)); // slow down precharge FET RC filter
        }
        else if (funSysTick32() > Ticks_from_Ms(1000)) {
            // funDigitalWrite( PIN_PRECHG, FUN_HIGH);
            t2pwm_setpw(0, 4095); // turn on precharge FET RC filter fully for 1s
            funDigitalWrite( PIN_CONTACTOR, FUN_HIGH);
        }

        iwdg_feed();
    }
}