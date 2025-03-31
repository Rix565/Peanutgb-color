#include <eadk.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "peanut_gb/peanut_gb.h"
#include "lz4.h"
#include "storage.h"

// Game name is max 0x10 bytes (with null), and we need to add ".gbs"
#define FILENAME_BUFFER_SIZE 0x10 + 4

#define ENABLE_FRAME_LIMITER 1
#define TARGET_FRAME_DURATION 16
#define AUTOMATIC_FRAME_SKIPPING 1
// Useful when AUTOMATIC_FRAME_SKIPPING is disabled
#define FRAME_SKIPPING_DEFAULT_STATE false

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
eadk_color_t palette_virtual_boy[4] = {0xE800, 0xA000, 0x5000, eadk_color_black};
eadk_color_t * palette = palette_original;

inline eadk_color_t eadk_color_from_gb_pixel(uint8_t gb_pixel) {
    uint8_t gb_color = gb_pixel & 0x3;
    return palette[gb_color];
}

static void lcd_draw_line_centered(struct gb_s* gb, const uint8_t* input_pixels, const uint_fast8_t line) {
    eadk_color_t output_pixels[LCD_WIDTH];
    eadk_point_t point = { 0, line };

    //fix each pixel with gb->cgb.fixPalette
    #pragma unroll 40
    for (int i = 0; i < LCD_WIDTH; i++) {
        if (gb->cgb.cgbMode) {
            output_pixels[i] = gb->cgb.fixPalette[input_pixels[i]];
            output_pixels[i] = (output_pixels[i] & 0x1F) << 11 | (output_pixels[i] & 0x3E0) << 1 | (output_pixels[i] & 0x7C00) >> 10;
            output_pixels[i] = (output_pixels[i] & 0x1F) << 11 | (output_pixels[i] & 0x7E0) | (output_pixels[i] & 0xF800) >> 11;
        }
        else {
            output_pixels[i] = eadk_color_from_gb_pixel(input_pixels[i]);
        }
    }
    //dump output_pixels to screen
    eadk_display_push_rect((eadk_rect_t) { (EADK_SCREEN_WIDTH - LCD_WIDTH) / 2, (EADK_SCREEN_HEIGHT - LCD_HEIGHT) / 2 + line, LCD_WIDTH, 1 }, output_pixels);

}


void lcd_draw_line_dummy(struct gb_s *gb, const uint8_t pixels[LCD_WIDTH], const uint_fast8_t line) {}

