/*
 *  ao_jack_resampler.c
 *
 *  Copyright (C) 2014  Laurent Pelecq
 *
 *  This file is part of libao, a cross-platform library.  See
 *  README for a history of this source code.
 *
 *  libao is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  libao is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <samplerate.h>

#include "ao_jack_resample.h"

struct _aojack_resampler_t {
	SRC_STATE *state;
	size_t channels;
	int passthrough;
	int src_rate;
	int dest_rate;
	double ratio;
	int quality;
	aojack_write_frames_t callback;
	void *arg;
};

static int quality_levels[] = {
	SRC_LINEAR,
	SRC_ZERO_ORDER_HOLD,
	SRC_SINC_FASTEST,
	SRC_SINC_MEDIUM_QUALITY,
	SRC_SINC_BEST_QUALITY
};

static size_t NUMBER_OF_QUALITY_LEVELS = sizeof(quality_levels) / sizeof(int);

aojack_resampler_t *aojack_new_resampler(size_t nchannels, int src_rate, int dest_rate, unsigned long quality, aojack_write_frames_t callback, void *arg)
{
	aojack_resampler_t *resampler = (aojack_resampler_t*)malloc(sizeof(aojack_resampler_t));
	if (resampler) {
		int error = 0;
		resampler->channels = nchannels;
		resampler->passthrough = (src_rate == dest_rate);
		resampler->src_rate = src_rate;
		aojack_change_resampler_rate(resampler, dest_rate);
		quality = quality * NUMBER_OF_QUALITY_LEVELS / 10;
		if (quality >= NUMBER_OF_QUALITY_LEVELS)
			quality = SRC_SINC_BEST_QUALITY;
		resampler->quality = quality_levels[quality];
		resampler->callback = callback;
		resampler->arg = arg;
		if (!resampler->passthrough) {
			resampler->state = src_new(resampler->quality, nchannels, &error);
			if (resampler->state == NULL) {
				free(resampler);
				return NULL;
			}
		}
	}
	return resampler;
}

void aojack_change_resampler_rate(aojack_resampler_t *resampler, int dest_rate)
{
	resampler->dest_rate = dest_rate;
	resampler->ratio = (double)dest_rate / (double)(resampler->src_rate);
}

void aojack_delete_resampler(aojack_resampler_t *resampler)
{
	if (resampler) {
		if (resampler->state) {
			src_delete(resampler->state);
			resampler->state = NULL;
		}
		free(resampler);
	}
}

int aojack_resample_frames(aojack_resampler_t *resampler, size_t nframes, float *data)
{
	int status = 0;
	size_t nchannels = resampler->channels;
	if (resampler->passthrough) {
		status = resampler->callback(nchannels, nframes, data, resampler->arg);
	} else {
		SRC_DATA resampler_data;
		long remaining_frames = nframes;

		/* Estimating the size of the output frames with a margin of 20%. The convertion should
		 * take place in 1 loop. If it isn't the case, the rest is processed in the next loop. */
		resampler_data.output_frames = nframes * resampler->ratio * 1.2;
		resampler_data.data_out = (float*)malloc(resampler_data.output_frames * nchannels * sizeof(float));
		resampler_data.src_ratio = resampler->ratio;
		resampler_data.end_of_input = 0;
		while (status == 0 && remaining_frames > 0) {
			resampler_data.input_frames = remaining_frames;
			resampler_data.data_in = data;
			status = src_process(resampler->state, &resampler_data);
			if (status == 0) {
				if (resampler->callback)
					status = resampler->callback(nchannels, resampler_data.output_frames_gen, resampler_data.data_out, resampler->arg);
				remaining_frames -= resampler_data.input_frames_used;
				data += (resampler_data.input_frames_used * nchannels);
			}
		}
		free(resampler_data.data_out);
	}
	return status;
}

/***  Local Variables:		***/
/***  mode: c  			***/
/***  c-basic-offset: 8		***/
/***  indent-tabs-mode: t  	***/
/***  End:  		***/
