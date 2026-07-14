#include "ch32fun.h"
#include <stdio.h>

// TODOs
// - DMA ADC reading
// - state machine 
// - output voltage slope verification (too slow or too fast is bad)
// - fast shutdown if input voltage drops


static const int PIN_PRECHG = PC5;
static const int PIN_LED_STATUS = PC4;
static const int ADC_CHANNEL_VIN = ANALOG_0;   // PA2
static const int ADC_CHANNEL_VOUT = ANALOG_1;  // PA1

static const int R1 = 120000, R2 = 10000;
static const int ADC_FULL_SCALE_MV = 5000 * (R1+R2) / R2;
#define ADC_TO_MV(adcVal) ( adcVal * ADC_FULL_SCALE_MV / 1023 ) // do it like this to circumvent integer imprecision

#define MODE_OUTPUT GPIO_Speed_10MHz | GPIO_CNF_OUT_PP

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



int main()
{
	SystemInit();

	// Enable GPIOs
	funGpioInitAll();

	funPinMode( PIN_LED_STATUS, MODE_OUTPUT );
	funPinMode( PIN_PRECHG, MODE_OUTPUT );
    // Analog pins don't need to be set, because that's the default state

    funAnalogInit();

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

            funDigitalWrite( PIN_LED_STATUS, !state );
		    funDigitalWrite( PIN_PRECHG, state );
        }

        if (funSysTick32() - lastADCRead > Ticks_from_Ms(100)) {
            lastADCRead = funSysTick32();

            int adcVin = funAnalogRead(ADC_CHANNEL_VIN);
            int adcVout = funAnalogRead(ADC_CHANNEL_VOUT);

            printf("ADC:    %4d, %4dn", adcVin, adcVout);
            printf("ADC mV: %5d, %5d\n", ADC_TO_MV(adcVin), ADC_TO_MV(adcVout));
        }

        iwdg_feed();
    }
}