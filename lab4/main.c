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
    // Set data pins (bits 2-5)
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
    lcd_command(0x0C); // Display ON, Cursor OFF
    lcd_command(0x01); // Clear
    _delay_ms(2);
}

/* ================= FAN & INTERRUPTS ================= */

void fan_init(void)
{
    // PD5 as Output for PWM (OC2B)
    DDRD |= (1 << PD5);
    // PD3 as Input with Pull-up for Tachometer (Bonus Monitoring Prep)
    DDRD &= ~(1 << PD3);
    PORTD |= (1 << PD3);

    // Timer 2 Setup for Fast PWM on OC2B (PD5)
    // WGM21:0 = 3 (Fast PWM), COM2B1 = 1 (Non-inverting Mode)
    TCCR2A = (1 << COM2B1) | (1 << WGM21) | (1 << WGM20);
    // Prescaler 64: 16MHz / 64 / 256 = ~976 Hz
    TCCR2B = (1 << CS22); 
    
    OCR2B = 0; // Initialize fan to off
}

void update_fan_hardware(void)
{
    if (!fan_on) {
        OCR2B = 0; // Turn off PWM output
    } else {
        // Clamp duty cycle between 1% and 100%
        if (counter > 100) counter = 100;
        if (counter < 1)   counter = 1;
        
        // Convert percentage (1-100) to 8-bit register value (0-255)
        // Note: 255/100 is approx 2.55. Using integer math:
        OCR2B = (uint8_t)((counter * 255) / 100);
    }
}

ISR(INT0_vect) // Pushbutton on PD2 (INT0)
{
    static uint32_t last_press = 0;
    uint32_t now = millis();
    
    // Debounce check
    if (now - last_press > BTN_DEBOUNCE_MS) {
        fan_on = !fan_on; // Toggle system state
        last_press = now;
    }
}

ISR(PCINT0_vect) // RPG on PB0 (B) and PB1 (A)
{
    static uint8_t last_state = 0;
    uint8_t curr_state = (PINB & 0x03); 
    
    // RPG should only be adjustable if the fan is on
    if (fan_on && (curr_state != last_state)) {
        // Looking for detent transitions (e.g., state 00)
        if (curr_state == 0x00) {
            if (last_state == 0x02)      counter++; // CW
            else if (last_state == 0x01) counter--; // CCW
        }
        last_state = curr_state;
    }
}

/* ================= SYSTEM INIT ================= */

void timer0_init(void)
{
    // Configure Timer 0 for 1ms heartbeat
    TCCR0A = (1 << WGM01); // CTC Mode
    OCR0A  = 249;          // 1ms interval at 16MHz/64
    TCCR0B = (1 << CS01) | (1 << CS00); // 64 prescaler
    TIMSK0 = (1 << OCIE0A); // Enable compare match interrupt
}

void interrupts_init(void)
{
    // External Interrupt 0 (Button)
    EICRA |= (1 << ISC01); // Trigger on falling edge
    EIMSK |= (1 << INT0);  // Enable INT0
    
    // Pin Change Interrupt (RPG)
    PCICR  |= (1 << PCIE0);  // Enable PCINT0 group
    PCMSK0 |= (1 << PCINT0) | (1 << PCINT1); // Pins PB0 and PB1
    
    sei(); // Enable global interrupts
}

int main(void)
{
    // 1. Initialize Subsystems
    timer0_init();
    lcd_init();
    fan_init();
    interrupts_init();
    
    char buffer[16];
    
    while (1)
    {
        // 2. Sync Logic to Hardware
        update_fan_hardware();
        
        // 3. Update LCD
        // Line 1: Duty Cycle
        lcd_command(0x80); 
        sprintf(buffer, "DC=%d.0%%   ", counter);
        lcd_print(buffer);
        
        // Line 2: Status
        lcd_command(0xC0);
        if (fan_on) lcd_print("Fan:ON     ");
        else        lcd_print("Fan:OFF    ");
        
        _delay_ms(50); // Loop delay for UI stability
    }
}