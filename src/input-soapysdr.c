/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>              // fprintf()
#include <stdint.h>
#include <stdlib.h>             // atof()
#include <string.h>             // strcmp()
#include <unistd.h>             // usleep()
#include <SoapySDR/Version.h>   // SOAPY_SDR_API_VERSION
#include <SoapySDR/Types.h>     // SoapySDRKwargs_*
#include <SoapySDR/Device.h>    // SoapySDRStream, SoapySDRDevice_*
#include <SoapySDR/Formats.h>   // SoapySDR_formatToSize()
#include "globals.h"            // do_exit
#include "block.h"              // block_*
#include "input-common.h"       // input, sample_format, input_vtable
#include "input-helpers.h"      // get_sample_full_scale_value, get_sample_size
#include "util.h"               // XCALLOC, XFREE, container_of, HZ_TO_KHZ

struct soapysdr_input {
	struct input input;
	SoapySDRDevice *sdr;
	SoapySDRStream *stream;
};

struct input *soapysdr_input_create(struct input_cfg *cfg) {
	UNUSED(cfg);
	NEW(struct soapysdr_input, soapysdr_input);
	return &soapysdr_input->input;
}

static void soapysdr_verbose_device_search() {
	size_t length;
	// enumerate devices
	SoapySDRKwargs *results = SoapySDRDevice_enumerate(NULL, &length);
	for(size_t i = 0; i < length; i++) {
		fprintf(stderr, "Found device #%d:\n", (int32_t)i);
		for(size_t j = 0; j < results[i].size; j++) {
			fprintf(stderr, "  %s = %s\n", results[i].keys[j], results[i].vals[j]);
		}
	}
	SoapySDRKwargsList_clear(results, length);
}

struct sample_format_search_result {
	sample_format sfmt;
	char *soapy_sfmt;
	float full_scale;
	size_t sample_size;
};

struct sample_format_search_result soapysdr_choose_sample_format(SoapySDRDevice *sdr, char const *source) {
	struct sample_format_search_result result = {0};
	result.sfmt = SFMT_UNDEF;
// First try device's native format to avoid extra conversion
	double fullscale = 0.0;
	char *fmt = SoapySDRDevice_getNativeStreamFormat(sdr, SOAPY_SDR_RX, 0, &fullscale);

	if((result.sfmt = sample_format_from_string(fmt)) != SFMT_UNDEF &&
			(result.sample_size = SoapySDR_formatToSize(fmt)) == get_sample_size(result.sfmt) &&
			(result.full_scale = fullscale) > 0.0) {
		fprintf(stderr, "%s: using native sample format %s (full_scale: %.3f)\n", source, fmt,
				result.full_scale);
		result.soapy_sfmt = fmt;
		return result;
	}
// Native format is not supported directly; find out if there is anything else.
	size_t len = 0;
	char **formats = SoapySDRDevice_getStreamFormats(sdr, SOAPY_SDR_RX, 0, &len);
	if(formats == NULL || len == 0) {
		fprintf(stderr, "%s: failed to read supported sample formats\n", source);
		result.sfmt = SFMT_UNDEF;
		return result;
	}
	for(size_t i = 0; i < len; i++) {
		if((result.sfmt = sample_format_from_string(formats[i])) != SFMT_UNDEF &&
			(result.sample_size = SoapySDR_formatToSize(formats[i])) == get_sample_size(result.sfmt)) {
			result.full_scale = get_sample_full_scale_value(result.sfmt);
			result.soapy_sfmt = formats[i];
			fprintf(stderr, "%s: using non-native sample format %s (assuming full_scale=%.3f)\n",
				source, formats[i], result.full_scale);
			break;
		}
	}
	return result;
}

