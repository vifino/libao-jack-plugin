/*
 *  ao_jack.c
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

#define JACK_PCM_NEW_HW_PARAMS_API
#define JACK_PCM_NEW_SW_PARAMS_API

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include <ao/ao.h>
#include <ao/plugin.h>

#include <jack/jack.h>
#include <jack/types.h>
#include <jack/ringbuffer.h>

#include "ao_jack_resample.h"

#define MAX_PORT_NAME_LEN (6 + 8)

#define INPUT_BUFFER_SIZE (10 * 1024 * sizeof(float))

#define CLIENT_NAME "aojack"

typedef jack_default_audio_sample_t sample_t;

#define aojdebug(format, args...) do { fprintf(stderr,"ao_jack debug: " format,## args); } while(0 == 1)

static char *ao_jack_options[] = {
        "client_name",
	"dev",
        "debug",
	"id",
        "matrix",
        "ports",
        "quality",
        "quiet",
        "verbose",
};


static ao_info ao_jack_info = {
	AO_TYPE_LIVE,
	"JACK Audio Connection Kit output",
	"jack",
	"Laurent Pelecq <lpelecq-org@circoise.eu>",
	"Outputs to the JACK Audio Connection Kit version 0.x",
	AO_FMT_LITTLE,
	50,
	ao_jack_options,
	sizeof(ao_jack_options)/sizeof(*ao_jack_options)
};


typedef struct ao_jack_internal
{
	jack_client_t *client;
	char *client_name;

	int input_rate;
	int output_rate;
	unsigned long quality;

	size_t bits;

	size_t nports;
	char **port_names;
	jack_port_t **output_ports;
	jack_ringbuffer_t **input_channels;

	aojack_resampler_t *resampler;

	/* synchronization when the input buffer is full */
	pthread_mutex_t input_mutex;
	pthread_cond_t input_cond;
} ao_jack_internal;


static int jack_shutdown = 0;

/**
 * Called by JACK on error
 */
static void on_jack_error(const char *msg)
{
	fprintf(stderr,"JACK ERROR: %s", msg);
}

/**
 * Called by JACK on shutdown
 */
static void on_jack_shutdown(void *arg)
{
	jack_shutdown = 1;
}

/**
 * Convert a comma separated list of values in an array of strings
 */
static char **parse_comma_separated_option(char *option)
{
	size_t allocated = 1;
	size_t size = 0;
	char *input = option;
	char *save_p = NULL;
	char *value = NULL;
	char **result = malloc(allocated * sizeof(const char *));
	do {
		value = strtok_r(input, ",", &save_p);
		if (size >= allocated) {
			size_t new_allocated = allocated * 2;
			result = realloc(result, new_allocated * sizeof(const char *));
			if (!result)
				abort();
			allocated = new_allocated;
		}
		if (value)
			value = strdup(value);
		result[size] = value;
		input = NULL;
		size++;
	} while (value != NULL);
	return result;
}

/**
 * Free arrays allocated by `parse_comma_separated_option'
 */
static void free_string_array(char **array)
{
	if (array) {
		char **p;
		for (p = array; *p; p++) {
			free(*p);
		}
		free(array);
	}
}

/************************************************************
 * Device control
 */

/**
 * Called by jack when the output rate change
 */
static int on_sample_rate_update(jack_nframes_t new_rate, void *arg)
{
	ao_jack_internal *internal = (ao_jack_internal*)arg;
	internal->output_rate = new_rate;
	if (internal->resampler)
		aojack_change_resampler_rate(internal->resampler, new_rate);
	return 0;
}

/**
 * Close and release all resources allocated to open the client
 */
static void close_internal(ao_jack_internal *internal)
{
	if (internal->client) {
		jack_client_t *client = internal->client;
		size_t i;
		if (internal->output_ports) {
			for (i = 0; i < internal->nports; i++) {
				jack_port_unregister(client, internal->output_ports[i]);
			}
			free(internal->output_ports);
			internal->output_ports = NULL;
		}
		if (internal->input_channels) {
			for (i = 0; i < internal->nports; i++) {
				jack_ringbuffer_free(internal->input_channels[i]);
			}
			free(internal->input_channels);
			internal->input_channels = NULL;
		}
		internal->client = NULL;
		jack_deactivate(client);
		jack_client_close(client);
	}
}


