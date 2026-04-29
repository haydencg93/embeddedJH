#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>

// ---------------- LCD ----------------
#define RS PB4
#define E  PB3
#define D4 PD6
#define D5 PD7
#define D6 PB0
#define D7 PB5

// ---------------- OTHER ----------------
#define BUTTON PD4
#define FAN_PWM PD5
#define RED_LED PB1
#define BLUE_LED PB2
#define DHT_PIN PD2

// ---------------- GLOBALS ----------------
uint8_t systemOn = 0;
uint8_t thresholdF = 75; // default threshold (°F)

// ---------------- LCD ----------------
void lcd_pulse() {
	PORTB |= (1 << E);
	_delay_us(1);
	PORTB &= ~(1 << E);
	_delay_us(100);
}

void lcd_send4(uint8_t data) {
	(data & 1) ? (PORTD |= (1 << D4)) : (PORTD &= ~(1 << D4));
	(data & 2) ? (PORTD |= (1 << D5)) : (PORTD &= ~(1 << D5));
	(data & 4) ? (PORTB |= (1 << D6)) : (PORTB &= ~(1 << D6));
	(data & 8) ? (PORTB |= (1 << D7)) : (PORTB &= ~(1 << D7));
	lcd_pulse();
}

void lcd_cmd(uint8_t cmd) {
	PORTB &= ~(1 << RS);
	lcd_send4(cmd >> 4);
	lcd_send4(cmd & 0x0F);
	_delay_ms(2);
}

void lcd_data(uint8_t data) {
	PORTB |= (1 << RS);
	lcd_send4(data >> 4);
	lcd_send4(data & 0x0F);
	_delay_ms(2);
}

void lcd_print(char *str) {
	while (*str) lcd_data(*str++);
}

void lcd_init() {
	DDRB |= (1<<RS)|(1<<E)|(1<<PB0)|(1<<PB5);
	DDRD |= (1<<PD6)|(1<<PD7);

	_delay_ms(50);

	lcd_send4(0x03);
	lcd_send4(0x03);
	lcd_send4(0x03);
	lcd_send4(0x02);

	lcd_cmd(0x28);
	lcd_cmd(0x0C);
	lcd_cmd(0x06);
	lcd_cmd(0x01);
}

// ---------------- ADC ----------------
void adc_init() {
	ADMUX = (1 << REFS0); // AVcc reference
	ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1);
}

uint16_t adc_read() {
	ADMUX = (1 << REFS0) | 0;  // A0

	ADCSRA |= (1 << ADSC);
	while (ADCSRA & (1 << ADSC));

	return ADC;
}

// ---------------- PWM ----------------
void pwm_init() {
	DDRD |= (1 << FAN_PWM);

	TCCR0A = (1 << COM0B1) | (1 << WGM00) | (1 << WGM01);
	TCCR0B = (1 << CS01);
}

void pwm_set(uint8_t val) {
	OCR0B = val;
}

// ---------------- BUTTON ----------------
uint8_t button_pressed() {
	return !(PIND & (1 << BUTTON));
}

// ---------------- DHT11 ----------------
uint8_t dht_read(uint8_t *tempC) {
	DDRD |= (1 << DHT_PIN);
	PORTD &= ~(1 << DHT_PIN);
	_delay_ms(20);

	PORTD |= (1 << DHT_PIN);
	DDRD &= ~(1 << DHT_PIN);

	_delay_us(40);

	if (PIND & (1 << DHT_PIN)) return 0;

	while (!(PIND & (1 << DHT_PIN)));
	while (PIND & (1 << DHT_PIN));

	uint8_t data[5] = {0};

	for (uint8_t i = 0; i < 40; i++) {
		while (!(PIND & (1 << DHT_PIN)));
		_delay_us(30);

		if (PIND & (1 << DHT_PIN)) {
			data[i/8] |= (1 << (7 - (i%8)));
			while (PIND & (1 << DHT_PIN));
		}
	}

	*tempC = data[2];
	return 1;
}

// ---------------- MAIN ----------------
int main(void) {

	// IO setup
	DDRB |= (1 << RED_LED) | (1 << BLUE_LED);
	DDRD &= ~(1 << BUTTON);
	PORTD |= (1 << BUTTON); // pull-up

	lcd_init();
	adc_init();
	pwm_init();

	while (1) {

		// ----- BUTTON: ON/OFF ONLY -----
		if (button_pressed()) {
			_delay_ms(20);
			if (button_pressed()) {
				systemOn ^= 1;
				while (button_pressed());
			}
		}

		// ----- READ TEMP -----
		uint8_t tempC;
		dht_read(&tempC);

		// Convert to Fahrenheit
		uint8_t tempF = (tempC * 9 / 5) + 32;

		// ----- READ POT (ONLY WHEN SYSTEM ON) -----
		if (systemOn) {
			uint16_t pot = adc_read();
			thresholdF = 60 + (pot * 40UL / 1023);
		}

		// ----- DISPLAY -----
		lcd_cmd(0x80);
		lcd_print("T:");

		lcd_data((tempF / 100) ? (tempF / 100 + '0') : ' ');
		lcd_data(((tempF / 10) % 10) + '0');
		lcd_data((tempF % 10) + '0');

		lcd_data('F');

		lcd_print(" Th:");

		lcd_data((thresholdF / 100) ? (thresholdF / 100 + '0') : ' ');
		lcd_data(((thresholdF / 10) % 10) + '0');
		lcd_data((thresholdF % 10) + '0');

		lcd_cmd(0xC0);

		if (!systemOn) {
			lcd_print("SYSTEM OFF   ");
			pwm_set(0);
			PORTB &= ~(1 << RED_LED);
			PORTB &= ~(1 << BLUE_LED);
			_delay_ms(300);
			continue;
		}

		// ----- CONTROL -----
		if (tempF > thresholdF) {
			PORTB |= (1 << RED_LED);
			PORTB &= ~(1 << BLUE_LED);
			pwm_set(200);
			lcd_print("FAN ON       ");
			} else {
			PORTB &= ~(1 << RED_LED);
			PORTB |= (1 << BLUE_LED);
			pwm_set(0);
			lcd_print("FAN OFF      ");
		}

		_delay_ms(300);
	}
}
