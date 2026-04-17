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

/* System Heartbeat (Timer 0) */
ISR(TIMER0_COMPA_vect) {
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
    DDRD |= (1 << PD5);      // PWM Output
    DDRD &= ~(1 << PD3);     // TACH Input
    PORTD |= (1 << PD3);    // Internal Pull-up for TACH

    // Timer 2: Fast PWM on OC2B (PD5), non-inverting
    TCCR2A = (1 << COM2B1) | (1 << WGM21) | (1 << WGM20);
    // Prescaler 64: Frequency ~976 Hz (Acceptable range 30Hz-300kHz)
    TCCR2B = (1 << CS22); 
    OCR2B = 0;
}

void update_fan_hardware(void) {
    if (!fan_on) {
        OCR2B = 0;
    } else {
        if (counter > 100) counter = 100;
        if (counter < 0)   counter = 0;
        
        // Linear mapping to 8-bit range
        // If counter is 0, explicitly set 0. 
        if (counter == 0) OCR2B = 0;
        else OCR2B = (uint8_t)((counter * 255) / 100);
    }
}

/* Interrupt Service Routines */
ISR(INT0_vect) { // Power Button
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

ISR(PCINT0_vect) { // RPG
    static uint8_t last_state = 0;
    uint8_t curr_state = (PINB & 0x03); 
    if (fan_on && (curr_state != last_state)) {
        if (curr_state == 0x00) { // On detent
            if (last_state == 0x02)      counter++;
            else if (last_state == 0x01) counter--;
        }
        last_state = curr_state;
    }
}

int main(void) {
    // Timer 0 for millis()
    TCCR0A = (1 << WGM01); 
    OCR0A  = 249;          
    TCCR0B = (1 << CS01) | (1 << CS00); 
    TIMSK0 = (1 << OCIE0A);

    lcd_init();
    fan_init();

    // INT0 (Button) and INT1 (Tach)
    EICRA |= (1 << ISC01) | (1 << ISC11); // Falling edges
    EIMSK |= (1 << INT0) | (1 << INT1);
    
    // PCINT (RPG)
    PCICR  |= (1 << PCIE0);
    PCMSK0 |= (1 << PCINT0) | (1 << PCINT1);
    
    sei();
    
    char buffer[16];
    
    while (1) {
        update_fan_hardware();
        
        lcd_command(0x80); 
        sprintf(buffer, "DC=%d.0%%   ", counter);
        lcd_print(buffer);
        
        lcd_command(0xC0);
        if (!fan_on) {
            lcd_print("Fan:OFF    ");
        } else {
            // Note: Lab 4 Slide 8 mentions min start duty is 25%.
            if (counter < 25) lcd_print("Fan:LOW    ");
            else             lcd_print("Fan:ON     ");
        }
        
        _delay_ms(50);
    }
}