/************************************************************
 * Frame processing
 */

static void array_uint8_to_float(const char *src, float *dest, size_t nvalues)
{
	const char *p;
	size_t i;
	for (i=0, p = src; i < nvalues; i++, p++)
		dest[i] = (float)(*p) / 128.0f;
}

static void array_uint16_to_float(const char *src, float *dest, size_t nvalues)
{
	const sint_16 *p;
	size_t i;
	for (i=0, p = (sint_16*)src; i < nvalues; i++, p++)
		dest[i] = (float)(*p) / 32768.0f;
}

static float int24_to_float(sint_32 val)
{
	static const sint_32 max24 = 4194304UL;
	if (val >= max24)
		val = -((val - 1) ^ 0xFFFFFF);
	return (float)val/4194304.0f;
}

static void array_uint24_to_float(const char *src, float *dest, size_t nvalues)
{
	const sint_32 *p;
	const char *q;
	size_t i;
	size_t nvalues4 = nvalues / 4 * 4;
	for (i=0, p = (sint_32*)src; i < nvalues4; i+=4, p+=4) {
		sint_32 a = *p;
		sint_32 b = *(p+1);
		sint_32 c = *(p+2);
		dest[i] = int24_to_float((a & 0xFFFFFF00) >> 8);
		dest[i+1] = int24_to_float(((a & 0xFF) << 16) + ((b & 0xFFFF0000) >> 16));
		dest[i+2] = int24_to_float(((b & 0xFFFF) << 16) + ((c & 0xFF000000) >> 24));
		dest[i+2] = int24_to_float(c & 0xFFFFFF);
	}
	q = (const char *)p;
	if (nvalues4 + 1 <= nvalues) {
		dest[i] = int24_to_float((((uint_32)*q) << 16) & (((uint_32)*(q+1)) << 8) & ((uint_32)*(q+2)));
		if (nvalues4 + 2 <= nvalues) {
			dest[i+1] = int24_to_float((((uint_32)*(q+3)) << 16) & (((uint_32)*(q+4)) << 8) & ((uint_32)*(q+5)));
			if (nvalues4 + 3 <= nvalues) {
				dest[i+2] = int24_to_float((((uint_32)*(q+6)) << 16) & (((uint_32)*(q+7)) << 8) & ((uint_32)*(q+8)));
			}
		}
	}
}

static void array_uint32_to_float(const char *src, float *dest, size_t nvalues)
{
	const sint_32 *p;
	size_t i;
	for (i=0, p = (sint_32*)src; i < nvalues; i++, p++)
		dest[i] = (float)(*p) / 2147483648.0f;
}

/**
 * Called by jack to get samples
 */
static int on_jack_hungry(jack_nframes_t nframes, void *arg)
{
	ao_jack_internal *internal = (ao_jack_internal*)arg;
	pthread_mutex_t *mutex_p = &(internal->input_mutex);
	pthread_cond_t *cond_p = &(internal->input_cond);
	if (nframes > 0) {
		jack_ringbuffer_t **input_channels = internal->input_channels;
		size_t i;
		for (i = 0; i < internal->nports; i++) {
			sample_t *out = (sample_t *) jack_port_get_buffer(internal->output_ports[i], nframes);
			size_t available_bytes = jack_ringbuffer_read_space(input_channels[i]);
			size_t available_frames = available_bytes / sizeof(sample_t);
			size_t possible_bytes = available_bytes;
			size_t read_bytes, read_frames;
			if (available_frames > nframes) {
				possible_bytes = nframes * sizeof(sample_t);
			}

			read_bytes = jack_ringbuffer_read (input_channels[i], (char *)out, possible_bytes);
			read_frames = read_bytes / sizeof(sample_t);

			/* Filling the remaining frames with silence */
			if (read_frames < nframes) {
				size_t j;
				for (j = read_frames; j < nframes; j++)
					out[j] = 0.0f;
			}
		}
	}
	if (pthread_mutex_lock(mutex_p) == 0) {
		/* signal waiting producer thread */
		pthread_cond_signal(cond_p);
		pthread_mutex_unlock(mutex_p);
	}
	return 0;
}