static void lcd_draw_line_maximized_ratio(struct gb_s * gb, const uint8_t * input_pixels, const uint_fast8_t line) {
  // Nearest neighbor scaling of a 160x144 texture to a 266x240 resolution (to keep the ratio)
  // Horizontally, we multiply by 1.66 (160*1.66 = 266)
  eadk_color_t output_pixels[LCD_WIDTH];
  uint16_t final_output_pixels[266];

  #pragma unroll 40
  for (int i=0; i<LCD_WIDTH; i++) {
    if (gb->cgb.cgbMode) {
        output_pixels[i] = gb->cgb.fixPalette[input_pixels[i]];
        output_pixels[i] = (output_pixels[i] & 0x1F) << 11 | (output_pixels[i] & 0x3E0) << 1 | (output_pixels[i] & 0x7C00) >> 10;
        output_pixels[i] = (output_pixels[i] & 0x1F) << 11 | (output_pixels[i] & 0x7E0) | (output_pixels[i] & 0xF800) >> 11;
    }
    else {
        output_pixels[i] = eadk_color_from_gb_pixel(input_pixels[i]);
    }

    eadk_color_t color = output_pixels[i];
    // We can't use floats for performance reason, so we use a fixed point
    // representation
    final_output_pixels[166*i/100] = color;
    // This line is useless 1/3 times, but using an if is slower
    final_output_pixels[166*i/100+1] = color;
  }

  // Vertically, we want to scale by a 5/3 ratio. So we need to make 5 lines out of three:  we double two lines out of three.
  uint16_t y = (5*line)/3;
  eadk_display_push_rect((eadk_rect_t){(320 - 265) / 2, y, 265, 1}, final_output_pixels);
  if (line%3 != 0) {
    eadk_display_push_rect((eadk_rect_t){(320 - 265) / 2, y + 1, 265, 1}, final_output_pixels);
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
    // TODO: Backup the previous save to a mallocated buffer to restore it in
    // case the storage is too full for the new save to be written
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

void willExecuteDFU() { asm("svc 54"); }
void didExecuteDFU() { asm("svc 51"); }
void suspend() { asm("svc 44"); }

void pre_exit() {
  didExecuteDFU();
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
    pre_exit();
    return -1;
  }

  // Alloc and init save RAM.
  size_t save_size = gb_get_save_size(&gb);
  priv.cart_ram = read_save_file(save_size);

  gb_init_lcd(&gb, lcd_draw_line_maximized_ratio);

  bool MSpFfCounter = false;
  bool wasMSpFPressed = false;
  uint32_t lastMSpF = 0;

  #if ENABLE_FRAME_LIMITER
  // We use a "smart" frame limiter: for each frame, we add
  // `frame duration - target frame time` to our budget. If the frame was faster
  // than target, we sleep for (simplified version without taking the case where
  // time budget > target frame time - last frame duration):
  // target frame time - last frame duration - time budget
  // This way, we will keep an average frame duration consistant.
  uint32_t timeBudget = 0;
  #endif

  // Skip 1/2 frame, spare 3 ms/f on my N0110
  bool frameSkipping = FRAME_SKIPPING_DEFAULT_STATE;
  void * drawLineMode = lcd_draw_line_maximized_ratio;

  while (true) {
    uint64_t start = eadk_timing_millis();
    gb_run_frame(&gb);

    eadk_keyboard_state_t kbd = eadk_keyboard_scan();
    gb.direct.joypad_bits.a = !eadk_keyboard_key_down(kbd, eadk_key_back);
    gb.direct.joypad_bits.b = !eadk_keyboard_key_down(kbd, eadk_key_ok);
    gb.direct.joypad_bits.select = !(eadk_keyboard_key_down(kbd, eadk_key_shift) || eadk_keyboard_key_down(kbd, eadk_key_home));
    gb.direct.joypad_bits.start = !(eadk_keyboard_key_down(kbd, eadk_key_backspace) || eadk_keyboard_key_down(kbd, eadk_key_alpha) || eadk_keyboard_key_down(kbd, eadk_key_on_off));
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
    if (eadk_keyboard_key_down(kbd, eadk_key_five)) {
      palette = palette_virtual_boy;
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_plus)) {
      gb.display.lcd_draw_line = lcd_draw_line_maximized_ratio;
      drawLineMode = lcd_draw_line_maximized_ratio;
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_minus)) {
      eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
      gb.display.lcd_draw_line = lcd_draw_line_centered;
      drawLineMode = lcd_draw_line_centered;
    }
    // if (eadk_keyboard_key_down(kbd, eadk_key_division)) {
    //   eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
    //   gb.display.lcd_draw_line = lcd_draw_line_dummy;
    //   drawLineMode = lcd_draw_line_dummy;
    // }
    if (eadk_keyboard_key_down(kbd, eadk_key_toolbox)) {
      write_save_file(priv.cart_ram, save_size);
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_seven)) {
      if (!wasMSpFPressed) {
        MSpFfCounter = !MSpFfCounter;
        wasMSpFPressed = true;
        eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
      }
    } else {
      wasMSpFPressed = false;
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_nine)) {
      // willExecuteDFU disable interrupts, CircuitBreaker and the keyboard, so
      // to get the keyboard back, we need to suspend the calculator as the
      // calculator will enable back the keyboard when going out of sleep,
      // without restoring circuitBreaker, effectively bypassing Home/OnOff
      // management by the kernel
      // It also have the nice side effect of allowing to suspend the calculator
      // without exiting, which is nice as savestates are not implemented
      willExecuteDFU();
      suspend();

      // Clear the screen as framebuffer is lost when the screen is shut down
      eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
    }
    if (eadk_keyboard_key_down(kbd, eadk_key_zero)) {
      // Save and exit
      // TODO: free buffers as we don't need them anymore and saving require a
      // bit of memory
      // In case of OOM, save won't be written
      write_save_file(priv.cart_ram, save_size);
      pre_exit();
      return 0;
    }

    uint64_t end = eadk_timing_millis();
    uint16_t MSpF = (uint16_t)(end - start);
    if (MSpFfCounter) {
      // We need to average the MSpF as skipped frames are faster
      uint16_t MSpFAverage = (MSpF + lastMSpF) / 2;
      char buffer[100];
      sprintf(buffer, "%d ms/f", MSpFAverage);
      // sprintf(buffer, "%d ms/f, %d ", MSpFAverage, timeBudget);
      eadk_point_t location = {2, 230};
      eadk_display_draw_string(buffer, location, false, eadk_color_white, eadk_color_black);
    }

    if (frameSkipping) {
      if (gb.display.lcd_draw_line != lcd_draw_line_dummy) {
        drawLineMode = gb.display.lcd_draw_line;
        gb.display.lcd_draw_line = lcd_draw_line_dummy;
      } else {
        gb.display.lcd_draw_line = drawLineMode;
      }
    }

    #if ENABLE_FRAME_LIMITER
    uint32_t differenceToTarget = abs(TARGET_FRAME_DURATION - MSpF);

    if (TARGET_FRAME_DURATION - MSpF > 0) {
      // Frame was faster than target, so let's slow down if we have time to
      // catch up

      // If on previous frames we were
      if (timeBudget >= differenceToTarget) {
        // We were too slow at previous frames so we have to catch up
        timeBudget -= differenceToTarget;
      } else if (timeBudget > 0) {
        // We can catch up everything on one frame, so let's sleep a bit less
        // than what we would have done if we weren't late
        uint32_t time_to_sleep = differenceToTarget - timeBudget;
        eadk_timing_msleep(time_to_sleep);
        timeBudget = 0;
      } else {
        // We don't have time to catch up, so we just sleep until we get to 16ms/f
        eadk_timing_msleep(differenceToTarget);

        #if AUTOMATIC_FRAME_SKIPPING
        // Disable frame skipping as we are running faster than required
        frameSkipping = false;
        gb.display.lcd_draw_line = drawLineMode;
        #endif
      }
    } else {
      // Comparaison is technically not required, but we do this avoid the
      // performance cost of duplicate assignation when we are at the maximum
      // time budget, which is often the case when lagging
      if (timeBudget < TARGET_FRAME_DURATION) {
        // Frame was slower than target, so we need to catch up.
        timeBudget += differenceToTarget;

        if (timeBudget >= TARGET_FRAME_DURATION) {
          timeBudget = TARGET_FRAME_DURATION;

          #if AUTOMATIC_FRAME_SKIPPING
          // Enable frame skipping in an attempt to speed up emulation
          frameSkipping = true;
          #endif
        }
      }
    }
    #endif

    lastMSpF = MSpF;
  }

  pre_exit();

  return 0;
}
