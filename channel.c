#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channel.h"

void channel_init(channel *c) {
  int i;

  c->raw = NULL;
  c->size = 0;

  c->wedge_line_mean = NULL;
  c->wedge_line_stddev = NULL;

  for (i = 0; i < 16; i++) {
    c->wedge[i] = 0;
    c->wedge_stddev[i] = 0;
  }
}

uint16_t *channel_alloc_line(channel *c) {
  uint32_t size = c->size;
  c->size += CHANNEL_WORDS;
  c->raw = realloc(c->raw, c->size * sizeof(uint16_t));
  memset(c->raw + size, 0, CHANNEL_WORDS * sizeof(uint16_t));
  return c->raw + size;
}

int channel_to_pgm(channel *c, FILE *f) {
  int width = CHANNEL_WORDS;
  int height = c->size / width;
  int i;
  int j;

  // TODO convert to bmp since pbm is inefficient, though pbm is easy.
  // P2 is for grayscale PBM via ASCII
  fprintf(f, "P2 %d %d %d\n", width, height, 65535);

  // My examples were ending up upside-down. I don't know if that was due to my particular samples.
  // I'm guessing my datasets just all happen to be south-to-north traversals.
  for (i = height-1; i >=0 ; i--) {
    for (j = width-1; j >= 0; j--) {
      fprintf(f, "%d ", c->raw[i * width + j]);
    }

    fprintf(f, "\n");
  }

  return 0;
}

// False color function
// Including the IR channel allows for better cloud distinction
// WAV pulled from http://www.fredvandenbosch.nl/satellites_WAV.html worked well
int channel_to_pgm_fc(channel *c, channel *ir, FILE *f) {
  int width = CHANNEL_WORDS;
  int height = c->size / width;
  int i;
  int j;

  int val;
  int irval;
  int r;
  int g;
  int b;

  // P3 is for RGB PBM via ASCII
  fprintf(f, "P3 %d %d %d\n", width, height, 65535);

  for (i = height-1; i >=0 ; i--) {
    for (j = width-1; j >= 0; j--) {
      val = c->raw[i * width + j];
      irval = ir->raw[i * width + j];

      // False color calc
      // These values were based off trial and error, not off any particular standard or resource.
      // TODO improve colors and/or make the limits adjustable from command line

      // Water identification
      if ( val < 13000 ) {
        r = (8*256) + val * .2;
        g = (20*256) + val * 1;
        b = (50*256) + val * .75;
      }
      // Cloud/snow/ice identification
      // IR channel helps distinguish clouds and water, particularly in arctic areas
      else if ( irval > 35000 ) {
        r = (irval+val)/2; // Average the two for a little better cloud distinction
        g = r;
        b = r;
      }
      // Vegetation identification
      else if ( val < 27000 ) { // green
        r = val * .8;
        g = val * .9;
        b = val * .6;
      }
      // Desert/dirt identification
      else if ( val <= 35000) { // brown
        r = val * 1;
        g = val * .9;
        b = val * .7;
      }
      // Everything else, but this was probably captured by the IR channel above
      else { // Clouds, snow, and really dry desert
        r = val;
        g = val;
        b = val;
      }

      if (j < SPACE_WORDS || j >= SPACE_WORDS + CHANNEL_DATA_WORDS) {
        r = 0;
        g = 0;
        b = 0;
      }

      fprintf(f, "%d %d %d ", r,g,b);

    }

    fprintf(f, "\n");
  }

  return 0;
}

void channel_compute_wedge_stats(channel *c) {
  const int N = TELEMETRY_WEDGE_WORDS;
  uint16_t width = CHANNEL_WORDS;
  uint16_t height = c->size / width;
  uint16_t i;
  uint16_t j;
  uint32_t v;
  uint64_t sums[8];
  uint64_t sq_sums[8];
  uint64_t sum = 0;
  uint64_t sq_sum = 0;
  uint64_t stddev;

  // Initialize arrays
  for (i = 0; i < 8; i++) {
    sums[i] = 0;
    sq_sums[i] = 0;
  }

  // Allocate space for wedge mean and stddev
  c->wedge_line_mean = calloc(height, sizeof(c->wedge_line_mean[0]));
  c->wedge_line_stddev = calloc(height, sizeof(c->wedge_line_stddev[0]));

  for (i = 0; i < height; i++) {
    // Subtract line from accumulators
    sum -= sums[i & 0x7];
    sq_sum -= sq_sums[i & 0x7];

    // Compute sum/squared sum for this line
    sums[i & 0x7] = 0;
    sq_sums[i & 0x7] = 0;
    for (j = CHANNEL_WORDS - TELEMETRY_WORDS; j < CHANNEL_WORDS; j++) {
      v = c->raw[i * width + j];
      sums[i & 0x7] += v;
      sq_sums[i & 0x7] += v*v;
    }

    // Add line to accumulators
    sum += sums[i & 0x7];
    sq_sum += sq_sums[i & 0x7];

    // Compute stddev
    stddev = sqrt((sq_sum - (sum * sum) / N) / N);

    // Store measurements
    c->wedge_line_mean[i] = sum / N;
    c->wedge_line_stddev[i] = stddev;
  }
}

int channel_find_frame_offset(channel *c) {
  uint16_t width = CHANNEL_WORDS;
  uint16_t height = c->size / width;
  uint16_t *mean = c->wedge_line_mean;
  uint16_t *stddev = c->wedge_line_stddev;
  uint16_t i;
  uint16_t j;

  for (i = 0, j = 0; i < height;) {
    // Check if minimum wrt previous line (only if not first wedge)
    if (i > 0 && j > 0 && stddev[i-1] < stddev[i]) {
      // Not a minimum; reset
      j = 0;
      i++;
      continue;
    }

    // Check if minimum wrt next line (only if not last wedge)
    if (i < (height - 1) && j < 7 && stddev[i+1] < stddev[i]) {
      // Not a minimum; reset
      j = 0;
      i++;
      continue;
    }

    // Check for brightness increase (only if not first wedge)
    if (j > 0 && mean[i] < mean[i-8]) {
      // No increasing brightness; reset j (still a minimum)
      j = 0;
      continue;
    }

    j++;
    i += 8;
    if (j == 8) {
      // Found 8 consecutive wedges of increasing brightness!
      return i-64;
    }
  }

  return -1;
}

int channel_detect_telemetry(channel *c) {
  int offset;
  int i;

  channel_compute_wedge_stats(c);

  offset = channel_find_frame_offset(c);
  if (offset < 0) {
    return -1;
  }

  for (i = 0; i < 16; i++) {
    c->wedge[i] = c->wedge_line_mean[offset+i*8];
    c->wedge_stddev[i] = c->wedge_line_stddev[offset+i*8];
  }

  return 0;
}

int channel_normalize(channel *c) {
  uint16_t width = CHANNEL_WORDS;
  uint16_t height = c->size / width;
  uint16_t low;
  uint16_t high;
  uint16_t i;
  uint16_t j;
  int32_t v;

  if (c->wedge[0] == 0) {
    abort();
  }

  // Limits - These are the lowest and highest values returned in the wedges
  low = c->wedge[8] - c->wedge_stddev[8]; // Wedge 9
  high = c->wedge[7] + c->wedge_stddev[7]; // Wedge 8

  // Normalize every pixel
  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      v = (65535 * (c->raw[i * width + j] - low)) / (high - low);
      if (v < 0) {
        v = 0;
      }
      if (v > 65535) {
        v = 65535;
      }

      c->raw[i * width + j] = v;
    }
  }

  return 0;
}
