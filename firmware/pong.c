// Pico Pong - Raspberry Pi Pico + SSD1331 OLED (96x64, SPI)
// Copyright (c) 2025-2026 Guillaume BORGET. All rights reserved.

#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

//pinout (see schematic)
#define PIN_BTN_LEFT  2
#define PIN_BTN_RIGHT 3
#define PIN_DC        16
#define PIN_CS        17
#define PIN_SCK       18
#define PIN_MOSI      19
#define PIN_RES       20
#define PIN_BUZZ      21

#define SCREEN_WIDTH  96
#define SCREEN_HEIGHT 64
#define SPI_PORT      spi0

//RGB565 colors
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F

//---------- SSD1331 ----------

void ssd1331_write_cmd(uint8_t cmd) {
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0);//DC low = command
    spi_write_blocking(SPI_PORT, &cmd, 1);
    gpio_put(PIN_CS, 1);
}

void ssd1331_write_data(uint8_t data) {
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 1);//DC high = data
    spi_write_blocking(SPI_PORT, &data, 1);
    gpio_put(PIN_CS, 1);
}

void ssd1331_init(void) {
    //hardware reset
    gpio_put(PIN_RES, 0);
    sleep_ms(100);
    gpio_put(PIN_RES, 1);
    sleep_ms(100);

    //init sequence from the SSD1331 datasheet
    ssd1331_write_cmd(0xAE); //display off
    ssd1331_write_cmd(0xA0); //remap
    ssd1331_write_cmd(0x72); //RGB
    ssd1331_write_cmd(0xA1); //start line
    ssd1331_write_cmd(0x00);
    ssd1331_write_cmd(0xA2); //display offset
    ssd1331_write_cmd(0x00);
    ssd1331_write_cmd(0xA4); //normal display
    ssd1331_write_cmd(0xA8); //multiplex
    ssd1331_write_cmd(0x3F); //1/64 duty
    ssd1331_write_cmd(0xAD); //master config
    ssd1331_write_cmd(0x8E); //external VCC
    ssd1331_write_cmd(0xB0); //power save
    ssd1331_write_cmd(0x0B); //disabled
    ssd1331_write_cmd(0xB1); //phase period
    ssd1331_write_cmd(0x31);
    ssd1331_write_cmd(0xB3); //clock divider
    ssd1331_write_cmd(0xF0);
    ssd1331_write_cmd(0x8A); //precharge A
    ssd1331_write_cmd(0x64);
    ssd1331_write_cmd(0x8B); //precharge B
    ssd1331_write_cmd(0x78);
    ssd1331_write_cmd(0x8C); //precharge C
    ssd1331_write_cmd(0x64);
    ssd1331_write_cmd(0xBE); //VCOMH
    ssd1331_write_cmd(0x3E);
    ssd1331_write_cmd(0x87); //master current
    ssd1331_write_cmd(0x06);
    ssd1331_write_cmd(0x81); //contrast A
    ssd1331_write_cmd(0x91);
    ssd1331_write_cmd(0x82); //contrast B
    ssd1331_write_cmd(0x50);
    ssd1331_write_cmd(0x83); //contrast C
    ssd1331_write_cmd(0x7D);
    ssd1331_write_cmd(0xAF); //display on
}

//clear the whole screen with the "clear window" command
void ssd1331_clear(void) {
    ssd1331_write_cmd(0x25);
    ssd1331_write_cmd(0x00);
    ssd1331_write_cmd(0x00);
    ssd1331_write_cmd(SCREEN_WIDTH - 1);
    ssd1331_write_cmd(SCREEN_HEIGHT - 1);
    sleep_ms(1);//the controller needs time, next command cancels the clear otherwise
}

void draw_pixel(uint8_t x, uint8_t y, uint16_t color) {
    if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) return;
    ssd1331_write_cmd(0x15); //column
    ssd1331_write_cmd(x);
    ssd1331_write_cmd(SCREEN_WIDTH - 1);
    ssd1331_write_cmd(0x75); //row
    ssd1331_write_cmd(y);
    ssd1331_write_cmd(SCREEN_HEIGHT - 1);

    ssd1331_write_data(color >> 8);
    ssd1331_write_data(color & 0xFF);
}

//filled rectangle drawn by the controller (paddle and ball)
void draw_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t color) {
    ssd1331_write_cmd(0x26); //fill enable
    ssd1331_write_cmd(0x01);

    ssd1331_write_cmd(0x22); //draw rectangle
    ssd1331_write_cmd(x);
    ssd1331_write_cmd(y);
    ssd1331_write_cmd(x + w - 1);
    ssd1331_write_cmd(y + h - 1);

    //outline color, RGB565 -> 6 bits per channel
    ssd1331_write_cmd((color >> 11) << 1);
    ssd1331_write_cmd((color >> 5) & 0x3F);
    ssd1331_write_cmd((color & 0x1F) << 1);

    //fill color
    ssd1331_write_cmd((color >> 11) << 1);
    ssd1331_write_cmd((color >> 5) & 0x3F);
    ssd1331_write_cmd((color & 0x1F) << 1);

    sleep_us(100);
}

