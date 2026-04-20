#define F_CPU 16000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdbool.h>
#include <stdio.h>

/* === PIN CONNECTIONS ===
LCD:
RS -> A0 (PC0)
E  -> A1 (PC1)
D4 -> A2 (PC2)
D5 -> A3 (PC3)
D6 -> A4 (PC4)
D7 -> A5 (PC5)

PUSH BUTTON -> PD2 (INT0)
RPG A       -> PB1 (Digital 9)
RPG B       -> PB0 (Digital 8)

FAN:
Power -> VIN (9V Wall Wart)
GND   -> GND
PWM   -> PD5 (OC2B / Digital 5)
TACH  -> PD3 (INT1 / Digital 3)
*/

#define BTN_DEBOUNCE_MS 50
#define RPG_DEBOUNCE_MS 2

volatile uint32_t ms_ticks = 0;
volatile int16_t counter = 50; 
volatile bool fan_on = true;
volatile uint32_t pulse_count = 0;
volatile uint32_t last_pulse_count = 0;
volatile uint32_t last_time = 0;
volatile uint16_t rpm = 0;

/* System Heartbeat (Now using Timer 2) - 1ms ticks */
ISR(TIMER2_COMPA_vect) {
    ms_ticks++;
}

uint32_t millis(void) {
    uint32_t t;
    cli();
    t = ms_ticks;
    sei();
    return t;
}

/* LCD Functions */
#define LCD_PORT PORTC
#define LCD_DDR  DDRC
#define LCD_RS PC0
#define LCD_E  PC1

void lcd_strobe(void) {
    LCD_PORT |= (1 << LCD_E);
    _delay_us(1);
    LCD_PORT &= ~(1 << LCD_E);
    _delay_us(100);
}

void lcd_write_nibble(uint8_t nibble, bool is_data) {
    LCD_PORT &= 0xC3; 
    LCD_PORT |= (nibble << 2);
    if (is_data) LCD_PORT |= (1 << LCD_RS);
    else         LCD_PORT &= ~(1 << LCD_RS);
    lcd_strobe();
}

void lcd_command(uint8_t cmd) {
    lcd_write_nibble(cmd >> 4, false);
    lcd_write_nibble(cmd & 0x0F, false);
    _delay_ms(2);
}

void lcd_char(uint8_t data) {
    lcd_write_nibble(data >> 4, true);
    lcd_write_nibble(data & 0x0F, true);
}

void lcd_print(const char* str) {
    while (*str) lcd_char(*str++);
}

void lcd_init(void) {
    LCD_DDR = 0xFF;
    _delay_ms(50);
    lcd_write_nibble(0x03, false); _delay_ms(5);
    lcd_write_nibble(0x03, false); _delay_us(200);
    lcd_write_nibble(0x03, false);
    lcd_write_nibble(0x02, false); 
    lcd_command(0x28); 
    lcd_command(0x0C); 
    lcd_command(0x01); 
    _delay_ms(2);
}

/* Fan Control Logic */
void fan_init(void) {
    // 1. Configure Pin Directions
    DDRD |= (1 << PD5);      // PWM Output (OC0B)
    DDRD &= ~((1 << PD2) | (1 << PD3)); // Inputs: Button (PD2) and Tach (PD3)
    
    // 2. Enable Pull-ups for Inputs (CRITICAL for "ON" Logic stability)
    PORTD |= (1 << PD2) | (1 << PD3);    

    /* 3. Timer 0: Fast PWM on OC0B (PD5) 
       Requirement: 80 kHz frequency. 
       Frequency = F_CPU / (Prescaler * (1 + TOP))
       For 80kHz: 16MHz / (1 * (1 + 199)) = 80,000 Hz
       We use Mode 7 (Fast PWM with OCR0A as TOP) */
    
    TCCR0A = (1 << COM0B1) | (1 << WGM01) | (1 << WGM00); // Fast PWM, Non-inv OC0B
    TCCR0B = (1 << WGM02) | (1 << CS00);                 // Mode 7, No Prescaling
    
    OCR0A = 199; // Sets frequency to 80 kHz
    OCR0B = 0;   // Initialize duty cycle to 0
}

void update_fan_hardware(void) {
    if (!fan_on) {
        OCR0B = 0;
    } else {
        // Enforce bounds 1-100%
        if (counter > 100) counter = 100;
        if (counter < 1)   counter = 1;
        
        uint16_t temp_ocr = (uint16_t)((counter * 199UL) / 100UL);
        OCR0B = (uint8_t)temp_ocr;
    }
}

/* Interrupt Service Routines */
ISR(INT0_vect) { // Power Button Toggle
    static uint32_t last_press = 0;
    uint32_t now = millis();
    if (now - last_press > BTN_DEBOUNCE_MS) {
        fan_on = !fan_on;
        last_press = now;
    }
}

ISR(INT1_vect) { // Tachometer Pulses
    pulse_count++;
}

ISR(PCINT0_vect) { // RPG Rotation
    static uint8_t last_state = 0;
    uint8_t curr_state = (PINB & 0x03); 
    
    // Only adjust speed if fan is currently toggled ON
    if (fan_on && (curr_state != last_state)) {
        if (curr_state == 0x00) { // Stable detent
            if (last_state == 0x02)      counter++;
            else if (last_state == 0x01) counter--;
        }
        last_state = curr_state;
    }
}

int main(void) {
    // 1. Initialize System Timer (Now using Timer 2) for millis()
    TCCR2A = (1 << WGM21); // CTC Mode
    OCR2A  = 249;          // 1ms @ 16MHz/64
    TCCR2B = (1 << CS22);  // 64 prescaler
    TIMSK2 = (1 << OCIE2A);

    // 2. Initialize Hardware Peripherals
    lcd_init();
    fan_init();

    // 3. Configure External Interrupts
    // INT0: Button (PD2), INT1: Tachometer (PD3)
    EICRA |= (1 << ISC01) | (1 << ISC11); // Falling edges
    EIMSK |= (1 << INT0) | (1 << INT1);
    
    // 4. Configure Pin Change Interrupts (RPG)
    PCICR  |= (1 << PCIE0); // Group 0
    PCMSK0 |= (1 << PCINT0) | (1 << PCINT1); // PB0, PB1
    
    // 5. Enable Global Interrupts
    sei();
    
    char buffer[16];
    
    while (1) {
        // Ensure hardware reflects software state
        update_fan_hardware();
		uint32_t now = millis();
		
		if(now - last_time >= 1000){
			uint32_t pulses = pulse_count - last_pulse_count;
			last_pulse_count = pulse_count;
			last_time = now;
			rpm = pulses * 30;
		}
		
        
        // Refresh LCD UI
        lcd_command(0x80); // Line 1
        sprintf(buffer, "DC = %d.0%%   ", counter);
        lcd_print(buffer);
        
        lcd_command(0xC0); // Line 2
        if (!fan_on) {
	        lcd_print("Fan: OFF     ");
	        } else {
	        if (rpm == 0) {
		        lcd_print("Fan: stopped "); 
	        }
	        else if (rpm < 2640) {
		        lcd_print("Fan: low RPM "); 
	        }
	        else {
		        lcd_print("Fan: RPM OK  "); 
	        }
        }
        
        _delay_ms(50); // Refresh rate
    }
}