int32_t soapysdr_input_init(struct input *input) {
	ASSERT(input != NULL);
	struct soapysdr_input *soapysdr_input = container_of(input, struct soapysdr_input, input);
	soapysdr_verbose_device_search();

	struct input_cfg *cfg = input->config;
	SoapySDRDevice *sdr = SoapySDRDevice_makeStrArgs(cfg->source);
	if(sdr == NULL) {
		fprintf(stderr, "%s: could not open SoapySDR device: %s\n", cfg->source, SoapySDRDevice_lastError());
		return -1;
	}
	if(SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, 0, cfg->sample_rate) != 0) {
		fprintf(stderr, "%s: setSampleRate failed: %s\n", cfg->source, SoapySDRDevice_lastError());
		return -1;
	}
	if(SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, 0, cfg->centerfreq + cfg->freq_offset, NULL) != 0) {
		fprintf(stderr, "%s: setFrequency failed: %s\n", cfg->source, SoapySDRDevice_lastError());
		return -1;
	}
	fprintf(stderr, "%s: center frequency set to %.3f kHz\n", cfg->source,
			HZ_TO_KHZ(cfg->centerfreq + cfg->freq_offset));
	if(SoapySDRDevice_setFrequencyCorrection(sdr, SOAPY_SDR_RX, 0, cfg->correction) != 0) {
		fprintf(stderr, "%s: setFrequencyCorrection failed: %s\n", cfg->source, SoapySDRDevice_lastError());
		return -1;
	}
	fprintf(stderr, "%s: frequency correction set to %.2f ppm\n", cfg->source, cfg->correction);
	if(SoapySDRDevice_hasDCOffsetMode(sdr, SOAPY_SDR_RX, 0)) {
		if(SoapySDRDevice_setDCOffsetMode(sdr, SOAPY_SDR_RX, 0, true) != 0) {
			fprintf(stderr, "%s: setDCOffsetMode failed: %s\n", cfg->source, SoapySDRDevice_lastError());
			return -1;
		}
	}

	// If both --gain and --soapy-gain are present, the latter takes precedence.
	// If neither is present, auto gain is enabled.
	if(cfg->gain_elements != NULL) {
		SoapySDRKwargs gains = SoapySDRKwargs_fromString(cfg->gain_elements);
		if(gains.size < 1) {
			fprintf(stderr, "Unable to parse gains string, "
					"must be a sequence of 'name1=value1,name2=value2,...'.\n");
			return -1;
		}
		for(size_t i = 0; i < gains.size; i++) {
			SoapySDRDevice_setGainElement(sdr, SOAPY_SDR_RX, 0, gains.keys[i], atof(gains.vals[i]));
			double gain_value = SoapySDRDevice_getGainElement(sdr, SOAPY_SDR_RX, 0, gains.keys[i]);
			fprintf(stderr, "%s: gain element %s set to %.2f dB\n", cfg->source, gains.keys[i], gain_value);

		}
		SoapySDRKwargs_clear(&gains);
	} else if(cfg->gain != AUTO_GAIN) {
		if(SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, cfg->gain) != 0) {
			fprintf(stderr, "%s: could not set gain: %s\n", cfg->source, SoapySDRDevice_lastError());
			return -1;
		}
		fprintf(stderr, "%s: gain set to %.2f dB\n", cfg->source, cfg->gain);
	} else {
		if(SoapySDRDevice_hasGainMode(sdr, SOAPY_SDR_RX, 0) == false) {
			fprintf(stderr, "%s: device does not support auto gain. "
					"Please specify gain manually.\n", cfg->source);
			return -1;
		}
		if(SoapySDRDevice_setGainMode(sdr, SOAPY_SDR_RX, 0, true) != 0) {
			fprintf(stderr, "%s: could not enable auto gain: %s\n", cfg->source, SoapySDRDevice_lastError());
			return -1;
		}
		fprintf(stderr, "%s: auto gain enabled\n", cfg->source);
	}
	if(cfg->antenna != NULL) {
		if(SoapySDRDevice_setAntenna(sdr, SOAPY_SDR_RX, 0, cfg->antenna) != 0) {
			fprintf(stderr, "could not select antenna %s: %s\n", cfg->antenna, SoapySDRDevice_lastError());
			return -1;
		}
	}
	fprintf(stderr, "%s: using antenna %s\n", cfg->source, SoapySDRDevice_getAntenna(sdr, SOAPY_SDR_RX, 0));

	if(cfg->device_settings != NULL) {
		SoapySDRKwargs settings_param = SoapySDRKwargs_fromString(cfg->device_settings);
		if(settings_param.size < 1) {
			fprintf(stderr, "%s: unable to parse --device-settings argument '%s' "
					"(must be a sequence of 'name1=value1,name2=value2,...')\n",
					cfg->source, cfg->device_settings);
			return -1;
		}
		for(size_t i = 0; i < settings_param.size; i++) {
			SoapySDRDevice_writeSetting(sdr, settings_param.keys[i], settings_param.vals[i]);
			char *setting_value = SoapySDRDevice_readSetting(sdr, settings_param.keys[i]);
			fprintf(stderr, "%s: setting %s to %s %s\n", cfg->source, settings_param.keys[i],
					setting_value,
					(strcmp(settings_param.vals[i], setting_value) == 0) ? "done" : "failed");
		}
		SoapySDRKwargs_clear(&settings_param);
	}
	struct sample_format_search_result chosen = soapysdr_choose_sample_format(sdr, cfg->source);
	if(chosen.sfmt == SFMT_UNDEF) {
		fprintf(stderr, "%s: could not find a suitable sample format; unable to use this device\n",
				cfg->source);
		return -1;
	}
	cfg->sfmt = chosen.sfmt;
	input->full_scale = chosen.full_scale;
	input->bytes_per_sample = chosen.sample_size;
	debug_print(D_SDR, "%s: sfmt: %d soapy_sfmt: %s full_scale: %.3f sample_size: %d\n",
			cfg->source, cfg->sfmt, chosen.soapy_sfmt, input->full_scale, input->bytes_per_sample);

	SoapySDRStream *stream = NULL;
