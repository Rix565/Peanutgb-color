#include <eadk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "peanut_gb/peanut_gb.h"
#include "lz4.h"
#include "storage.h"


// Game name is max 0x10 bytes (with null), and we need to add ".gbs"
#define FILENAME_BUFFER_SIZE 0x10 + 4

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "Game Boy";
const uint32_t eadk_api_level  __attribute__((section(".rodata.eadk_api_level"))) = 0;

struct gb_s gb;

struct priv_t {
  // Pointer to allocated memory holding GB file.
  const uint8_t *rom;
  // Pointer to allocated memory holding save file.
  uint8_t *cart_ram;
  // Line buffer
  uint16_t line_buffer[LCD_WIDTH];
};

uint8_t gb_rom_read(struct gb_s * gb, const uint_fast32_t addr) {
  const struct priv_t * const p = gb->direct.priv;
  return p->rom[addr];
}

void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
  const struct priv_t * const p = gb->direct.priv;
  p->cart_ram[addr] = val;
}

uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
  const struct priv_t * const p = gb->direct.priv;
  return p->cart_ram[addr];
}

void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val) {
  //  printf("GB_ERROR %d %d %d\n", gb_err, GB_INVALID_WRITE, val);
  return;
}

eadk_color_t palette_peanut_GB[4] = {0x9DE1, 0x8D61, 0x3306, 0x09C1};
eadk_color_t palette_original[4] = {0x8F80, 0x24CC, 0x4402, 0x0A40};
eadk_color_t palette_gray[4] = {eadk_color_white, 0xAD55, 0x52AA, eadk_color_black};
eadk_color_t palette_gray_negative[4] = {eadk_color_black, 0x52AA, 0xAD55, eadk_color_white};
eadk_color_t * palette = palette_original;

eadk_color_t eadk_color_from_gb_pixel(uint8_t gb_pixel) {
    uint8_t gb_color = gb_pixel & 0x3;
    return palette[gb_color];
}

static void lcd_draw_line_centered(struct gb_s * gb, const uint8_t * input_pixels, const uint_fast8_t line) {
  eadk_color_t output_pixels[LCD_WIDTH];
  for (int i=0; i<LCD_WIDTH; i++) {
    output_pixels[i] = eadk_color_from_gb_pixel(input_pixels[i]);
  }
  eadk_display_push_rect((eadk_rect_t){(EADK_SCREEN_WIDTH-LCD_WIDTH)/2, (EADK_SCREEN_HEIGHT-LCD_HEIGHT)/2+line, LCD_WIDTH, 1}, output_pixels);
}

static void lcd_draw_line_maximized(struct gb_s * gb, const uint8_t * input_pixels, const uint_fast8_t line) {
  // Nearest neighbor scaling of a 160x144 texture to a 320x240 resolution
  // Horizontally, we just double
  eadk_color_t output_pixels[2*LCD_WIDTH];
  for (int i=0; i<LCD_WIDTH; i++) {
    eadk_color_t color = eadk_color_from_gb_pixel(input_pixels[i]);
    output_pixels[2*i] = color;
    output_pixels[2*i+1] = color;
  }
  // Vertically, we want to scale by a 5/3 ratio. So we need to make 5 lines out of three:  we double two lines out of three.
  uint16_t y = (5*line)/3;
  eadk_display_push_rect((eadk_rect_t){0, y, 2*LCD_WIDTH, 1}, output_pixels);
  if (line%3 != 0) {
    eadk_display_push_rect((eadk_rect_t){0, y+1, 2*LCD_WIDTH, 1}, output_pixels);
  }
}

static void lcd_draw_line_maximized_ratio(struct gb_s * gb, const uint8_t * input_pixels, const uint_fast8_t line) {
  // Nearest neighbor scaling of a 160x144 texture to a 266x240 resolution (to keep the ratio)
  // Horizontally, we multiply by 1.66 (160*1.66 = 266)
  uint16_t output_pixels[266];
  for (int i=0; i<LCD_WIDTH; i++) {
    uint16_t color = eadk_color_from_gb_pixel(input_pixels[i]);
    // We can't use floats, so we use a fixed point representation
    output_pixels[166*i/100] = color;
    output_pixels[166*i/100+1] = color;
    output_pixels[166*i/100+2] = color;
  }

  // Vertically, we want to scale by a 5/3 ratio. So we need to make 5 lines out of three:  we double two lines out of three.
  uint16_t y = (5*line)/3;
  eadk_display_push_rect((eadk_rect_t){(320 - 266) / 2, y, 266, 1}, output_pixels);
  if (line%3 != 0) {
    eadk_display_push_rect((eadk_rect_t){(320 - 266) / 2, y + 1, 266, 1}, output_pixels);
  }
}

enum save_status_e {
  SAVE_READ_OK,
  SAVE_WRITE_OK,
  SAVE_READ_ERR,
  SAVE_WRITE_ERR,
  SAVE_COMPRESS_ERR,
  SAVE_NODISP
};
static enum save_status_e saveMessage = SAVE_NODISP;