/**
 * Write each channel one after the other in the destination buffer
 */
static void deinterleave_frames(int nchannels, size_t nframes, float *source, float *destination)
{
	size_t c, f;
	size_t i = 0;
	for (f = 0; f < nframes; f++) {
		for (c = 0; c < nchannels; c++) {
			destination[c * nframes + f] = source[i];
			i++;
		}
	}
}

/**
 * Write each channel in its input buffer to be fetched by JACK
 */
static int write_deinterleaved_frames(ao_jack_internal *internal, size_t nchannels, size_t nframes, float *data)
{
	size_t channel_size = nframes * sizeof(float);
	size_t nbytes_by_channel = channel_size;
	jack_ringbuffer_t **input_channels = internal->input_channels;
	size_t pos = 0;

	while (nbytes_by_channel > 0 && !jack_shutdown) {
		size_t i;
		size_t available = jack_ringbuffer_write_space(input_channels[0]);
		for (i = 1; i < nchannels; i++) {
			size_t available2 = jack_ringbuffer_write_space(input_channels[i]);
			if (available2 < available)
				available = available2;
		}
		if (available > nbytes_by_channel)
			available = nbytes_by_channel;

		if (available > 0) {
			const char *start = (const char *)data + pos;
			size_t written = 0;
			for (i = 0; i < nchannels; i++) {
				size_t written2 = jack_ringbuffer_write(input_channels[i], start, available);
				if (i == 0)
					written = written2;
				else if (written2 != written) {
					return -1;
				}
				start += channel_size;
			}
			pos += written;
			nbytes_by_channel -= written;
		} else { /* buffer is full */
			pthread_mutex_t *mutex_p = &(internal->input_mutex);
			pthread_cond_t *cond_p = &(internal->input_cond);
			if (pthread_mutex_lock(mutex_p) == 0) {
				/* wait for consumer thread */
				pthread_cond_wait(cond_p, mutex_p);
				pthread_mutex_unlock(mutex_p);
			} else {
				return -1;
			}
		}
	}
	return 0;
}

/**
 * Callback for processing incoming frames
 *
 * Deinterleave frames, resample them if needed and send them to JACK
 */
static int on_frames_available(size_t nchannels, size_t nframes, float *interleaved_data, void *arg)
{
	int status = 0;
	ao_jack_internal *internal = (ao_jack_internal*)arg;
	float *data = interleaved_data;
	float *deinterleaved_data = NULL;

	if (nchannels > 1) {
		data = deinterleaved_data = malloc(nchannels * nframes * sizeof(float));
		deinterleave_frames(nchannels, nframes, interleaved_data, data);
	}

	status = write_deinterleaved_frames(internal, nchannels, nframes, data);

	if (deinterleaved_data)
		free(deinterleaved_data);
	return status;
}

/************************************************************
 * Plugin interface
 */

/**
 * Test if plugin can run
 */
int ao_plugin_test()
{
	jack_status_t status;
	jack_options_t options = JackNoStartServer;
	jack_client_t *client = jack_client_open(CLIENT_NAME, options, &status, NULL);
	if (client == NULL) {
		return 0;
	}
	jack_client_close(client);
	return 1;
}


/**
 * Return the address of the driver info structure
 */
ao_info *ao_plugin_driver_info(void)
{
	return &ao_jack_info;
}


/**
 * Initialize internal data structures
 */
int ao_plugin_device_init(ao_device *device)
{
	ao_jack_internal *internal = (ao_jack_internal *) calloc(1,sizeof(ao_jack_internal));

	if (internal == NULL)
		return 0;

	jack_set_error_function(on_jack_error);

	internal->client = NULL;
	internal->client_name = strdup(CLIENT_NAME);
	internal->quality = 5;
	pthread_mutex_init(&(internal->input_mutex), NULL);
	pthread_cond_init(&(internal->input_cond), NULL);

	device->internal = internal;
        device->output_matrix = strdup("L,R,BL,BR,C,LFE,SL,SR");
        device->output_matrix_order = AO_OUTPUT_MATRIX_PERMUTABLE;
	device->driver_byte_format = AO_FMT_LITTLE;

	return 1;
}