//---------- buzzer ----------

//PWM square wave on the piezo (driven by Q1)
void beep(uint freq_hz, uint duration_ms) {
    uint slice_num = pwm_gpio_to_slice_num(PIN_BUZZ);
    uint32_t clock = 125000000;//default sys clock
    uint32_t div = 100;
    uint32_t wrap = clock / div / freq_hz;

    pwm_set_clkdiv(slice_num, (float)div);
    pwm_set_wrap(slice_num, wrap);
    pwm_set_chan_level(slice_num, pwm_gpio_to_channel(PIN_BUZZ), wrap / 2);//50% duty

    pwm_set_enabled(slice_num, true);
    sleep_ms(duration_ms);
    pwm_set_enabled(slice_num, false);
}

//---------- game ----------

int main() {
    stdio_init_all();

    //SPI at 20MHz
    spi_init(SPI_PORT, 20000000);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    gpio_init(PIN_DC);
    gpio_set_dir(PIN_DC, GPIO_OUT);

    gpio_init(PIN_RES);
    gpio_set_dir(PIN_RES, GPIO_OUT);

    //buttons are active low (pull-up)
    gpio_init(PIN_BTN_LEFT);
    gpio_set_dir(PIN_BTN_LEFT, GPIO_IN);
    gpio_pull_up(PIN_BTN_LEFT);

    gpio_init(PIN_BTN_RIGHT);
    gpio_set_dir(PIN_BTN_RIGHT, GPIO_IN);
    gpio_pull_up(PIN_BTN_RIGHT);

    gpio_set_function(PIN_BUZZ, GPIO_FUNC_PWM);
    gpio_set_drive_strength(PIN_BUZZ, GPIO_DRIVE_STRENGTH_12MA);

    ssd1331_init();
    ssd1331_clear();

    int paddle_w = 20;
    int paddle_h = 3;
    int paddle_x = (SCREEN_WIDTH - paddle_w) / 2;
    int paddle_y = SCREEN_HEIGHT - 5;

    float ball_x = SCREEN_WIDTH / 2;
    float ball_y = SCREEN_HEIGHT / 2;
    float ball_dx = 1.0;
    float ball_dy = -1.0;
    int ball_size = 3;

    int score = 0;

    while (1) {
        //inputs
        if (!gpio_get(PIN_BTN_LEFT)) {
            paddle_x -= 3;
            if (paddle_x < 0) paddle_x = 0;
        }
        if (!gpio_get(PIN_BTN_RIGHT)) {
            paddle_x += 3;
            if (paddle_x > SCREEN_WIDTH - paddle_w) paddle_x = SCREEN_WIDTH - paddle_w;
        }

        //erase the ball at its old position
        draw_rect((int)ball_x, (int)ball_y, ball_size, ball_size, BLACK);

        ball_x += ball_dx;
        ball_y += ball_dy;

        //side walls
        if (ball_x <= 0 || ball_x >= SCREEN_WIDTH - ball_size) {
            ball_dx = -ball_dx;
            beep(4000, 20);
            //push the ball back inside so it doesn't get stuck in the wall
            if (ball_x <= 0) ball_x = 1;
            else ball_x = SCREEN_WIDTH - ball_size - 1;
        }

        //top wall
        if (ball_y <= 0) {
            ball_dy = -ball_dy;
            beep(4000, 20);
        }

        //paddle
        if (ball_y >= paddle_y - ball_size &&
            ball_x + ball_size >= paddle_x &&
            ball_x <= paddle_x + paddle_w) {

            ball_dy = -ball_dy;
            score++;
            beep(4000, 20);

            //a bit faster every 5 points
            if (score % 5 == 0) {
                if (ball_dx > 0) ball_dx += 0.2; else ball_dx -= 0.2;
                if (ball_dy > 0) ball_dy += 0.2; else ball_dy -= 0.2;
            }
        }

        //missed -> game over
        if (ball_y >= SCREEN_HEIGHT) {
            beep(4000, 1000);
            sleep_ms(500);

            ball_x = SCREEN_WIDTH / 2;
            ball_y = SCREEN_HEIGHT / 2;
            ball_dx = 1.0;
            ball_dy = -1.0;
            score = 0;
            ssd1331_clear();
        }

        //redraw paddle line then paddle
        draw_rect(0, paddle_y, SCREEN_WIDTH, paddle_h, BLACK);
        draw_rect(paddle_x, paddle_y, paddle_w, paddle_h, BLUE);

        draw_rect((int)ball_x, (int)ball_y, ball_size, ball_size, WHITE);

        sleep_ms(20);//game speed
    }
}