void get_save_file_name(char * filename_buffer) {
  const char * rom = eadk_external_data;

  // We assume the buffer is safe
  // Fill the whole buffer with zeroes to be safe for the loop
  memset(filename_buffer, 0, FILENAME_BUFFER_SIZE);
  memcpy(filename_buffer, rom + 0x134, 0x10);

  char * end_of_rom_name = filename_buffer;
  for (; end_of_rom_name - filename_buffer <= 0x10; end_of_rom_name++) {
    if (*end_of_rom_name == '\0') {
      break;
    }
  }

  // Now, we can just add the extension
  sprintf(end_of_rom_name, ".gbs");
}

char* read_save_file(size_t size) {
  char save_name[FILENAME_BUFFER_SIZE];
  get_save_file_name(save_name);

  char* output = malloc(size);

  if (output == 0) {
    saveMessage = SAVE_READ_ERR;
    return NULL;
  }


  if (extapp_fileExists(save_name)) {
    size_t file_len = 0;
    const char* save_content = extapp_fileRead(save_name, &file_len);
    int error = LZ4_decompress_safe(save_content, output, file_len, size);

    // Handling corrupted save.
    if (error <= 0) {
      memset(output, 0xFF, size);
      extapp_fileErase(save_name);
      saveMessage = SAVE_READ_ERR;
    } else {
      saveMessage = SAVE_READ_OK;
    }
  } else {
    memset(output, 0xFF, size);
  }

  return output;
}

void write_save_file(char* data, size_t size) {
  char save_name[FILENAME_BUFFER_SIZE];
  get_save_file_name(save_name);

  // TODO: Avoid using a buffer (by streaming to the storage) or at least use a buffer
  // based on available memory instead of max.

  uint16_t max_scriptstore_size = extapp_size();

  char* output = malloc(max_scriptstore_size);
  // char* output = malloc(size);
  if (output == 0) {
    saveMessage = SAVE_WRITE_ERR;
    return;
  }

  int compressed_size = LZ4_compress_default(data, output, size, max_scriptstore_size);

  if (compressed_size > 0) {
    extapp_fileErase(save_name);
    if (extapp_fileWrite(save_name, output, compressed_size)) {
      saveMessage = SAVE_WRITE_OK;
    } else {
      saveMessage = SAVE_WRITE_ERR;
    }
  } else {
    saveMessage = SAVE_COMPRESS_ERR;
  }

  free(output);
}

int main(int argc, char * argv[]) {
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);

  struct priv_t priv = {
    .rom = NULL,
    .cart_ram = NULL
  };

  priv.rom = (const uint8_t*) eadk_external_data;

  int ret = gb_init(&gb, gb_rom_read, gb_cart_ram_read, gb_cart_ram_write, gb_error, &priv);
  if (ret != GB_INIT_NO_ERROR) {
    return -1;
  }

  // Alloc and init save RAM.
  size_t save_size = gb_get_save_size(&gb);
  priv.cart_ram = read_save_file(save_size);

  gb_init_lcd(&gb, lcd_draw_line_maximized_ratio);

  while (true) {
    gb_run_frame(&gb);

    eadk_keyboard_state_t kbd = eadk_keyboard_scan();
    gb.direct.joypad_bits.a = !eadk_keyboard_key_down(kbd, eadk_key_back);
    gb.direct.joypad_bits.b = !eadk_keyboard_key_down(kbd, eadk_key_ok);
    gb.direct.joypad_bits.select = !eadk_keyboard_key_down(kbd, eadk_key_shift);
    gb.direct.joypad_bits.start = !(eadk_keyboard_key_down(kbd, eadk_key_backspace) || eadk_keyboard_key_down(kbd, eadk_key_alpha));
    gb.direct.joypad_bits.right = !eadk_keyboard_key_down(kbd, eadk_key_right);
    gb.direct.joypad_bits.left = !eadk_keyboard_key_down(kbd, eadk_key_left);
    gb.direct.joypad_bits.up = !eadk_keyboard_key_down(kbd, eadk_key_up);
    gb.direct.joypad_bits.down = !eadk_keyboard_key_down(kbd, eadk_key_down);

    if (eadk_keyboard_key_down(kbd, eadk_key_one)) {
      palette = palette_original;
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_two)) {
      palette = palette_gray;
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_three)) {
      palette = palette_gray_negative;
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_four)) {
      palette = palette_peanut_GB;
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_plus)) {
      gb.display.lcd_draw_line = lcd_draw_line_maximized;
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_minus)) {
      eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
      gb.display.lcd_draw_line = lcd_draw_line_centered;
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_multiplication)) {
      eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
      gb.display.lcd_draw_line = lcd_draw_line_maximized_ratio;
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_toolbox)) {
      // We are not using the cooldown because it would slow down the emulation
      // for something useless (we are writing into ram, not flash)
      write_save_file(priv.cart_ram, save_size);
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_zero)) {
      // Save and exit
      write_save_file(priv.cart_ram, save_size);
      return 0;
    }
  }

  return 0;
}
