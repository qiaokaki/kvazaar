/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (C) 2013-2015 Tampere University of Technology and others (see
 * COPYING file).
 *
 * Kvazaar is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * Kvazaar is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Kvazaar.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

/*
 * \file
 *
 */

#ifdef _WIN32
/* The following two defines must be located before the inclusion of any system header files. */
#define WINVER       0x0500
#define _WIN32_WINNT 0x0500
#include <io.h>       /* _setmode() */
#include <fcntl.h>    /* _O_BINARY */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "checkpoint.h"
#include "global.h"
#include "config.h"
#include "threadqueue.h"
#include "encoder.h"
#include "encoderstate.h"
#include "image.h"
#include "cli.h"
#include "kvazaar.h"
#include "yuv_io.h"

/**
 * \brief Open a file for reading.
 *
 * If the file is "-", stdin is used.
 *
 * \param filename  name of the file to open or "-"
 * \return          the opened file or NULL if opening fails
 */
static FILE* open_input_file(const char* filename)
{
  if (!strcmp(filename, "-")) return stdin;
  return fopen(filename, "rb");
}

/**
 * \brief Open a file for writing.
 *
 * If the file is "-", stdout is used.
 *
 * \param filename  name of the file to open or "-"
 * \return          the opened file or NULL if opening fails
 */
static FILE* open_output_file(const char* filename)
{
  if (!strcmp(filename, "-")) return stdout;
  return fopen(filename, "wb");
}

/**
 * \brief Program main function.
 * \param argc Argument count from commandline
 * \param argv Argument list
 * \return Program exit state
 */
