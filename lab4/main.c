#define F_CPU 16000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdbool.h>
#include <stdio.h>

/* === PIN CONNECTIONS ===
LCD:
RS -> A0
E  -> A1
D4 -> A2
D5 -> A3
D6 -> A4
D7 -> A5

PUSH BUTTON -> PD2 (INT0)
RPG A       -> PB1 (Digital 9)
RPG B       -> PB0 (Digital 8)

FAN:
Power -> VIN (9V Wall Wart)
GND   -> GND
PWM   -> PD5 (OC2B / Digital 5)
TACH  -> PD3 (INT1 / Digital 3)
*/

/* ================= DEBOUNCE & GLOBAL STATE ================= */

#define BTN_DEBOUNCE_MS 40
#define RPG_DEBOUNCE_MS 2

volatile uint32_t ms_ticks = 0;
volatile int16_t counter = 50; // Start at 50%
volatile bool fan_on = true;

/* millisecond timer using Timer0 */
ISR(TIMER0_COMPA_vect)
{
    ms_ticks++;
}

uint32_t millis(void)
{
    uint32_t t;
    cli();
    t = ms_ticks;
    sei();
    return t;
}

/* ================= LCD LOW-LEVEL ================= */

#define LCD_PORT PORTC
#define LCD_DDR  DDRC
#define LCD_RS PC0
#define LCD_E  PC1

void lcd_strobe(void)
{
    LCD_PORT |= (1 << LCD_E);
    _delay_us(1);
    LCD_PORT &= ~(1 << LCD_E);
    _delay_us(100);
}

void lcd_write_nibble(uint8_t nibble, bool is_data)
{
    // Clear pins PC2-PC5 (D4-D7)
    LCD_PORT &= 0xC3; 
    // Set data pins
    LCD_PORT |= (nibble << 2);
    
    if (is_data) LCD_PORT |= (1 << LCD_RS);
    else         LCD_PORT &= ~(1 << LCD_RS);
    
    lcd_strobe();
}

void lcd_command(uint8_t cmd)
{
    lcd_write_nibble(cmd >> 4, false);
    lcd_write_nibble(cmd & 0x0F, false);
    _delay_ms(2);
}

void lcd_char(uint8_t data)
{
    lcd_write_nibble(data >> 4, true);
    lcd_write_nibble(data & 0x0F, true);
}

void lcd_print(const char* str)
{
    while (*str) lcd_char(*str++);
}

void lcd_init(void)
{
    LCD_DDR = 0xFF;
    _delay_ms(50);
    
    // Manual 4-bit init sequence
    lcd_write_nibble(0x03, false); _delay_ms(5);
    lcd_write_nibble(0x03, false); _delay_us(200);
    lcd_write_nibble(0x03, false);
    lcd_write_nibble(0x02, false); 
    
    lcd_command(0x28); // 2 lines, 5x8
    lcd_command(0x0C); // Display ON
    lcd_command(0x01); // Clear
    _delay_ms(2);
}

/* ================= FAN & INTERRUPTS ================= */

void fan_init(void)
{
    // PD5 as Output for PWM (OC2B)
    DDRD |= (1 << PD5);
    // PD3 as Input with Pull-up for Tachometer
    DDRD &= ~(1 << PD3);
    PORTD |= (1 << PD3);

    // Timer 2 Setup for Fast PWM on OC2B (PD5)
    // WGM21:0 = 3 (Fast PWM), COM2B1 = 1 (Non-inverting)
    TCCR2A = (1 << COM2B1) | (1 << WGM21) | (1 << WGM20);
    // Prescaler 64: 16MHz / 64 / 256 = ~976 Hz
    TCCR2B = (1 << CS22); 
}

void update_fan_speed(void)
{
    if (!fan_on) {
        OCR2B = 0;
        return;
    }
    
    // Clamp counter between 1 and 100 for duty cycle %
    if (counter > 100) counter = 100;
    if (counter < 1)   counter = 1;
    
    // Map 1-100% to 0-255 hardware register
    OCR2B = (uint8_t)((counter * 255) / 100);
}

ISR(INT0_vect) // Pushbutton on PD2
{
    static uint32_t last_press = 0;
    uint32_t now = millis();
    if (now - last_press > BTN_DEBOUNCE_MS) {
        fan_on = !fan_on;
        last_press = now;
    }
}

ISR(PCINT0_vect) // RPG on PB0/PB1
{
    static uint8_t last_state = 0;
    uint8_t curr_state = (PINB & 0x03); 
    
    if (curr_state != last_state) {
        // Simple state-based RPG logic
        if (last_state == 0x02 && curr_state == 0x00) counter++;
        if (last_state == 0x01 && curr_state == 0x00) counter--;
        last_state = curr_state;
    }
}

/* ================= SYSTEM INIT ================= */

void timer0_init(void)
{
    TCCR0A = (1 << WGM01); // CTC
    OCR0A  = 249;          // 1ms
    TCCR0B = (1 << CS01) | (1 << CS00); // 64 prescaler
    TIMSK0 = (1 << OCIE0A);
}

void interrupts_init(void)
{
    // Button on INT0 (PD2)
    EICRA |= (1 << ISC01); // Falling edge
    EIMSK |= (1 << INT0);
    
    // RPG on PCINT0 (PB0, PB1)
    PCICR |= (1 << PCIE0);
    PCMSK0 |= (1 << PCINT0) | (1 << PCINT1);
    
    sei();
}

int main(void)
{
    timer0_init();
    lcd_init();
    fan_init();
    interrupts_init();
    
    char buffer[16];
    
    while (1)
    {
        update_fan_speed();
        
        lcd_command(0x80); // Line 1
        sprintf(buffer, "DC=%d.0%%  ", counter);
        lcd_print(buffer);
        
        lcd_command(0xC0); // Line 2
        if (fan_on) lcd_print("Fan:ON     ");
        else        lcd_print("Fan:OFF    ");
        
        _delay_ms(50);
    }
}