/**
 * Pass application parameters regarding the sound device
 */
int ao_plugin_set_option(ao_device *device, const char *key, const char *value)
{
	ao_jack_internal *internal = (ao_jack_internal *) device->internal;

	if (strcmp(key, "client_name") == 0) {
		free(internal->client_name);
		internal->client_name = strdup(value);
	} else if (strcmp(key, "dev") == 0) {
		/* ignore */
	} else if (strcmp(key, "id") == 0) {
		/* ignore */
	} else if (strcmp(key, "ports") == 0) {
		char *writable_value = strdup(value);
		free_string_array(internal->port_names);
		internal->port_names = parse_comma_separated_option(writable_value);
		free(writable_value);
	} else if (strcmp(key, "quality") == 0) {
		internal->quality = strtoul(value, NULL, 10);
	} else
		return 0;

	return 1;
}


/**
 * Prepare the audio device for playback
 */
int ao_plugin_open(ao_device *device, ao_sample_format *format)
{
	int status = 0;
	const char **p;
	size_t i;
	const char **port_names = NULL;
	const char **physical_port_names = NULL;
	jack_client_t *client = NULL;
	jack_status_t jack_status;
	jack_options_t options = JackNoStartServer;
	size_t nreqports = 0;

	ao_jack_internal *internal  = (ao_jack_internal *) device->internal;

	client = internal->client = jack_client_open(internal->client_name, options, &jack_status, NULL);
	if (client == NULL) {
		aerror("%s: cannot open jack client, status = 0x%2.0x\n", internal->client_name, jack_status);
		return 0;
	}

	internal->input_rate = format->rate;
	internal->output_rate = jack_get_sample_rate(client);
	internal->bits = format->bits;
	internal->resampler = aojack_new_resampler(device->output_channels, internal->input_rate, internal->output_rate, internal->quality, on_frames_available, internal);
	if (internal->resampler == NULL) {
		internal->client = NULL;
		jack_client_close(client);
		aerror("%s: cannot create the sample rate converter\n", internal->client_name);
		return 0;
	}
	adebug("from %d to %d Hz (%lu)\n", internal->input_rate, internal->output_rate, internal->bits);

	jack_shutdown = 0;
	jack_on_shutdown(client, on_jack_shutdown, NULL);

	/* activate the client */
	jack_set_process_callback(client, on_jack_hungry, internal);
	jack_set_sample_rate_callback(client, on_sample_rate_update, internal);
	status = jack_activate(client);
	if (status != 0) {
		internal->client = NULL;
		jack_client_close(client);
		aerror("%s: cannot activate client\n", internal->client_name);
		return 0;
	}

	/* connect ports */
	if (internal->port_names == NULL) {
		port_names = physical_port_names = jack_get_ports(client, "system:*", NULL, JackPortIsPhysical|JackPortIsInput);
		if (physical_port_names == NULL) {
			aerror("%s: cannot find any physical playback ports\n", internal->client_name);
			status = -1;
		}
	} else {
		port_names = (const char **)internal->port_names;
	}

	for (p = port_names, nreqports=0; *p; ++p, nreqports++); /* count number of ports */

	internal->output_ports = calloc(nreqports, sizeof(jack_port_t *));
	internal->input_channels = calloc(nreqports, sizeof(jack_ringbuffer_t *));
	if (internal->output_ports == NULL || internal->input_channels == NULL) {
		status = -1;
	} else {
		for (i = 0; i < nreqports; i++) {
			char name[MAX_PORT_NAME_LEN+1];
			snprintf(name, MAX_PORT_NAME_LEN, "output%lu", i);
			internal->output_ports[i] = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
			internal->input_channels[i] = jack_ringbuffer_create(INPUT_BUFFER_SIZE);
		}

		for (i = 0; status == 0 && i < nreqports; i++) {
			const char *port_name = jack_port_name(internal->output_ports[i]);
			adebug("connecting %s to %s\n", port_name, port_names[i]);
			status = jack_connect(client, port_name, port_names[i]);
			if (status == EEXIST) {
				aerror("%s: port %s is already connected\n", internal->client_name, port_name);
			} else if (status != 0) {
				aerror("%s: can't connect port %s (code: %d)\n", internal->client_name, port_name, status);
			}
		}
		internal->nports = nreqports;
	}

	if (physical_port_names)
		jack_free(physical_port_names);

	if (status != 0) {
		close_internal(internal);
		return 0;
	}

	return 1;
}


