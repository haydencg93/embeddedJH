#define F_CPU 16000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdbool.h>
#include <stdio.h>

/* === PIN CONNECTIONS ===
LCD:
RS -> A0
E -> A1
D4 -> A2
D5 -> A3
D6 -> A4
D7 -> A5

PUSH BUTTON -> 2

RPG:
A -> ~9
B -> 8

FAN:
Power -> VIN
GND -> GND
PWM -> ~5
TACH -> ~3

/* ================= DEBOUNCE ================= */

#define BTN_DEBOUNCE_MS 40
#define RPG_DEBOUNCE_MS 2

volatile uint32_t ms_ticks = 0;

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
#define LCD_D4 PC2
#define LCD_D5 PC3
#define LCD_D6 PC4
#define LCD_D7 PC5

void lcd_pulse_enable(void)
{
	LCD_PORT |= (1 << LCD_E);
	_delay_us(1);
	LCD_PORT &= ~(1 << LCD_E);
	_delay_us(100);
}

void lcd_send_nibble(uint8_t nibble)
{
	LCD_PORT &= ~((1<<LCD_D4)|(1<<LCD_D5)|(1<<LCD_D6)|(1<<LCD_D7));
	if (nibble & 0x01) LCD_PORT |= (1 << LCD_D4);
	if (nibble & 0x02) LCD_PORT |= (1 << LCD_D5);
	if (nibble & 0x04) LCD_PORT |= (1 << LCD_D6);
	if (nibble & 0x08) LCD_PORT |= (1 << LCD_D7);
	lcd_pulse_enable();
}

void lcd_cmd(uint8_t cmd)
{
	LCD_PORT &= ~(1 << LCD_RS);
	lcd_send_nibble(cmd >> 4);
	lcd_send_nibble(cmd & 0x0F);
	_delay_ms(2);
}

void lcd_data(uint8_t data)
{
	LCD_PORT |= (1 << LCD_RS);
	lcd_send_nibble(data >> 4);
	lcd_send_nibble(data & 0x0F);
	_delay_ms(2);
}

void lcd_init(void)
{
	LCD_DDR |= (1<<LCD_RS)|(1<<LCD_E)|(1<<LCD_D4)|(1<<LCD_D5)|(1<<LCD_D6)|(1<<LCD_D7);
	_delay_ms(50);

	lcd_send_nibble(0x03);
	_delay_ms(5);
	lcd_send_nibble(0x03);
	_delay_us(150);
	lcd_send_nibble(0x03);
	lcd_send_nibble(0x02);

	lcd_cmd(0x28);
	lcd_cmd(0x0C);
	lcd_cmd(0x06);
	lcd_cmd(0x01);
}

void lcd_clear(void)
{
	lcd_cmd(0x01);
}

void lcd_goto(uint8_t col, uint8_t row)
{
	lcd_cmd(0x80 + (row ? 0x40 : 0x00) + col);
}

void lcd_print(const char *s)
{
	while (*s) lcd_data(*s++);
}

/* ================= GLOBAL STATE ================= */

volatile int16_t counter = 0;
volatile bool btn_event = false;
volatile bool rpg_event = false;
volatile bool rpg_cw = false;

volatile uint32_t last_btn_time = 0;
volatile uint32_t last_rpg_time = 0;

/* ================= INTERRUPTS ================= */

/* Pushbutton INT0 with debounce */
ISR(INT0_vect)
{
	uint32_t now = millis();
	if (now - last_btn_time >= BTN_DEBOUNCE_MS)
	{
		btn_event = true;
		last_btn_time = now;
	}
}

/* RPG: debounced + 1 count per detent */
ISR(PCINT0_vect)
{
	static uint8_t last_state = 0;
	uint8_t curr_state = PINB & 0x03;

	uint32_t now = millis();
	if (now - last_rpg_time < RPG_DEBOUNCE_MS)
	return;

	/* Count only at stable detent state (00) */
	if (curr_state == 0x00 && last_state != 0x00)
	{
		if (last_state == 0x01)
		{
			counter++;
			rpg_cw = true;
			rpg_event = true;
		}
		else if (last_state == 0x02)
		{
			counter--;
			rpg_cw = false;
			rpg_event = true;
		}

		last_rpg_time = now;
	}

	last_state = curr_state;
}

/* ================= INIT ================= */

void timer0_init(void)
{
	TCCR0A = (1 << WGM01);
	OCR0A  = 249;                 // 1 ms at 16 MHz / 64
	TCCR0B = (1 << CS01) | (1 << CS00);
	TIMSK0 = (1 << OCIE0A);
}

void pushbutton_init(void)
{
	DDRD &= ~(1 << PD2);
	PORTD |= (1 << PD2);

	EICRA |= (1 << ISC01);
	EIMSK |= (1 << INT0);
}

void rpg_init(void)
{
	DDRB &= ~((1<<PB0)|(1<<PB1));
	PORTB |= (1<<PB0)|(1<<PB1);

	PCICR |= (1 << PCIE0);
	PCMSK0 |= (1 << PCINT0) | (1 << PCINT1);
}

/* ================= MAIN ================= */

int main(void)
{
	char buf[17];

	cli();
	timer0_init();
	lcd_init();
	pushbutton_init();
	rpg_init();
	sei();

	lcd_print("I/O Test Ready");

	while (1)
	{
		if (btn_event)
		{
			lcd_clear();
			lcd_print("Button Pressed");
			btn_event = false;
		}

		if (rpg_event)
		{
			lcd_clear();
			snprintf(buf, 16, "Counter: %d", counter);
			lcd_print(buf);

			lcd_goto(0,1);
			lcd_print(rpg_cw ? "RPG: CW" : "RPG: CCW");

			rpg_event = false;
		}
	}
}