int main(int argc, char *argv[])
{
  int retval = EXIT_SUCCESS;

  config_t *cfg = NULL; //!< Global configuration
  kvz_encoder* enc = NULL;
  FILE *input  = NULL; //!< input file (YUV)
  FILE *output = NULL; //!< output file (HEVC NAL stream)
  FILE *recout = NULL; //!< reconstructed YUV output, --debug
  clock_t start_time = clock();
  clock_t encoding_start_cpu_time;
  CLOCK_T encoding_start_real_time;
  
  clock_t encoding_end_cpu_time;
  CLOCK_T encoding_end_real_time;

  // Stdin and stdout need to be binary for input and output to work.
  // Stderr needs to be text mode to convert \n to \r\n in Windows.
  #ifdef _WIN32
      _setmode( _fileno( stdin ),  _O_BINARY );
      _setmode( _fileno( stdout ), _O_BINARY );
      _setmode( _fileno( stderr ), _O_TEXT );
  #endif
      
  CHECKPOINTS_INIT();

  const kvz_api * const api = kvz_api_get(8);

  // Handle configuration
  cfg = api->config_alloc();

  // If problem with configuration, print banner and shutdown
  if (!cfg || !api->config_init(cfg) || !config_read(cfg,argc,argv)) {
    print_version();
    print_help();

    goto exit_failure;
  }

  input = open_input_file(cfg->input);
  if (input == NULL) {
    fprintf(stderr, "Could not open input file, shutting down!\n");
    goto exit_failure;
  }

  output = open_output_file(cfg->output);
  if (output == NULL) {
    fprintf(stderr, "Could not open output file, shutting down!\n");
    goto exit_failure;
  }

  if (cfg->debug != NULL) {
    recout = open_output_file(cfg->debug);
    if (recout == NULL) {
      fprintf(stderr, "Could not open reconstruction file (%s), shutting down!\n", cfg->debug);
      goto exit_failure;
    }
  }

  bitstream_t output_stream;
  if (!bitstream_init(&output_stream, BITSTREAM_TYPE_FILE)) {
    fprintf(stderr, "Could not initialize stream!\n");
    goto exit_failure;
  }
  output_stream.file.output = output;

  enc = api->encoder_open(cfg);
  if (!enc) {
    fprintf(stderr, "Failed to open encoder.\n");
    goto exit_failure;
  }

  encoder_control_t *encoder = enc->control;
  
  fprintf(stderr, "Input: %s, output: %s\n", cfg->input, cfg->output);
  fprintf(stderr, "  Video size: %dx%d (input=%dx%d)\n",
         encoder->in.width, encoder->in.height,
         encoder->in.real_width, encoder->in.real_height);

  if (cfg->seek > 0 && !yuv_io_seek(input, cfg->seek, cfg->width, cfg->height)) {
    fprintf(stderr, "Failed to seek %d frames.\n", cfg->seek);
    goto exit_failure;
  }

  //Now, do the real stuff
  {

    GET_TIME(&encoding_start_real_time);
    encoding_start_cpu_time = clock();

    uint64_t bitstream_length = 0;
    uint32_t frames_started = 0;
    uint32_t frames_done = 0;
    double psnr_sum[3] = { 0.0, 0.0, 0.0 };

    // Start coding cycle while data on input and not on the last frame
    while (cfg->frames == 0 || frames_started < cfg->frames) {
      encoder_state_t *state = &enc->states[enc->cur_state_num];

      frames_started += 1;

      
      image_t *img_in = image_alloc(state->tile->frame->width, state->tile->frame->height);
      if (!img_in) {
        fprintf(stderr, "Failed to allocate image.\n");
        goto exit_failure;
      }

      // Clear the encoder state.
      encoder_next_frame(state, img_in);

      // Read one frame from the input
      if (!read_one_frame(input, &enc->states[enc->cur_state_num], img_in)) {
        if (!feof(input))
          fprintf(stderr, "Failed to read a frame %d\n", enc->states[enc->cur_state_num].global->frame);
        image_free(img_in);
        break;
      }

      image_t *img_out = NULL;
      if (1 != api->encoder_encode(enc, img_in, &img_out, &output_stream)) {
        fprintf(stderr, "Failed to encode image.\n");
        image_free(img_in);
        goto exit_failure;
      }

      if (img_out != NULL) {
        state = &enc->states[enc->cur_state_num];
        double frame_psnr[3] = { 0.0, 0.0, 0.0 };
        encoder_compute_stats(state, recout, frame_psnr, &bitstream_length);
        
        frames_done += 1;
        psnr_sum[0] += frame_psnr[0];
        psnr_sum[1] += frame_psnr[1];
        psnr_sum[2] += frame_psnr[2];

        print_frame_info(state, frame_psnr);
      }

      image_free(img_in);
      image_free(img_out);
    }
    
    //Compute stats for the remaining encoders
    {
      image_t *img_out = NULL;
      while (1 == api->encoder_encode(enc, NULL, &img_out, &output_stream)) {
        if (img_out != NULL) {
          double frame_psnr[3] = { 0.0, 0.0, 0.0 };
          encoder_state_t *state = &enc->states[enc->cur_state_num];

          encoder_compute_stats(state, recout, frame_psnr, &bitstream_length);
          print_frame_info(state, frame_psnr);
          frames_done += 1;
          psnr_sum[0] += frame_psnr[0];
          psnr_sum[1] += frame_psnr[1];
          psnr_sum[2] += frame_psnr[2];

          image_free(img_out);
          img_out = NULL;
        }
      }
    }
    
    GET_TIME(&encoding_end_real_time);
    encoding_end_cpu_time = clock();
    
    threadqueue_flush(encoder->threadqueue);
    // Coding finished

    // Print statistics of the coding
    fprintf(stderr, " Processed %d frames, %10llu bits AVG PSNR: %2.4f %2.4f %2.4f\n", 
            frames_done, (long long unsigned int)bitstream_length * 8,
            psnr_sum[0] / frames_done, psnr_sum[1] / frames_done, psnr_sum[2] / frames_done);
    fprintf(stderr, " Total CPU time: %.3f s.\n", ((float)(clock() - start_time)) / CLOCKS_PER_SEC);
    
    {
      double encoding_time = ( (double)(encoding_end_cpu_time - encoding_start_cpu_time) ) / (double) CLOCKS_PER_SEC;
      double wall_time = CLOCK_T_AS_DOUBLE(encoding_end_real_time) - CLOCK_T_AS_DOUBLE(encoding_start_real_time);
      fprintf(stderr, " Encoding time: %.3f s.\n", encoding_time);
      fprintf(stderr, " Encoding wall time: %.3f s.\n", wall_time);
      fprintf(stderr, " Encoding CPU usage: %.2f%%\n", encoding_time/wall_time*100.f);
      fprintf(stderr, " FPS: %.2f\n", ((double)frames_done)/wall_time);
    }
  }

  goto done;

exit_failure:
  retval = EXIT_FAILURE;

done:
  // deallocate structures
  if (enc) api->encoder_close(enc);
  bitstream_finalize(&output_stream);
  if (cfg) api->config_destroy(cfg);

  // close files
  if (input)  fclose(input);
  if (output) fclose(output);
  if (recout) fclose(recout);

  CHECKPOINTS_FINALIZE();

  return retval;
}