#if SOAPY_SDR_API_VERSION < 0x00080000
	if(SoapySDRDevice_setupStream(sdr, &stream, SOAPY_SDR_RX, chosen.soapy_sfmt, NULL, 0, NULL) != 0)
#else
	if((stream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_RX, chosen.soapy_sfmt, NULL, 0, NULL)) == NULL)
#endif
	{
		fprintf(stderr, "%s: could not set up stream: %s\n", cfg->source, SoapySDRDevice_lastError());
		return -1;
	}

	input->block.producer.max_tu = SoapySDRDevice_getStreamMTU(sdr, stream);
	soapysdr_input->sdr = sdr;
	soapysdr_input->stream = stream;
	return 0;
}

void soapysdr_input_destroy(struct input *input) {
	if(input != NULL) {
		struct soapysdr_input *si = container_of(input, struct soapysdr_input, input);
		XFREE(si);
	}
}

#define SOAPYSDR_READSTREAM_TIMEOUT_US 1000000L

void *soapysdr_input_thread(void *ctx) {
	ASSERT(ctx);
	struct block *block = ctx;
	struct input *input = container_of(block, struct input, block);
	struct soapysdr_input *soapysdr_input = container_of(input, struct soapysdr_input, input);
	void *inbuf = XCALLOC(input->block.producer.max_tu, input->bytes_per_sample);
	float complex *outbuf = XCALLOC(input->block.producer.max_tu, sizeof(float complex));
	int32_t ret;
	if((ret = SoapySDRDevice_activateStream(soapysdr_input->sdr, soapysdr_input->stream, 0, 0, 0)) != 0) {
		fprintf(stderr, "Failed to activate stream for SoapySDR device '%s': %s\n",
			input->config->source, SoapySDR_errToStr(ret));
		do_exit = 1;
		goto shutdown;
	}
	usleep(100000);

	while(do_exit == 0) {
		int32_t flags;
		long long timeNs;
		int32_t samples_read = SoapySDRDevice_readStream(soapysdr_input->sdr, soapysdr_input->stream, &inbuf,
			input->block.producer.max_tu, &flags, &timeNs, SOAPYSDR_READSTREAM_TIMEOUT_US);
		if(samples_read < 0) {	// when it's negative, it's the error code
			fprintf(stderr, "SoapySDR device '%s': readStream failed: %s\n",
				input->config->source, SoapySDR_errToStr(samples_read));
			continue;
		}
		input->convert_sample_buffer(input, inbuf, samples_read * input->bytes_per_sample, outbuf);
		complex_samples_produce(&input->block.producer.out->circ_buffer, outbuf, samples_read);
	}
shutdown:
	debug_print(D_MISC, "Shutdown ordered, signaling consumer shutdown\n");
	SoapySDRDevice_deactivateStream(soapysdr_input->sdr, soapysdr_input->stream, 0, 0);
	SoapySDRDevice_closeStream(soapysdr_input->sdr, soapysdr_input->stream);
	SoapySDRDevice_unmake(soapysdr_input->sdr);
	block_connection_one2one_shutdown(block->producer.out);
	block->running = false;
	XFREE(inbuf);
	XFREE(outbuf);
	return NULL;
}

struct input_vtable const soapysdr_input_vtable = {
	.create = soapysdr_input_create,
	.init = soapysdr_input_init,
	.destroy = soapysdr_input_destroy,
	.rx_thread_routine = soapysdr_input_thread
};

