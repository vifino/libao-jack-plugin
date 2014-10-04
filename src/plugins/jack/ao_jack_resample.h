/*
 *  ao_jack_resampler.h
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

#ifndef __INCLUDE_AOJACK_RESAMPLE_H__
#define __INCLUDE_AOJACK_RESAMPLE_H__

struct _aojack_resampler_t;
typedef struct _aojack_resampler_t aojack_resampler_t;

typedef int (*aojack_write_frames_t)(size_t nchannels, size_t nframes, float *data, void *arg);

aojack_resampler_t *aojack_new_resampler(size_t nchannels, int src_rate, int dest_rate, unsigned long quality, aojack_write_frames_t callback, void *arg);

void aojack_delete_resampler(aojack_resampler_t *resampler);

int aojack_resample_frames(aojack_resampler_t *resampler, size_t nframes, float *data);

void aojack_change_resampler_rate(aojack_resampler_t *resampler, int dest_rate);

#endif /* __INCLUDE_AOJACK_RESAMPLE_H__ */