/**
 * play num_bytes of audio data
 */
int ao_plugin_play(ao_device *device, const char *output_samples, uint_32 num_bytes)
{
	ao_jack_internal *internal = (ao_jack_internal*)device->internal;
	size_t nchannels = device->output_channels;
	size_t nvalues = (num_bytes * 8) / internal->bits;
	size_t nframes = nvalues / nchannels;
	int status = 0;
	float *data = NULL;
	size_t data_size = nchannels * nframes * sizeof(float);

	if (jack_shutdown) {
		aerror("%s: jack is stopped\n", internal->client_name);
		return 0;
	} else if (nchannels > internal->nports) {
		aerror("%s: %lu: too many channels, maximum is %lu\n", internal->client_name, nchannels, internal->nports);
		return 0 ;
	} else {
		/* We must not write more bytes that the input buffer can contain. Otherwise it is
		 * not possible to resample the frames while jack is consuming the previous chunk.
		 * We estimate the number of frames the input buffer can hold according to the
		 * convertion ratio. And we write half this size to always be able to convert
		 * some frames while the rest is played. */
		size_t max_input_frames = (INPUT_BUFFER_SIZE / (nchannels * sizeof(float)) * internal->input_rate) / internal->output_rate / 2;
		size_t i;

		data = (float*)malloc(data_size);
		if (internal->bits == 8) {
			array_uint8_to_float(output_samples, data, nvalues);
		} else if (internal->bits == 16) {
			array_uint16_to_float(output_samples, data, nvalues);
		} else if (internal->bits == 24) {
			array_uint24_to_float(output_samples, data, nvalues);
		} else if (internal->bits == 32) {
			array_uint32_to_float(output_samples, data, nvalues);
		}

		for (i = 0; i < nframes && status == 0; i += max_input_frames) {
			size_t partial_nframes = max_input_frames;
			float *partial_data = data + (i * nchannels);
			if (i + max_input_frames > nframes)
				partial_nframes = nframes - i;
			status = aojack_resample_frames(internal->resampler, partial_nframes, partial_data);
		}
		free(data);
	}
	return (status == 0 ? 1 : 0);
}


/**
 * Close the audio device
 */
int ao_plugin_close(ao_device *device)
{
	ao_jack_internal *internal;

	if (device) {
		if ((internal = (ao_jack_internal *) device->internal)) {
			close_internal(internal);
		} else
			awarn("ao_plugin_close called with uninitialized ao_device->internal\n");
	} else
		awarn("ao_plugin_close called with uninitialized ao_device\n");

	return 1;
}


/**
 * Free the internal data structures
 */
void ao_plugin_device_clear(ao_device *device)
{
	ao_jack_internal *internal;

	if (device) {
		if ((internal = (ao_jack_internal *) device->internal)) {
			free(internal->client_name);
			free_string_array(internal->port_names);
			pthread_mutex_destroy(&(internal->input_mutex));
			pthread_cond_destroy(&(internal->input_cond));
			free(internal);
			device->internal = NULL;
		} else
			awarn("ao_plugin_device_clear called with uninitialized ao_device->internal\n");
	} else
		awarn("ao_plugin_device_clear called with uninitialized ao_device\n");
}

/***  Local Variables:		***/
/***  mode: c  			***/
/***  c-basic-offset: 8		***/
/***  indent-tabs-mode: t  	***/
/***  End:  			***/
